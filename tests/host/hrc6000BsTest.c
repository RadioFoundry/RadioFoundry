// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C6000 split duplex BS outbound (drivers/modem/hrc6000/hrc6000_bs.c): the takeover of the slot engine, key-up on
// an outbound TS1 slot, per-slot queues in order with idle fill, the PA keyed once and dropped at unkey, the framing
// experiment's register writes and their restore, the embedded LC rewrite, the stall watchdog and the way back to the
// slot engine. Against a fake config port, voice port and RF switch with a fake microsecond clock
#include <stdio.h>
#include <string.h>
#include "modem/hrc6000/hrc6000_bs.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while (0)

typedef struct {
	uint8_t page, reg, n;
	bool read;
	uint8_t data[40];
} xfer_t;

static xfer_t xlog[8192];
static int nx;
static uint8_t rd[8][256];
static int voice_writes;

static bool fake_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	uint8_t page = hdr[0] & 7u, reg = hdr[1];
	bool read = (hdr[0] & 0x80u) != 0;

	if (nx < (int)(sizeof(xlog) / sizeof(xlog[0]))) {
		xfer_t *x = &xlog[nx];

		x->page = page;
		x->reg = reg;
		x->n = (uint8_t)len;
		x->read = read;
		if (!read && tx != NULL) {
			memcpy(x->data, tx, len < sizeof(x->data) ? len : sizeof(x->data));
		}
		nx++;
	}
	if (read && rx != NULL) {
		for (uint16_t i = 0; i < len; i++) {
			rx[i] = rd[page][(uint8_t)(reg + i)];
		}
	}
	return true;
}

static bool fake_voice(void *ctx, bool write, uint8_t *buf, uint8_t len)
{
	voice_writes += write;
	return true;
}

static void no_delay(uint32_t v) { (void)v; }

static const hrc6000_bus_t bus = { fake_transfer, no_delay, no_delay, NULL, fake_voice };
static hrc6000_t dev;
static uint32_t t_us;
static int rf_on, rf_off, locks, unlocks;
static bool rf_ok = true;

static bool fake_rf(void *ctx, bool tx)
{
	if (tx) {
		rf_on++;
	} else {
		rf_off++;
	}
	return rf_ok;
}
static uint32_t fake_now(void *ctx) { return t_us; }
static void fake_lock(void *ctx) { locks++; }
static void fake_unlock(void *ctx) { unlocks++; }
static void fake_notify(void *ctx) { }

static const hrc6000_dmr_ops_t ops = { fake_rf, fake_now, fake_lock, fake_unlock, fake_notify, NULL, NULL };
static hrc6000_dmr_t eng;
static hrc6000_bs_t bs;

static int last_write(uint8_t page, uint8_t reg)
{
	int v = -1;

	for (int i = 0; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == page && xlog[i].reg == reg) {
			v = xlog[i].data[0];
		}
	}
	return v;
}

static int count_writes(uint8_t page, uint8_t reg, int val)
{
	int n = 0;

	for (int i = 0; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == page && xlog[i].reg == reg && (val < 0 || xlog[i].data[0] == val)) {
			n++;
		}
	}
	return n;
}

static void fresh(void)
{
	memset(rd, 0, sizeof(rd));
	t_us = 1000000;
	rf_on = rf_off = 0;
	rf_ok = true;
	hrc6000_dmr_init(&eng, &dev, &ops);
	hrc6000_dmr_run(&eng);
	nx = 0;
}

// One slot boundary 30 ms after the last
static void tick(void)
{
	t_us += 30000;
	hrc6000_dmr_isr_timeslot(&eng);
}

static dmr_tx_burst_t burst(uint8_t kind, uint8_t index, uint8_t fill)
{
	dmr_tx_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.voice_index = index;
	b.len = (kind == DMR_BURST_VOICE) ? DMR_VOICE_BYTES : 12;
	memset(b.payload, fill, b.len);
	return b;
}

static hrc6000_bs_raw_fn start_raw;	// the raw slot builder start() hands over

static void start(uint8_t variant)
{
	dmr_bs_config_t c = { .cc = 1, .variant = variant };

	if (variant == HRC6000_BS_VARIANT_RAW) {
		c.nregs = 2;
		c.regs[0][0] = 0x0D;
		c.regs[0][1] = 0x5A;
		c.regs[1][0] = 0x0E;
		c.regs[1][1] = 0xA5;
	}
	CHECK(hrc6000_bs_start(&bs, &eng, &c, start_raw));
}

static void test_start_stop(void)
{
	fresh();
	dmr_bs_config_t c = { .cc = 3 };

	hrc6000_dmr_halt(&eng);
	CHECK(!hrc6000_bs_start(&bs, &eng, &c, NULL));	// the engine must be running
	fresh();
	CHECK(hrc6000_bs_start(&bs, &eng, &c, NULL));
	CHECK(hrc6000_bs_active(&eng));
	CHECK(!hrc6000_bs_start(&bs, &eng, &c, NULL));	// once
	CHECK(last_write(HRC6000_PAGE_SYS, 0x40) == 0xE3);
	CHECK(last_write(HRC6000_PAGE_SYS, 0x1F) == 0x30);
	CHECK(last_write(HRC6000_PAGE_SYS, 0x41) == 0x00);
	CHECK(last_write(HRC6000_PAGE_AUX, 0x04) >= 0);	// the BS sync pattern
	CHECK(locks == unlocks);
	CHECK(!hrc6000_dmr_is_transmitting(&eng));	// started, not keyed
	CHECK(hrc6000_bs_space(&eng, 1) == HRC6000_BS_Q && hrc6000_bs_space(&eng, 2) == HRC6000_BS_Q);

	// Unkeyed: slots tick, nothing set up, the receive path is not run (no reads of the status registers)
	nx = 0;
	for (int i = 0; i < 4; i++) {
		tick();
	}
	CHECK(bs.st.slots == 4 && bs.st.bursts == 0);
	CHECK(count_writes(HRC6000_PAGE_SYS, 0x41, 0x80) == 0);
	uint32_t cnt, t;
	int8_t ts;

	CHECK(hrc6000_bs_slot_clock(&eng, &cnt, &t, &ts));
	CHECK(cnt == eng.tick && t == t_us && (ts == 1 || ts == 2));

	// Stop: MS sync, passive timing, back to the slot engine
	nx = 0;
	hrc6000_bs_stop(&eng);
	CHECK(!hrc6000_bs_active(&eng));
	CHECK(count_writes(HRC6000_PAGE_SYS, 0x40, 0xC3) >= 1);
	CHECK(eng.sync_cur == DMR_SYNC_MS);
	CHECK(rf_off == 0);
	CHECK(locks == unlocks);
	tick();		// the slot engine's own handling again
	CHECK(bs.st.slots == 4);
}

static void test_keyup(void)
{
	fresh();
	start(0);
	tick();
	tick();
	int8_t first_next;

	// Keyed: the first slot set up is an outbound TS1 slot; nothing queued, idle bursts (underruns)
	hrc6000_bs_key(&eng, true);
	CHECK(hrc6000_dmr_is_transmitting(&eng));
	nx = 0;
	tick();
	first_next = (bs.ts_tick == 2) ? 1 : 2;
	if (first_next != 1) {
		CHECK(bs.st.bursts == 0);	// waited
		tick();
	}
	CHECK(bs.st.bursts == 1 && bs.on_air);
	CHECK(bs.st.underruns == 1);
	CHECK(last_write(HRC6000_PAGE_SYS, 0x41) == 0x80);
	CHECK(last_write(HRC6000_PAGE_SYS, 0x50) == hrc6000_dmr_reg50(DMR_BURST_IDLE, 0));
	CHECK(count_writes(HRC6000_PAGE_DATA, 0x18, -1) == 1);	// the idle message
	CHECK(bs.st.keyups == 1);

	// PA: keyed once at the first RF TX interrupt, never again while on air
	hrc6000_dmr_isr_rftx(&eng);
	hrc6000_dmr_isr_rftx(&eng);
	CHECK(rf_on == 1 && hrc6000_bs_keyed(&eng));
	CHECK(eng.pa_on);

	// Each outbound slot its own queue, in order
	dmr_tx_burst_t h1 = burst(DMR_BURST_VOICE_LC_HEADER, 0, 0x11), h2 = burst(DMR_BURST_VOICE_LC_HEADER, 0, 0x22);
	dmr_tx_burst_t v1 = burst(DMR_BURST_VOICE, 0, 0x33), v2 = burst(DMR_BURST_VOICE, 1, 0x44);
	static const uint8_t lc[9] = { 0, 0, 0, 0, 0, 9, 0, 0, 1 };

	CHECK(hrc6000_bs_queue(&eng, 2, &h2, NULL, NULL));
	CHECK(hrc6000_bs_queue(&eng, 1, &h1, NULL, NULL));
	CHECK(hrc6000_bs_queue(&eng, 1, &v1, NULL, lc));
	CHECK(hrc6000_bs_queue(&eng, 1, &v2, NULL, NULL));
	CHECK(hrc6000_bs_space(&eng, 1) == HRC6000_BS_Q - 3);
	CHECK(!hrc6000_bs_queue(&eng, 1, &v2, NULL, NULL));	// full
	CHECK(hrc6000_bs_space(&eng, 2) == HRC6000_BS_Q - 1);

	uint32_t under = bs.st.underruns;
	uint8_t want50[6], got50[6];
	int n = 0;
	int8_t order[6];

	// The slot just set up was TS1 (idle): next comes TS2 (h2), TS1 (h1), TS2 (idle), TS1 (v1), TS2 (idle), TS1 (v2)
	for (int i = 0; i < 6; i++) {
		nx = 0;
		tick();
		order[n] = (bs.ts_tick == 1) ? 2 : 1;
		got50[n] = (uint8_t)last_write(HRC6000_PAGE_SYS, 0x50);
		n++;
	}
	want50[0] = hrc6000_dmr_reg50(DMR_BURST_VOICE_LC_HEADER, 0);
	want50[1] = hrc6000_dmr_reg50(DMR_BURST_VOICE_LC_HEADER, 0);
	want50[2] = hrc6000_dmr_reg50(DMR_BURST_IDLE, 0);
	want50[3] = hrc6000_dmr_reg50(DMR_BURST_VOICE, 0);
	want50[4] = hrc6000_dmr_reg50(DMR_BURST_IDLE, 0);
	want50[5] = hrc6000_dmr_reg50(DMR_BURST_VOICE, 1);
	for (int i = 0; i < 6; i++) {
		CHECK(order[i] == ((i & 1) ? 1 : 2));
		CHECK(got50[i] == want50[i]);
	}
	CHECK(bs.st.underruns == under + 2);
	CHECK(voice_writes >= 2);

	// Unkey: queues dropped, PA off at the next slot boundary, not set up any more
	CHECK(hrc6000_bs_queue(&eng, 2, &h2, NULL, NULL));
	hrc6000_bs_key(&eng, false);
	CHECK(hrc6000_bs_space(&eng, 2) == HRC6000_BS_Q);
	CHECK(rf_off == 0);
	nx = 0;
	tick();
	CHECK(rf_off == 1 && !hrc6000_bs_keyed(&eng) && !eng.pa_on);
	CHECK(last_write(HRC6000_PAGE_SYS, 0x41) == 0x00);
	CHECK(!hrc6000_dmr_is_transmitting(&eng));
	hrc6000_dmr_isr_rftx(&eng);
	CHECK(rf_on == 1);	// unkeyed: no PA
	hrc6000_bs_stop(&eng);
	CHECK(rf_off == 1);
}

// Page 2 0x00 holds the embedded LC for one slot: after the other slot's header it goes back before the next voice
static void test_embedded_lc(void)
{
	fresh();
	start(0);
	hrc6000_bs_key(&eng, true);
	do {
		tick();
	} while (!bs.on_air);
	static const uint8_t lc1[9] = { 0, 0, 0, 0, 0, 1, 0, 0, 7 };
	dmr_tx_burst_t a = burst(DMR_BURST_VOICE, 0, 0x55), b = burst(DMR_BURST_VOICE, 1, 0x56);
	dmr_tx_burst_t h2 = burst(DMR_BURST_VOICE_LC_HEADER, 0, 0x22);

	// Next slot is TS2: h2; then TS1 a (LC written), TS2 h2 (page 2 overwritten), TS1 b (LC written again)
	CHECK(hrc6000_bs_queue(&eng, 2, &h2, NULL, NULL));
	CHECK(hrc6000_bs_queue(&eng, 2, &h2, NULL, NULL));
	CHECK(hrc6000_bs_queue(&eng, 1, &a, NULL, lc1));
	CHECK(hrc6000_bs_queue(&eng, 1, &b, NULL, NULL));
	nx = 0;
	for (int i = 0; i < 4; i++) {
		tick();
	}
	CHECK(bs.st.page2_lc == 2);
	int lc_writes = 0;

	for (int i = 0; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == HRC6000_PAGE_DATA && xlog[i].reg == 0x00 && xlog[i].n == 12 &&
		    memcmp(xlog[i].data, lc1, 9) == 0) {
			lc_writes++;
		}
	}
	CHECK(lc_writes == 2);
	hrc6000_bs_stop(&eng);
}

// The framing experiment: the variant's values at key-up, the operation values back at unkey
static void test_variants(void)
{
	static const struct { uint8_t v, r10, r11; } want[] = {
		{ 0, 0x6E, 0x80 }, { 1, 0x6E, 0x00 }, { 2, 0x4E, 0x00 }, { 3, 0x6A, 0x00 }, { 4, 0x6E, 0x40 },
		{ 5, 0x6E, 0x40 },
	};

	for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
		fresh();
		start(want[k].v);
		hrc6000_bs_key(&eng, true);
		nx = 0;
		do {
			tick();
		} while (!bs.on_air);
		CHECK(last_write(HRC6000_PAGE_SYS, 0x10) == want[k].r10);
		CHECK(last_write(HRC6000_PAGE_SYS, 0x11) == want[k].r11);
		hrc6000_bs_key(&eng, false);
		nx = 0;
		tick();
		CHECK(last_write(HRC6000_PAGE_SYS, 0x10) == 0x6E);
		CHECK(last_write(HRC6000_PAGE_SYS, 0x11) == 0x80);
		hrc6000_bs_stop(&eng);
	}
	fresh();
	start(HRC6000_BS_VARIANT_RAW);
	hrc6000_bs_key(&eng, true);
	nx = 0;
	do {
		tick();
	} while (!bs.on_air);
	CHECK(last_write(HRC6000_PAGE_SYS, 0x0D) == 0x5A);
	CHECK(last_write(HRC6000_PAGE_SYS, 0x0E) == 0xA5);
	hrc6000_bs_stop(&eng);
	CHECK(last_write(HRC6000_PAGE_SYS, 0x10) == 0x6E && last_write(HRC6000_PAGE_SYS, 0x11) == 0x80);
}

// The core's CACH reaches the chip: AT / TC in 0x52 every slot, the short LC (28 bits + CRC-8) in page 2 0x24 at the
// first fragment; variant 5 also puts the LCSS in 0x50 bits 1-0
static void test_cach(void)
{
	for (uint8_t variant = 0; variant <= 5; variant += 5) {
		fresh();
		start(variant);
		static const uint8_t lcss[4] = { 1, 3, 3, 2 };
		for (int i = 0; i < 4; i++) {
			dmr_tx_burst_t b = burst(DMR_BURST_IDLE, 0, 0);
			dmr_cach_t c = { .at = (uint8_t)(i & 1), .tc = 0, .lcss = lcss[i] };
			if (i == 0) {
				c.slc = 0x0ABCDEF1u;
				c.slc_crc = 0x5A;
			}
			CHECK(hrc6000_bs_queue(&eng, 1, &b, &c, NULL));
			dmr_tx_burst_t b2 = burst(DMR_BURST_IDLE, 0, 0);
			dmr_cach_t c2 = { .at = 1, .tc = 1, .lcss = lcss[i] };
			if (i < 3) {
				CHECK(hrc6000_bs_queue(&eng, 2, &b2, &c2, NULL));
			}
			if (i == 2) {
				break;	// the queue holds three per slot
			}
		}
		hrc6000_bs_key(&eng, true);
		nx = 0;
		uint8_t r52[8];
		int n52 = 0, slc_at = -1, lcss50 = 0;
		for (int k = 0; k < 8; k++) {
			tick();
		}
		for (int i = 0; i < nx; i++) {
			if (xlog[i].read) {
				continue;
			}
			if (xlog[i].page == HRC6000_PAGE_SYS && xlog[i].reg == 0x52 && n52 < 8) {
				r52[n52++] = xlog[i].data[0];
			}
			if (xlog[i].page == HRC6000_PAGE_DATA && xlog[i].reg == 0x24 && slc_at < 0) {
				slc_at = i;
			}
			if (xlog[i].page == HRC6000_PAGE_SYS && xlog[i].reg == 0x50) {
				lcss50 |= xlog[i].data[0] & 3;
			}
		}
		// TS1 first: AT 0 TC 0, then TS2 AT 1 TC 1, TS1 AT 1 TC 0, TS2 AT 1 TC 1, TS1 AT 0 TC 0
		CHECK(n52 >= 5 && r52[0] == 0x00 && r52[1] == 0x0C && r52[2] == 0x08 && r52[3] == 0x0C && r52[4] == 0x00);
		CHECK(slc_at >= 0 && xlog[slc_at].n == 5 && xlog[slc_at].data[0] == 0xAB && xlog[slc_at].data[1] == 0xCD &&
		      xlog[slc_at].data[2] == 0xEF && xlog[slc_at].data[3] == 0x10 && xlog[slc_at].data[4] == 0x5A);
		CHECK((lcss50 != 0) == (variant == 5));
		CHECK(bs.st.cach_written == 6 && bs.st.cach_given == 6);
		hrc6000_bs_stop(&eng);
	}
}

static int raw_built;

static void fake_raw(const dmr_tx_burst_t *b, const dmr_cach_t *cach, uint8_t cc, uint8_t *out)
{
	memset(out, 0, HRC6000_BS_RAW_BYTES);
	out[0] = (uint8_t)(0xA0u | ((cach != NULL) ? cach->tc : 0x0Fu));
	out[35] = (uint8_t)(b->kind ^ cc);
	raw_built++;
}

// Variants 6 and 7: every slot is the builder's 36 bytes in page 2 0x00, nothing written to 0x52; 6 puts the chip in
// the physical layer mode (0x10 bits 4-3 = 00), 7 sets Decode_Mode 00 (0x40 bits 1-0); both restored at unkey
static void test_raw_slot(void)
{
	for (uint8_t variant = 6; variant <= 7; variant++) {
		fresh();
		start_raw = fake_raw;
		start(variant);
		start_raw = NULL;
		raw_built = 0;
		dmr_tx_burst_t b = burst(DMR_BURST_CSBK, 0, 0x11);
		dmr_cach_t c = { .tc = 1 };
		CHECK(hrc6000_bs_queue(&eng, 2, &b, &c, NULL));
		hrc6000_bs_key(&eng, true);
		nx = 0;
		do {
			tick();
		} while (!bs.on_air);
		CHECK(last_write(HRC6000_PAGE_SYS, 0x10) == ((variant == 6) ? 0x66 : 0x6E));
		CHECK(last_write(HRC6000_PAGE_SYS, 0x40) == ((variant == 7) ? 0xE0 : 0xE3));
		tick();
		int raws = 0, last = -1;
		for (int i = 0; i < nx; i++) {
			if (!xlog[i].read && xlog[i].page == HRC6000_PAGE_DATA && xlog[i].reg == 0x00) {
				CHECK(xlog[i].n == HRC6000_BS_RAW_BYTES);
				raws++;
				last = i;
			}
		}
		// TS1's underrun (no CACH) then TS2's CSBK with its CACH
		CHECK(raws == 2 && raw_built == 2 && bs.st.raw == 2);
		CHECK(last >= 0 && xlog[last].data[0] == 0xA1 && xlog[last].data[35] == (DMR_BURST_CSBK ^ 1));
		CHECK(count_writes(HRC6000_PAGE_SYS, 0x52, -1) == 0);
		hrc6000_bs_key(&eng, false);
		nx = 0;
		tick();
		CHECK(last_write(HRC6000_PAGE_SYS, 0x10) == 0x6E && last_write(HRC6000_PAGE_SYS, 0x40) == 0xE3);
		hrc6000_bs_stop(&eng);
	}
}

// Variants 8-12: variant 6 with every frame also at a second place
static void test_raw_mirror(void)
{
	static const struct { uint8_t page, addr; } want[5] = {
		{ HRC6000_PAGE_DATA, 0x24 }, { HRC6000_PAGE_DATA, 0x30 }, { HRC6000_PAGE_DATA, 0x48 },
		{ HRC6000_PAGE_DATA, 0x60 }, { 6, 0x00 },
	};

	for (uint8_t variant = 8; variant <= 12; variant++) {
		fresh();
		start_raw = fake_raw;
		start(variant);
		start_raw = NULL;
		hrc6000_bs_key(&eng, true);
		nx = 0;
		do {
			tick();
		} while (!bs.on_air);
		tick();
		CHECK(last_write(HRC6000_PAGE_SYS, 0x10) == 0x66);
		int at0 = 0, at2 = 0;
		for (int i = 0; i < nx; i++) {
			if (xlog[i].read || xlog[i].n != HRC6000_BS_RAW_BYTES) {
				continue;
			}
			at0 += (xlog[i].page == HRC6000_PAGE_DATA) && (xlog[i].reg == 0x00);
			at2 += (xlog[i].page == want[variant - 8].page) && (xlog[i].reg == want[variant - 8].addr);
		}
		CHECK(at0 == 2 && at2 == 2);
		hrc6000_bs_stop(&eng);
	}
}

// No timeslot interrupt for 200 ms while keyed: unkeyed, PA off; halt while the outbound runs ends it too
static void test_stall_and_halt(void)
{
	fresh();
	start(0);
	hrc6000_bs_key(&eng, true);
	do {
		tick();
	} while (!bs.on_air);
	hrc6000_dmr_isr_rftx(&eng);
	CHECK(eng.pa_on);
	t_us += 100000;
	hrc6000_dmr_poll(&eng);
	CHECK(eng.pa_on);
	t_us += 150000;
	hrc6000_dmr_poll(&eng);
	CHECK(!eng.pa_on && rf_off == 1 && bs.st.stalls == 1 && !bs.keyed);
	CHECK(hrc6000_bs_active(&eng));	// still in charge, unkeyed (the core sees bs_keyed false)

	fresh();
	start(0);
	hrc6000_bs_key(&eng, true);
	do {
		tick();
	} while (!bs.on_air);
	hrc6000_dmr_isr_rftx(&eng);
	hrc6000_dmr_halt(&eng);
	CHECK(!hrc6000_bs_active(&eng) && !eng.pa_on && rf_off == 1);
	CHECK(locks == unlocks);

	// The PA key the board could not do is tried again at the next RF TX interrupt
	fresh();
	start(0);
	hrc6000_bs_key(&eng, true);
	do {
		tick();
	} while (!bs.on_air);
	rf_ok = false;
	hrc6000_dmr_isr_rftx(&eng);
	CHECK(!eng.pa_on && bs.st.rf_fail == 1);
	rf_ok = true;
	hrc6000_dmr_isr_rftx(&eng);
	CHECK(eng.pa_on && rf_on == 2);
	hrc6000_bs_stop(&eng);
}

// Every place of each slot's ring holds the burst queued there, through many wraps: the ring once had a place
// without storage, and every fourth TS2 burst went out as a zeroed PI header (HIL 2026-10-11)
static void test_ring_wrap(void)
{
	fresh();
	start(0);
	tick();
	tick();
	hrc6000_bs_key(&eng, true);
	while (!bs.on_air) {
		tick();
	}
	dmr_tx_burst_t c = burst(DMR_BURST_CSBK, 0, 0x5A);
	int bad = 0;
	uint32_t under = bs.st.underruns;

	for (int i = 0; i < 4 * (HRC6000_BS_Q + 1) * 3; i++) {
		CHECK(hrc6000_bs_queue(&eng, bs.ts_tick, &c, NULL, NULL));	// the next tick sets up this slot again
		nx = 0;
		tick();
		if (last_write(HRC6000_PAGE_SYS, 0x50) != hrc6000_dmr_reg50(DMR_BURST_CSBK, 0)) {
			bad++;
		}
	}
	CHECK(bad == 0);
	CHECK(bs.st.underruns == under);
	hrc6000_bs_stop(&eng);
}

int main(void)
{
	hrc6000_attach(&dev, &bus);
	test_start_stop();
	test_keyup();
	test_embedded_lc();
	test_ring_wrap();
	test_variants();
	test_cach();
	test_raw_slot();
	test_raw_mirror();
	test_stall_and_halt();
	printf("# hrc6000BsTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
