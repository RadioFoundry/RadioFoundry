// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C6000 physical layer pipe (drivers/modem/hrc_common/hrc6000_air.c): fit, the two record layouts against golden
// vectors and across chunk boundaries, soft scaling, the sync flag, gaps and the correlator re-arm, transmit halves per
// slot interrupt with no stale half, underrun idle, last-chunk padding, the deviation pair set and put back, open /
// close / open, the slot engine starting again on the memory the session used, a cheap fuzz, and the FM copy: its fit,
// set-up, halves in order with their times, a missed interrupt, an overrun, the tap, and the registers put back.
// Against a chip simulator: a register file, the receive record RAM, the FM receive RAM, the two TX RAM halves and a
// fake microsecond clock; the soft modem is a stub that keeps the samples it is given
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modem/hrc_common/hrc6000_air.h"
#include "modem/hrc6000/hrc6000.h"
#include "fuzz.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while (0)

// --- Chip simulator ---

typedef struct {
	uint8_t page;
	uint16_t addr;
	uint16_t n;
	bool read;
	uint8_t data[40];
} xfer_t;

static xfer_t xlog[16384];
static int nx;
static uint8_t regs[8][256];		// what a read returns
static uint8_t wr[8][256];		// the last value written
static uint8_t record[HRC6000_AIR_RAW_BYTES];
static uint8_t tx_ram[0x48];		// page 2 0x00-0x47: the two halves
static uint8_t copy_ram[4 * HRC6000_AIR_COPY_HALF];	// page 2 from 0x30 in FM: the two receive halves
static bool fm_ram;			// page 2 0x30 reads give the FM receive RAM, not a record
static int half_writes[2];

static bool fake_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	uint8_t page = hdr[0] & 7u;
	bool read = (hdr[0] & 0x80u) != 0;
	bool ext = (hdr[0] & 0x40u) != 0;
	uint16_t addr = (uint16_t)(hdr[1] | (ext ? (hdr[2] << 8) : 0));

	(void)ctx;
	CHECK(hdr_len == (ext ? 3 : 2));
	if (nx < (int)(sizeof(xlog) / sizeof(xlog[0]))) {
		xfer_t *x = &xlog[nx++];
		x->page = page;
		x->addr = addr;
		x->n = len;
		x->read = read;
		if (!read && tx != NULL) {
			memcpy(x->data, tx, len < sizeof(x->data) ? len : sizeof(x->data));
		}
	}
	if (read) {
		if (page == 2 && fm_ram) {
			CHECK(addr >= 0x30 && addr - 0x30u + len <= sizeof(copy_ram));
			memcpy(rx, &copy_ram[addr - 0x30u], len);
		} else if (page == 2 && addr == 0x30) {
			CHECK(len == HRC6000_AIR_RAW_BYTES);
			memcpy(rx, record, len);
		} else {
			for (uint16_t i = 0; i < len; i++) {
				rx[i] = regs[page][(uint8_t)(addr + i)];
			}
		}
		return true;
	}
	if (page == 2 && addr + len <= sizeof(tx_ram)) {
		memcpy(&tx_ram[addr], tx, len);
		if (len == HRC6000_AIR_HALF_BYTES && (addr == 0x00 || addr == 0x24)) {
			half_writes[addr == 0x24]++;
		}
	}
	for (uint16_t i = 0; i < len && addr + i < 256; i++) {
		wr[page][addr + i] = tx[i];
	}
	if (page == 4 && len == 1 && (addr == 0x45 || addr == 0x46)) {
		regs[4][addr] = tx[0];	// the pair reads back
	}
	return true;
}

static bool fake_voice(void *ctx, bool write, uint8_t *buf, uint8_t len) { return true; }
static void no_delay(uint32_t v) { (void)v; }

static const hrc_bus_t bus = { fake_transfer, no_delay, no_delay, NULL, fake_voice };
static hrc_t dev;
static uint32_t t_us;
static int rf_on, rf_off;
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
static int lock_depth, max_depth;
static void fake_lock(void *ctx) { if (++lock_depth > max_depth) max_depth = lock_depth; }
static void fake_unlock(void *ctx) { lock_depth--; }
static void fake_notify(void *ctx) { }

static const hrc6000_dmr_ops_t ops = { fake_rf, fake_now, fake_lock, fake_unlock, fake_notify, NULL, NULL, NULL };

// The session's memory is the engine's event queue, as on the radio
static hrc6000_dmr_t eng;
typedef struct __attribute__((may_alias)) {
	hrc6000_air_t a;
	hrc6000_air_buf_t b;
} air_mem_t;
#define AIR	(&((air_mem_t *)(void *)eng.evq)->a)
#define BUF	(&((air_mem_t *)(void *)eng.evq)->b)

// --- The stub soft modem: the samples it is given, each call's time, where it was reset ---

#define SM_MAX	8192
static air_softmodem_t sm_mem;
static struct {
	int16_t s[SM_MAX];
	int n, calls, opens, resets;
	uint32_t t[64];
	int reset_at[16];
	bool refuse;
} smg;

uint32_t air_softmodem_rx_fit(const air_profile_t *p, const hal_baseband_t *bb)
{
	return p->sym_rate * 5u <= bb->rx_rate_hz * 3u ? AIR_FIT_RX | AIR_FIT_SOFT : 0;
}

bool air_softmodem_rx_open(air_softmodem_t *m, const air_profile_t *p, uint32_t rate_hz, uint16_t counts_per_khz,
			   air_softmodem_out_t out, void *ctx)
{
	(void)counts_per_khz;
	memset(m, 0, sizeof(*m));
	m->p = p;
	m->out = out;
	m->ctx = ctx;
	smg.opens++;
	return !smg.refuse && rate_hz == HRC6000_AIR_COPY_HZ;
}

void air_softmodem_rx_reset(air_softmodem_t *m)
{
	(void)m;
	if (smg.resets < 16) {
		smg.reset_at[smg.resets] = smg.n;
	}
	smg.resets++;
}

// One chunk out per call, so the path to the session's callback is checked too
void air_softmodem_rx(air_softmodem_t *m, const int16_t *samples, uint16_t n, uint32_t at_us)
{
	CHECK(n == HRC6000_AIR_COPY_HALF);
	if (smg.calls < 64) {
		smg.t[smg.calls] = at_us;
	}
	smg.calls++;
	for (uint16_t i = 0; i < n && smg.n < SM_MAX; i++) {
		smg.s[smg.n++] = samples[i];
	}
	static const uint8_t hard[1] = { 0 };
	static const int8_t soft[1] = { 0 };
	air_chunk_t c = { .t_us = at_us, .nsyms = 1, .sync_at = -1, .hard = hard, .soft = soft };
	m->out(&c, m->ctx);
}

// --- Events ---

#define MAXSYMS 8192
static struct {
	int n_rx, n_started, n_underrun, n_done, n_space;
	uint32_t t_started, t_done;
	uint8_t flags[64];
	int16_t sync_at[64];
	uint8_t sync_id[64];
	uint32_t t[64];
	uint16_t n[64];
	uint8_t dibit[MAXSYMS];
	int8_t soft[MAXSYMS];
	int nsym;
} got;

static void on_event(const air_event_t *ev, void *ctx)
{
	(void)ctx;
	switch (ev->kind) {
	case AIR_EV_RX: {
		const air_chunk_t *c = ev->chunk;
		CHECK(c != NULL && c->nsyms >= 1 && c->nsyms <= HRC6000_AIR_SYMS && c->soft != NULL && ev->t_us == c->t_us);
		if (got.n_rx < 64) {
			got.n[got.n_rx] = c->nsyms;
			got.flags[got.n_rx] = c->flags;
			got.sync_at[got.n_rx] = c->sync_at;
			got.sync_id[got.n_rx] = c->sync_id;
			got.t[got.n_rx] = c->t_us;
		}
		for (int i = 0; i < c->nsyms && got.nsym < MAXSYMS; i++) {
			got.dibit[got.nsym] = (uint8_t)((c->hard[i / 4] >> (6 - 2 * (i % 4))) & 3u);
			got.soft[got.nsym] = c->soft[i];
			got.nsym++;
		}
		got.n_rx++;
		break;
	}
	case AIR_EV_TX_STARTED: got.n_started++; got.t_started = ev->t_us; break;
	case AIR_EV_TX_UNDERRUN: got.n_underrun++; break;
	case AIR_EV_TX_SPACE: got.n_space++; break;
	case AIR_EV_TX_DONE: got.n_done++; got.t_done = ev->t_us; break;
	default: break;
	}
}

// --- Profiles ---

static const uint8_t P25_SYNC[6] = { 0x55, 0x75, 0xF5, 0xFF, 0x77, 0xFF };

static air_profile_t p25(void)
{
	air_profile_t p;
	memset(&p, 0, sizeof(p));
	p.sym_rate = 4800;
	p.levels = 4;
	p.shape = AIR_SHAPE_C4FM;
	p.shape_param = 20;
	p.dev_outer_hz = 1800;
	p.timing = AIR_TIMING_CONTINUOUS;
	p.sync_every_syms = 864;
	p.idle = 0x5F;	// arbitrary but recognizable: 01 01 11 11
	p.nsyncs = 2;
	p.sync[0].id = 9;
	p.sync[0].nsyms = 12;	// a short one first: never the correlator's
	p.sync[1].id = 7;
	p.sync[1].nsyms = 24;
	p.sync[1].max_errs = 2;
	memcpy(p.sync[1].syms, P25_SYNC, 6);
	return p;
}

static void fresh(void)
{
	memset(regs, 0, sizeof(regs));
	memset(wr, 0, sizeof(wr));
	memset(&got, 0, sizeof(got));
	memset(tx_ram, 0, sizeof(tx_ram));
	memset(copy_ram, 0, sizeof(copy_ram));
	fm_ram = false;
	half_writes[0] = half_writes[1] = 0;
	regs[4][0x45] = 0x2A;	// a DMR calibration pair (1944 Hz outer)
	regs[4][0x46] = 0x52;
	t_us = 1000000;
	rf_on = rf_off = 0;
	rf_ok = true;
	hrc6000_attach(&dev, &bus);
	hrc6000_dmr_init(&eng, &dev, &ops);
	nx = 0;
}

static int last_write(uint8_t page, uint16_t addr)
{
	int v = -1;
	for (int i = 0; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == page && xlog[i].addr == addr) {
			v = xlog[i].data[0];
		}
	}
	return v;
}

static int count_writes(uint8_t page, uint16_t addr, int val)
{
	int n = 0;
	for (int i = 0; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == page && xlog[i].addr == addr && (val < 0 || xlog[i].data[0] == val)) {
			n++;
		}
	}
	return n;
}

// --- Receive stimulus: an air symbol stream turned into the chip's records ---

static uint8_t air[MAXSYMS];		// dibits in air order
static const int8_t LEVEL[4] = { 7, 20, -7, -20 };	// soft counts per dibit (+1, +3, -1, -3) at 1800 Hz
static const int8_t LEVEL_HOT[4] = { 9, 28, -9, -28 };	// a receiver with more counts per kHz
static const int8_t *level = LEVEL;
static const int8_t DC = -3;

// Chip order for the record that starts at air symbol off: the inverse of the layout. One with a sync detection has
// the correlator's pattern where the chip puts it
static void make_record_at(int off, bool synced, int8_t dc)
{
	static const uint8_t LS[][2] = { { 0, 12 }, { 46, 49 }, { 12, 5 }, { 22, 24 }, { 17, 5 }, { 95, 49 } };
	static const uint8_t LP[][2] = { { 0, 12 }, { 36, 54 }, { 12, 24 }, { 90, 54 } };
	const uint8_t (*seg)[2] = synced ? LS : LP;
	int nseg = synced ? 6 : 4, j = 0;

	for (int i = 0; synced && i < 24; i++) {
		air[off + HRC6000_AIR_SYNC_AT + i] = (uint8_t)((P25_SYNC[i / 4] >> (6 - 2 * (i % 4))) & 3u);
	}
	for (int k = 0; k < nseg; k++) {
		for (int i = seg[k][0]; i < seg[k][0] + seg[k][1]; i++) {
			int v = level[air[off + j]] + dc;
			record[2 * i] = (uint8_t)((v & 0x3F) | 0x80);	// bits 7-6 aren't part of the value
			record[2 * i + 1] = (uint8_t)((v > 14 || v < -14) ? 0x20 : 0x00);
			j++;
		}
	}
}

static void make_record(int r, bool synced, int8_t dc)
{
	make_record_at(r * 144, synced, dc);
}

// One record: the timeslot interrupt that ends it (dt_us after the last), the system interrupt 2.42 ms later, the task
static void deliver_at(int off, bool synced, uint32_t dt_us)
{
	t_us += dt_us;
	hrc6000_air_isr_timeslot(AIR);
	make_record_at(off, synced, DC);
	regs[4][0x82] = 0x01;
	regs[4][0x51] = synced ? 0x02 : 0x00;
	t_us += 2420;
	hrc6000_air_isr_sys(AIR);
	hrc6000_air_poll(AIR);
	t_us -= 2420;	// the clock stays monotonic: the next boundary is 30 ms after this one
}

static void deliver(int r, bool synced, uint32_t gap_us)
{
	deliver_at(r * 144, synced, 30000 + gap_us);
}

static void rand_stream(uint32_t seed)
{
	for (int i = 0; i < MAXSYMS; i++) {
		seed = seed * 1103515245u + 12345u;
		air[i] = (uint8_t)((seed >> 16) & 3u);
	}
}

// --- Tests ---

static void test_fit(void)
{
	air_profile_t p = p25();
	uint32_t f = hrc6000_air_fit(&p);
	CHECK(f == (AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT | AIR_FIT_NEEDS_SYNC | AIR_FIT_SHAPE_APPROX));
	CHECK((f & AIR_FIT_UNTESTED) == 0);	// the board adds it

	air_profile_t q = p;
	q.shape = AIR_SHAPE_RRC;
	CHECK((hrc6000_air_fit(&q) & AIR_FIT_SHAPE_APPROX) == 0);
	q = p;
	q.sym_rate = 2400;
	CHECK(hrc6000_air_fit(&q) == (AIR_FIT_TX | AIR_FIT_RATE_DOUBLED | AIR_FIT_SHAPE_APPROX));
	q = p;
	q.sym_rate = 1200;
	CHECK(hrc6000_air_fit(&q) == 0);
	q = p;
	q.timing = AIR_TIMING_TDMA;
	CHECK(hrc6000_air_fit(&q) == 0);
	q = p;
	q.sync[1].flags = AIR_SYNC_START_ONLY;
	CHECK((hrc6000_air_fit(&q) & AIR_FIT_RX) == 0 && (hrc6000_air_fit(&q) & AIR_FIT_TX) != 0);
	q = p;
	q.sync[1].syms[0] = 0x45;	// an inner symbol in the pattern
	CHECK((hrc6000_air_fit(&q) & AIR_FIT_RX) == 0);
	q = p;
	q.sync_every_syms = 0;
	CHECK((hrc6000_air_fit(&q) & AIR_FIT_RX) == 0);
	q = p;
	q.sync_every_syms = 1728 + 144;
	CHECK((hrc6000_air_fit(&q) & AIR_FIT_RX) == 0);
	q = p;
	q.dev_outer_hz = 1050;
	CHECK((hrc6000_air_fit(&q) & (AIR_FIT_RX | AIR_FIT_DEV_APPROX)) == 0 && (hrc6000_air_fit(&q) & AIR_FIT_TX));
	q = p;
	q.dev_outer_hz = 4000;
	CHECK((hrc6000_air_fit(&q) & AIR_FIT_DEV_APPROX) != 0);
	q = p;
	q.levels = 3;
	CHECK(hrc6000_air_fit(&q) == 0);
	CHECK(hrc6000_air_fit(NULL) == 0);

	// The pair: both calibration values scaled by the target over 1944 Hz; C4FM aims at 1800 / 0.903
	uint8_t m2, m1;
	q = p;
	q.shape = AIR_SHAPE_RRC;
	hrc6000_air_mod_pair(&q, 0x2A, 0x52, &m2, &m1);
	CHECK(m1 == 0x4C && m2 == 0x27);	// 1800 Hz from 0x52 / 0x2A
	q.dev_outer_hz = 2400;
	hrc6000_air_mod_pair(&q, 0x2A, 0x52, &m2, &m1);
	CHECK(m1 == 0x65 && m2 == 0x34);
	hrc6000_air_mod_pair(&p, 0x2A, 0x52, &m2, &m1);
	CHECK(m1 == 0x54 && m2 == 0x2B);	// C4FM 1800: 1993 Hz at the chip
}

static void test_golden(void)
{
	uint8_t raw[HRC6000_AIR_RAW_BYTES], hard[36];
	int8_t soft[144];

	// Symbol i of the chip's record carries level (i % 60) - 30 (odd bytes are the slice and are ignored)
	for (int i = 0; i < 144; i++) {
		raw[2 * i] = (uint8_t)(((i % 60) - 30) & 0x3F);
		raw[2 * i + 1] = 0xFF;
	}
	int16_t outer16 = 16 * 30;	// 30 counts = AIR_SOFT_OUTER: soft = level * 96 / 30
	hrc6000_air_unpack(raw, true, 0, 0, outer16, 4, hard, soft);
	// With a sync: chip 0-11, 46-94, 12-16, 22-45 (the sync, at air 66), 17-21, 95-143
	static const int sync_map[][2] = { { 0, 0 }, { 11, 11 }, { 12, 46 }, { 60, 94 }, { 61, 12 }, { 65, 16 },
					   { 66, 22 }, { 89, 45 }, { 90, 17 }, { 94, 21 }, { 95, 95 }, { 143, 143 } };
	for (unsigned k = 0; k < sizeof(sync_map) / sizeof(sync_map[0]); k++) {
		int lv = (sync_map[k][1] % 60) - 30;
		CHECK(soft[sync_map[k][0]] == (lv * 96 / 30 > 127 ? 127 : lv * 96 / 30 < -127 ? -127 : lv * 96 / 30));
	}
	static const int plain_map[][2] = { { 0, 0 }, { 11, 11 }, { 12, 36 }, { 65, 89 }, { 66, 12 }, { 89, 35 },
					    { 90, 90 }, { 143, 143 } };
	hrc6000_air_unpack(raw, false, 0, 0, outer16, 4, hard, soft);
	for (unsigned k = 0; k < sizeof(plain_map) / sizeof(plain_map[0]); k++) {
		int lv = (plain_map[k][1] % 60) - 30;
		CHECK(soft[plain_map[k][0]] == lv * 96 / 30);
	}
	// Slicing at 0 and two thirds of the outer level; two levels slice by sign
	for (int i = 0; i < 144; i++) {
		raw[2 * i] = (uint8_t)((i % 4 == 0 ? 30 : i % 4 == 1 ? 9 : i % 4 == 2 ? -9 : -30) & 0x3F);
	}
	int16_t mean = hrc6000_air_unpack(raw, false, 0, 0, outer16, 4, hard, soft);
	CHECK(mean == 0);
	CHECK(hard[0] == 0x4B);	// 01 00 10 11
	CHECK(soft[0] == 96 && soft[1] == 28 && soft[2] == -28 && soft[3] == -96);
	hrc6000_air_unpack(raw, false, 0, 0, outer16, 2, hard, soft);
	CHECK(hard[0] == 0x5F);	// 01 01 11 11
	// The offset comes off before the scaling
	hrc6000_air_unpack(raw, false, 0, 16 * 10, outer16, 4, hard, soft);
	CHECK(soft[0] == 64 && soft[3] == -127);
	// Without its first symbols: the rest moves up, and the mean is still the whole record's
	int8_t all[144];
	uint8_t hard_all[36];
	for (int i = 0; i < 144; i++) {
		raw[2 * i] = (uint8_t)(((i * 7) % 41 - 20) & 0x3F);
	}
	int16_t m_all = hrc6000_air_unpack(raw, true, 0, 0, outer16, 4, hard_all, all);
	CHECK(hrc6000_air_unpack(raw, true, 17, 0, outer16, 4, hard, soft) == m_all);
	CHECK(memcmp(soft, all + 17, 144 - 17) == 0);
	for (int i = 0; i < 144 - 17; i++) {
		CHECK(((hard[i / 4] >> (6 - 2 * (i % 4))) & 3) == ((hard_all[(i + 17) / 4] >> (6 - 2 * ((i + 17) % 4))) & 3));
	}
	// The outer level from a sync: chip symbols 22-45 hold the pattern; +3 at 25, -3 at -21 counts
	for (int i = 0; i < 24; i++) {
		bool plus = ((P25_SYNC[i / 4] >> (6 - 2 * (i % 4))) & 3) == 1;
		raw[2 * (22 + i)] = (uint8_t)((plus ? 25 : -21) & 0x3F);
	}
	CHECK(hrc6000_air_sync_outer16(raw, P25_SYNC) == 16 * 23);
	static const uint8_t one_sign[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	CHECK(hrc6000_air_sync_outer16(raw, one_sign) == 0);
}

static void test_receive(void)
{
	fresh();
	air_profile_t p = p25();
	hrc6000_air_init(AIR, &dev, &ops, BUF, NULL, NULL);
	nx = 0;
	CHECK(hrc6000_air_open(AIR, &p, on_event, NULL));
	CHECK(lock_depth == 0);
	CHECK(last_write(4, 0x10) == 0x02 && last_write(4, 0x40) == 0x40 && last_write(4, 0x41) == 0x40);
	CHECK(last_write(4, 0x36) == 0x00 && last_write(4, 0x5F) == 0xF0);
	CHECK(memcmp(&wr[1][0x47], P25_SYNC, 6) == 0);	// the 24-symbol one, not the short one listed first

	rand_stream(42);
	// Records 0-11: a sync every 6, as P25's
	for (int r = 0; r < 12; r++) {
		deliver(r, r % 6 == 0, 0);
	}
	CHECK(got.n_rx == 12 && got.nsym == 12 * 144);
	int bad = 0, soft_bad = 0;
	for (int i = 0; i < got.nsym; i++) {
		bad += got.dibit[i] != air[i];
		int want = air[i] == 1 ? 96 : air[i] == 0 ? 32 : air[i] == 2 ? -32 : -96;
		soft_bad += abs(got.soft[i] - want) > 24;
	}
	CHECK(bad == 0);
	CHECK(soft_bad == 0);
	CHECK(got.flags[0] == AIR_CHUNK_GAP);		// the first after open
	CHECK(got.flags[1] == 0 && got.flags[6] == 0);
	CHECK(got.sync_at[0] == 66 && got.sync_id[0] == 7 && got.sync_at[1] == -1 && got.sync_at[6] == 66);
	CHECK(got.t[1] - got.t[0] == 30000);
	CHECK(got.t[0] == 1000000 + 30000 - 30000 + 104);	// the record before its boundary, symbol centers

	// A record lost: 144 erasures stand in for it, so the count of symbols holds and nothing is flagged
	deliver(13, true, 30000);
	CHECK(got.n_rx == 14 && got.n[12] == 144 && got.flags[12] == 0 && got.sync_at[12] == -1);
	CHECK(got.t[12] - got.t[11] == 30000 && got.t[13] - got.t[12] == 30000);
	CHECK(got.flags[13] == 0 && got.sync_at[13] == 66);
	int erased = 0;
	for (int i = 12 * 144; i < 13 * 144; i++) {
		erased += got.soft[i] == 0 && got.dibit[i] == 0;
	}
	CHECK(erased == 144 && memcmp(got.dibit + 13 * 144, air + 13 * 144, 144) == 0);
	CHECK(AIR->st.gaps == 1 && AIR->st.fills == 1);

	// The signal steps: no sync for K = 7 records -> re-arm (0x41 = 0x60), FREE_RUN past the spacing
	nx = 0;
	for (int r = 14; r < 21; r++) {
		deliver(r, false, 0);
	}
	CHECK(got.n_rx == 21);
	CHECK((got.flags[19] & AIR_CHUNK_FREE_RUN) == 0 && (got.flags[20] & AIR_CHUNK_FREE_RUN) != 0);
	CHECK(count_writes(4, 0x41, 0x60) == 1 && AIR->st.rearms == 1);
	// Held 40 ms: records meanwhile are dropped, the poll releases it
	deliver(21, false, 0);
	CHECK(got.n_rx == 21 && count_writes(4, 0x41, 0x40) == 0);
	t_us += 15000;
	hrc6000_air_poll(AIR);
	CHECK(count_writes(4, 0x41, 0x40) == 1);
	// The next sync's record is too far on for erasures: a gap
	deliver(22, true, 0);
	CHECK(got.n_rx == 22 && (got.flags[21] & AIR_CHUNK_GAP) && (got.flags[21] & AIR_CHUNK_FREE_RUN) == 0);

	// rx_restart: the set-up again, the next chunk a gap
	nx = 0;
	hrc6000_air_rx_restart(AIR);
	CHECK(count_writes(4, 0x10, 0x02) == 1 && count_writes(4, 0x41, 0x40) == 1);
	deliver(23, true, 0);
	CHECK(got.flags[22] & AIR_CHUNK_GAP);

	// The task behind: records beyond the queue are dropped and the next delivered one is a gap
	for (int r = 24; r < 24 + HRC6000_AIR_RXQ + 2; r++) {
		t_us += 30000;
		hrc6000_air_isr_timeslot(AIR);
		make_record(r, true, DC);
		regs[4][0x82] = 1;
		regs[4][0x51] = 2;
		hrc6000_air_isr_sys(AIR);
	}
	CHECK(AIR->st.rx_overflow == 2);
	int before = got.n_rx;
	hrc6000_air_poll(AIR);
	CHECK(got.n_rx == before + HRC6000_AIR_RXQ);
	deliver(24 + HRC6000_AIR_RXQ + 2, true, 0);
	CHECK(got.flags[got.n_rx - 1] & AIR_CHUNK_GAP);

	// A system interrupt with no record flag reads nothing more; its flags are cleared
	nx = 0;
	regs[4][0x82] = 0x08;
	hrc6000_air_isr_sys(AIR);
	CHECK(count_writes(4, 0x83, 0x08) == 1 && nx == 2);
	CHECK(lock_depth == 0);
}

// The chip's record clock steps at a sync (10.2), and the receiver's level scale is the board's
static void test_reassembly(void)
{
	fresh();
	hrc6000_dmr_halt(&eng);
	air_profile_t p = p25();
	hrc6000_air_init(AIR, &dev, &ops, BUF, NULL, NULL);
	CHECK(hrc6000_air_open(AIR, &p, on_event, NULL));
	rand_stream(11);
	// A new signal: the record after the first sync's comes 128 symbols later and repeats 16
	deliver_at(0, true, 30000);
	deliver_at(128, false, 26667);
	CHECK(got.n_rx == 2 && got.n[0] == 144 && got.n[1] == 128 && got.flags[1] == 0);
	CHECK(got.t[1] - got.t[0] == 30000);	// the chunk starts where the last one ended
	CHECK(AIR->st.trims == 1);
	for (int r = 2; r < 6; r++) {
		deliver_at(128 + (r - 1) * 144, false, 30000);
	}
	CHECK(got.nsym == 848 && memcmp(got.dibit, air, 848) == 0);
	// The next sync's record comes 160 symbols later: 16 are lost, and 16 erasures stand in for them
	deliver_at(864, true, 33333);
	CHECK(got.n_rx == 8 && got.n[6] == 16 && got.flags[6] == 0 && got.sync_at[6] == -1);
	CHECK(got.t[6] - got.t[5] == 30000 && got.t[7] - got.t[6] == 3333);
	CHECK(got.n[7] == 144 && got.flags[7] == 0 && got.sync_at[7] == 66);
	CHECK(got.soft[848] == 0 && got.soft[863] == 0 && memcmp(got.dibit + 864, air + 864, 144) == 0);
	CHECK(AIR->st.fills == 1 && AIR->st.gaps == 1);
	// In step from here. A boundary a symbol late or early is the interrupt's jitter: nothing is cut or flagged
	deliver_at(1008, false, 30000);
	deliver_at(1152, false, 30180);
	deliver_at(1296, false, 29820);
	deliver_at(1440, false, 30000);
	CHECK(got.n_rx == 12 && got.nsym == 1584 && AIR->st.trims == 1 && AIR->st.gaps == 1 && AIR->st.fills == 1);
	CHECK(memcmp(got.dibit + 864, air + 864, 5 * 144) == 0);
	// A sync record that repeats symbols keeps its sync where it now is
	deliver_at(1584 - 16, true, 26667);
	CHECK(got.n[12] == 128 && got.sync_at[12] == 50 && got.flags[12] == 0);
	CHECK(memcmp(got.dibit + 1584, air + 1584, 128) == 0);
	// A re-arm's hole: the sync's record 268 symbols after the last. 124 erasures, in one chunk
	deliver_at(1568 + 268, true, 55833);
	CHECK(got.n_rx == 15 && got.n[13] == 124 && got.n[14] == 144 && got.flags[14] == 0);
	CHECK(got.nsym == 1980 && memcmp(got.dibit + 1836, air + 1836, 144) == 0);
	// More than a chunk holds: two chunks of erasures
	deliver_at(1980 + 190, false, 30000 + 39583);
	CHECK(got.n_rx == 18 && got.n[15] == 144 && got.n[16] == 46 && got.n[17] == 144);
	CHECK(got.t[16] - got.t[15] == 30000 && got.nsym == 2314 && memcmp(got.dibit + 2170, air + 2170, 144) == 0);
	// Too far early to be a step, or lost past what erasures stand in for: a gap, nothing cut
	deliver_at(3000, false, 15000);
	CHECK(got.n[18] == 144 && (got.flags[18] & AIR_CHUNK_GAP));
	deliver_at(3400, false, 30000 + 41667);
	CHECK(got.n_rx == 20 && got.n[19] == 144 && (got.flags[19] & AIR_CHUNK_GAP));
	hrc6000_air_close(AIR, true);

	// Levels: a receiver with 1.4 times the counts. The first sync's record is scaled by its own sync
	fresh();
	hrc6000_dmr_halt(&eng);
	hrc6000_air_init(AIR, &dev, &ops, BUF, NULL, NULL);
	CHECK(hrc6000_air_open(AIR, &p, on_event, NULL));
	CHECK(AIR->outer16 == 316);	// 1800 Hz at 11 counts per kHz
	rand_stream(12);
	level = LEVEL_HOT;
	deliver(0, true, 0);
	CHECK(AIR->outer16 == 28 * 16);
	deliver(1, false, 0);
	int bad = 0;
	for (int i = 0; i < 288; i++) {
		int want = air[i] == 1 ? 96 : air[i] == 0 ? 31 : air[i] == 2 ? -31 : -96;
		bad += got.dibit[i] != air[i] || abs(got.soft[i] - want) > 12;
	}
	CHECK(bad == 0);
	// In step the scale follows a quarter of the way per sync
	level = LEVEL;
	for (int r = 2; r < 6; r++) {
		deliver(r, false, 0);
	}
	deliver(6, true, 0);
	CHECK(AIR->outer16 == 28 * 16 + (20 * 16 - 28 * 16) / 4);
	// A detection whose symbols are not the pattern's levels (noise) leaves it
	int16_t before = AIR->outer16;
	t_us += 30000;
	hrc6000_air_isr_timeslot(AIR);
	memset(record, 0x80, sizeof(record));	// every level 0
	regs[4][0x82] = 0x01;
	regs[4][0x51] = 0x02;
	t_us += 2420;
	hrc6000_air_isr_sys(AIR);
	hrc6000_air_poll(AIR);
	t_us -= 2420;
	CHECK(AIR->outer16 == before);
	hrc6000_air_close(AIR, true);
	CHECK(lock_depth == 0);
}

static air_tx_chunk_t chunk(uint8_t fill, uint16_t n, uint8_t flags)
{
	air_tx_chunk_t c;
	memset(&c, 0, sizeof(c));
	c.nsyms = n;
	c.flags = flags;
	memset(c.syms, fill, sizeof(c.syms));
	return c;
}

// The chip's rule (modem-hrc6000.md 10.3): slot n + 1 is set up at interrupt n, and the first slot set up reads 0x00
static int half_of_slot(uint32_t n)
{
	return (n & 1) == 1 ? 0 : 1;
}

static bool half_is(int h, uint8_t fill)
{
	for (int i = 0; i < 36; i++) {
		if (tx_ram[h * 0x24 + i] != fill) {
			return false;
		}
	}
	return true;
}

static void open_p25(air_profile_t *p)
{
	fresh();
	*p = p25();
	hrc6000_air_init(AIR, &dev, &ops, BUF, NULL, NULL);
	CHECK(hrc6000_air_open(AIR, p, on_event, NULL));
	nx = 0;
}

static void tick(void)
{
	t_us += 30000;
	hrc6000_air_isr_timeslot(AIR);
	hrc6000_air_poll(AIR);
}

static void test_transmit(void)
{
	air_profile_t p;
	open_p25(&p);

	CHECK(!hrc6000_air_tx_begin(AIR));	// nothing queued
	CHECK(hrc6000_air_tx_space(AIR) == HRC6000_AIR_TXQ);
	air_tx_chunk_t c1 = chunk(0x11, 144, 0), c2 = chunk(0x22, 144, 0), c3 = chunk(0x33, 144, 0);
	CHECK(hrc6000_air_tx_queue(AIR, &c1) && hrc6000_air_tx_queue(AIR, &c2));
	CHECK(hrc6000_air_tx_space(AIR) == HRC6000_AIR_TXQ - 2);
	CHECK(hrc6000_air_tx_begin(AIR));
	CHECK(lock_depth == 0);
	CHECK(last_write(4, 0x10) == 0x66 && last_write(4, 0x40) == 0xE3 && last_write(4, 0x21) == 0x02);
	CHECK(last_write(4, 0x20) == 0x00 && last_write(4, 0x22) == 0x86);	// the chip's own transmission start
	CHECK(last_write(4, 0x46) == 0x54 && last_write(4, 0x45) == 0x2B);	// scaled from 0x52 / 0x2A
	CHECK(half_is(0, 0x5F) && half_is(1, 0x5F));	// both idle until the first chunk's slot
	CHECK(rf_on == 0 && hrc6000_air_tx_active(AIR));
	CHECK(!hrc6000_air_tx_begin(AIR));	// already
	half_writes[0] = half_writes[1] = 0;

	// The RF TX interrupt comes anywhere in slot 1, which is not a transmit slot: no key there
	hrc6000_air_isr_rftx(AIR);
	CHECK(rf_on == 0);

	// Interrupt 1 sets up slot 2 as an idle transmit slot: one half only, the one slot 2 reads; no chunk, no key
	nx = 0;
	tick();
	CHECK(half_writes[half_of_slot(1)] == 1 && half_writes[!half_of_slot(1)] == 0);
	CHECK(half_is(0, 0x5F) && half_is(1, 0x5F));
	CHECK(last_write(4, 0x41) == 0x80);
	CHECK(rf_on == 0 && got.n_started == 0 && got.n_space == 0);
	CHECK(hrc6000_air_tx_space(AIR) == HRC6000_AIR_TXQ - 2);
	hrc6000_air_isr_rftx(AIR);
	CHECK(rf_on == 0);
	// Interrupt 2: slot 2 starts and the PA is keyed, once; chunk 1 goes to the half slot 3 reads
	tick();
	CHECK(rf_on == 1);
	hrc6000_air_isr_rftx(AIR);
	CHECK(rf_on == 1);
	CHECK(half_writes[0] == 1 && half_writes[1] == 1);
	CHECK(half_is(half_of_slot(2), 0x11) && half_is(half_of_slot(1), 0x5F));
	CHECK(last_write(4, 0x41) == 0x80);
	CHECK(got.n_started == 1 && got.t_started == t_us + 30000 + 2200 + 104);
	CHECK(got.n_space == 1);	// a chunk left the queue: the core may fill it again
	// Interrupt 3: the other half gets chunk 2; the half on air now (chunk 1) is not touched
	CHECK(hrc6000_air_tx_queue(AIR, &c3));
	tick();
	CHECK(half_writes[half_of_slot(3)] == 2 && half_writes[half_of_slot(2)] == 1);
	CHECK(half_is(half_of_slot(3), 0x22) && half_is(half_of_slot(2), 0x11));
	tick();
	CHECK(half_is(half_of_slot(4), 0x33) && half_is(half_of_slot(3), 0x22));
	// Underrun: idle, one event however many slots
	tick();
	tick();
	CHECK(half_is(half_of_slot(5), 0x5F) && half_is(half_of_slot(6), 0x5F));
	CHECK(got.n_underrun == 1 && AIR->st.tx_underruns == 2);
	CHECK(got.n_space == 3);	// one per chunk taken, none for an idle slot
	// More comes, then a short last chunk: padded with idle from its last symbol (100 = 25 bytes exactly; 101 cuts one)
	air_tx_chunk_t c4 = chunk(0x44, 144, 0), c5 = chunk(0xAA, 101, 0);
	CHECK(hrc6000_air_tx_queue(AIR, &c4) && hrc6000_air_tx_queue(AIR, &c5));
	hrc6000_air_tx_end(AIR);
	CHECK(hrc6000_air_tx_space(AIR) == 0);
	air_tx_chunk_t late = chunk(0x55, 144, 0);
	CHECK(!hrc6000_air_tx_queue(AIR, &late));	// after the last
	tick();	// 7: c4
	CHECK(half_is(half_of_slot(7), 0x44));
	tick();	// 8: c5, the last
	int h = half_of_slot(8);
	for (int i = 0; i < 25; i++) {
		CHECK(tx_ram[h * 0x24 + i] == 0xAA);
	}
	CHECK(tx_ram[h * 0x24 + 25] == ((0xAA & 0xC0) | (0x5F & 0x3F)));
	for (int i = 26; i < 36; i++) {
		CHECK(tx_ram[h * 0x24 + i] == 0x5F);
	}
	CHECK(got.n_underrun == 1);
	// The last chunk on air (9), one idle slot after it (10), the PA drop at 11
	tick();
	CHECK(half_is(half_of_slot(9), 0x5F) && half_is(h, 0xAA) == false && rf_off == 0);
	tick();
	CHECK(rf_off == 0 && got.n_done == 0);
	nx = 0;
	tick();
	CHECK(rf_off == 1 && got.n_done == 1 && got.t_done == t_us);
	CHECK(!hrc6000_air_tx_active(AIR));
	CHECK(last_write(4, 0x45) == 0x2A && last_write(4, 0x46) == 0x52);	// the pair back
	CHECK(last_write(4, 0x10) == 0x02 && last_write(4, 0x41) == 0x40);	// receiving
	CHECK(rf_on == 1);
	// Receive after: the first chunk is a gap
	rand_stream(7);
	deliver(0, true, 0);
	CHECK(got.n_rx == 1 && (got.flags[0] & AIR_CHUNK_GAP));
	// No more slot writes after the end
	nx = 0;
	tick();
	CHECK(half_writes[0] + half_writes[1] == 0 || nx == 0);
	CHECK(lock_depth == 0);

	// tx_end with an empty queue: the chunk on air is the last
	got.n_done = 0;
	CHECK(hrc6000_air_tx_queue(AIR, &c1));
	CHECK(hrc6000_air_tx_begin(AIR));
	tick();	// 1: idle for slot 2
	CHECK(rf_on == 1);
	tick();	// 2: c1 for slot 3
	CHECK(rf_on == 2);	// no RF TX interrupt: keyed as the idle slot starts
	hrc6000_air_tx_end(AIR);
	tick();	// 3: empty, ended: idle (c1 on air)
	tick();	// 4: idle
	CHECK(got.n_done == 0);
	tick();	// 5: drop
	CHECK(got.n_done == 1 && rf_off == 2);
	CHECK(got.n_underrun == 1);

	// A core that queues one chunk for every space event never runs the queue dry
	got.n_space = got.n_underrun = 0;
	AIR->st.tx_underruns = 0;
	CHECK(hrc6000_air_tx_queue(AIR, &c1) && hrc6000_air_tx_queue(AIR, &c2));
	CHECK(hrc6000_air_tx_begin(AIR));
	tick();	// the idle slot's set-up takes no chunk
	CHECK(got.n_space == 0);
	for (int i = 0, seen = 0; i < 40; i++) {
		tick();
		CHECK(got.n_space == seen + 1);
		seen = got.n_space;
		CHECK(hrc6000_air_tx_queue(AIR, &c3));
	}
	CHECK(got.n_underrun == 0 && AIR->st.tx_underruns == 0 && AIR->st.ev_overflow == 0);
	hrc6000_air_tx_abort(AIR);
	tick();
}

static void test_abort_stall_suspend(void)
{
	air_profile_t p;
	open_p25(&p);
	air_tx_chunk_t c = chunk(0x11, 144, 0);

	// Abort mid-transmission: PA off now, TX_DONE, the pair back, receiving
	CHECK(hrc6000_air_tx_queue(AIR, &c) && hrc6000_air_tx_queue(AIR, &c));
	CHECK(hrc6000_air_tx_begin(AIR));
	tick();
	tick();
	CHECK(rf_on == 1);
	hrc6000_air_tx_abort(AIR);
	hrc6000_air_poll(AIR);
	CHECK(rf_off == 1 && got.n_done == 1 && !hrc6000_air_tx_active(AIR));
	CHECK(last_write(4, 0x46) == 0x52 && last_write(4, 0x10) == 0x02);
	CHECK(hrc6000_air_tx_space(AIR) == HRC6000_AIR_TXQ);

	// No slot clock for 200 ms: dropped by the watchdog
	CHECK(hrc6000_air_tx_queue(AIR, &c));
	CHECK(hrc6000_air_tx_begin(AIR));
	tick();
	tick();
	t_us += 150000;
	hrc6000_air_poll(AIR);
	CHECK(got.n_done == 1);
	t_us += 60000;
	hrc6000_air_poll(AIR);
	CHECK(got.n_done == 2 && rf_off == 2 && AIR->st.stalls == 1);

	// The chip leaves DMR under the session: the transmission ends, no chip writes, receive waits
	CHECK(hrc6000_air_tx_queue(AIR, &c));
	CHECK(hrc6000_air_tx_begin(AIR));
	tick();
	tick();
	nx = 0;
	hrc6000_air_suspend(AIR);
	CHECK(nx == 0);
	hrc6000_air_poll(AIR);
	CHECK(got.n_done == 3 && rf_off == 3);
	int n = got.n_rx;
	deliver(0, true, 0);
	CHECK(got.n_rx == n);
	CHECK(!hrc6000_air_tx_begin(AIR));
	hrc6000_air_rx_restart(AIR);
	deliver(0, true, 0);
	CHECK(got.n_rx == n + 1);
	// A PA key the board could not do is tried again at the next slot
	rf_ok = false;
	CHECK(hrc6000_air_tx_queue(AIR, &c) && hrc6000_air_tx_queue(AIR, &c));
	CHECK(hrc6000_air_tx_begin(AIR));
	int on = rf_on;
	tick();
	CHECK(rf_on == on);
	tick();
	CHECK(rf_on == on + 1 && AIR->st.rf_fail == 1);
	rf_ok = true;
	tick();
	CHECK(rf_on == on + 2);
	tick();
	CHECK(rf_on == on + 2);
	hrc6000_air_tx_abort(AIR);
	CHECK(lock_depth == 0);
}

static void test_open_close(void)
{
	air_profile_t p;
	open_p25(&p);
	CHECK(!hrc6000_air_open(AIR, &p, on_event, NULL));	// one session at a time

	// Close mid-transmission: PA off, the pair back, the DMR receive set-up
	air_tx_chunk_t c = chunk(0x11, 144, 0);
	CHECK(hrc6000_air_tx_queue(AIR, &c));
	CHECK(hrc6000_air_tx_begin(AIR));
	tick();
	tick();
	nx = 0;
	hrc6000_air_close(AIR, true);
	CHECK(rf_off == 1);
	CHECK(last_write(4, 0x46) == 0x52 && last_write(4, 0x45) == 0x2A);
	CHECK(last_write(4, 0x10) == 0x6E && last_write(4, 0x40) == 0xC3 && last_write(4, 0x41) == 0x00);
	CHECK(memcmp(&wr[1][0x47], hrc_ms_data_sync, 6) == 0);
	hrc6000_air_poll(AIR);
	CHECK(got.n_done == 0);		// the session is over: nothing more to the callback
	// Closed: the ISRs and the commands do nothing
	nx = 0;
	tick();
	regs[4][0x82] = 1;
	hrc6000_air_isr_sys(AIR);
	hrc6000_air_isr_rftx(AIR);
	CHECK(rf_on == 1 && !hrc6000_air_tx_queue(AIR, &c) && hrc6000_air_tx_space(AIR) == 0 && !hrc6000_air_tx_begin(AIR));

	// The slot engine runs again on the memory the session used: no stale events, ticks counted
	hrc6000_dmr_run(&eng);
	dmr_event_t ev;
	dmr_burst_t b;
	t_us += 30000;
	hrc6000_dmr_isr_timeslot(&eng);
	int pops = 0;
	while (hrc6000_dmr_pop(&eng, &ev, &b)) {
		pops++;
		CHECK(ev.kind == DMR_EV_SLOT_TICK || ev.kind == DMR_EV_SYNC_LOST);
	}
	CHECK(pops <= 1 && eng.st.ticks == 1 && eng.running);
	CHECK(last_write(4, 0x40) == 0xC3);

	// And a session again on top of it
	hrc6000_dmr_halt(&eng);
	eng.evq_tail = eng.evq_head;
	hrc6000_air_init(AIR, &dev, &ops, BUF, NULL, NULL);
	memset(&got, 0, sizeof(got));
	CHECK(hrc6000_air_open(AIR, &p, on_event, NULL));
	rand_stream(3);
	deliver(0, true, 0);
	deliver(1, false, 0);
	CHECK(got.n_rx == 2 && memcmp(got.dibit, air, 288) == 0);
	// Close with the chip out of DMR: nothing written
	nx = 0;
	hrc6000_air_close(AIR, false);
	CHECK(nx == 0);
	CHECK(lock_depth == 0);
}

// A chip description like the HR_C5000's: no 0x5F, no transmission start of its own (its vocoder runs on the MCU), a
// 0x10 that differs by channel. Its configuration page stays the simulator's 4: the page mapping is hrc.c's
static uint8_t c5_mode(bool repeater)
{
	return repeater ? 0x4F : 0x6B;
}

static const hrc_chip_t chip_c5 = {
	.cfg_page = 4, .id_low_first = true, .sync_reg = false, .mode_per_channel = true, .tc_last_rx = true,
	.dmr_mode = c5_mode,
};

static void test_chip_description(void)
{
	fresh();
	hrc_attach(&dev, &bus, &chip_c5);
	hrc6000_dmr_init(&eng, &dev, &ops);
	air_profile_t p = p25();
	hrc6000_air_init(AIR, &dev, &ops, BUF, NULL, NULL);
	nx = 0;
	CHECK(hrc6000_air_open(AIR, &p, on_event, NULL));
	CHECK(last_write(4, 0x10) == 0x02 && last_write(4, 0x40) == 0x40 && last_write(4, 0x41) == 0x40);
	CHECK(count_writes(4, 0x5F, -1) == 0);	// a register the chip doesn't have
	CHECK(memcmp(&wr[1][0x47], P25_SYNC, 6) == 0);

	air_tx_chunk_t c = chunk(0x11, 144, 0);
	CHECK(hrc6000_air_tx_queue(AIR, &c));
	nx = 0;
	CHECK(hrc6000_air_tx_begin(AIR));
	CHECK(last_write(4, 0x10) == 0x66 && last_write(4, 0x40) == 0xE3);
	CHECK(last_write(4, 0x20) == 0x00 && last_write(4, 0x21) == 0x02);	// the access policy alone
	CHECK(count_writes(4, 0x22, -1) == 0);					// no encoder start
	CHECK(last_write(4, 0x46) == 0x54 && last_write(4, 0x45) == 0x2B);

	// Close: the chip's own DMR 0x10, and the engine puts the channel's back when it runs (a repeater channel here)
	nx = 0;
	hrc6000_air_close(AIR, true);
	CHECK(last_write(4, 0x10) == 0x6B && count_writes(4, 0x5F, -1) == 0);
	eng.cfg.repeater = true;
	nx = 0;
	hrc6000_dmr_run(&eng);
	CHECK(last_write(4, 0x10) == 0x4F);
	hrc6000_dmr_halt(&eng);
	CHECK(lock_depth == 0);
}

// The copy RAM half h filled with the next 256 samples of a count (big endian), then its interrupt 32 ms after the
// last one, then the task when poll
static int copy_next;
static void copy_half(int h, bool poll)
{
	for (int i = 0; i < HRC6000_AIR_COPY_HALF; i++) {
		int16_t v = (int16_t)(copy_next++ - 3000);
		copy_ram[h * 2 * HRC6000_AIR_COPY_HALF + 2 * i] = (uint8_t)((uint16_t)v >> 8);
		copy_ram[h * 2 * HRC6000_AIR_COPY_HALF + 2 * i + 1] = (uint8_t)v;
	}
	regs[4][0x82] = 0x02;
	t_us += 32000;
	hrc6000_air_isr_sys(AIR);
	if (poll) {
		hrc6000_air_poll(AIR);
	}
}

static void test_copy(void)
{
	static const hal_baseband_t tap = { .rx_rate_hz = HRC6000_AIR_COPY_HZ, .rx_hi_hz = HRC6000_AIR_COPY_HI_HZ };
	air_profile_t p = p25();
	air_profile_t q = p;
	q.sync[1].flags = AIR_SYNC_START_ONLY;	// a sync the correlator can't hold: transmit only by the chip alone

	// Fit: the copy adds receive where the correlator has none, and only there
	CHECK(hrc6000_air_fit_tap(&q, NULL) == hrc6000_air_fit(&q));
	CHECK(hrc6000_air_fit_tap(&q, &tap) == (hrc6000_air_fit(&q) | AIR_FIT_RX | AIR_FIT_SOFT));
	CHECK(hrc6000_air_fit_tap(&p, &tap) == hrc6000_air_fit(&p));
	air_profile_t z = q;
	z.sym_rate = 1200;
	CHECK(hrc6000_air_fit_tap(&z, &tap) == 0);	// nothing to send it with: no session

	// A board without the copy: the profile opens for transmit only, the chip in the physical layer
	fresh();
	memset(&smg, 0, sizeof(smg));
	hrc6000_air_init(AIR, &dev, &ops, BUF, NULL, NULL);
	CHECK(hrc6000_air_open(AIR, &q, on_event, NULL));
	CHECK(smg.opens == 0 && !AIR->rx_copy && last_write(4, 0x10) != 0x80);
	hrc6000_air_close(AIR, true);

	// With it: FM receive, every audio filter off, nothing to the line out, the receive RAM's interrupt on
	fresh();
	fm_ram = true;
	copy_next = 0;
	regs[4][0x34] = 0xE4;
	regs[4][0x81] = 0x19;
	hrc6000_air_init(AIR, &dev, &ops, BUF, &sm_mem, &tap);
	CHECK(hrc6000_air_open(AIR, &q, on_event, NULL));
	CHECK(smg.opens == 1 && AIR->rx_copy && AIR->copy);
	CHECK(last_write(4, 0x10) == 0x80 && last_write(4, 0x34) == 0x00 && last_write(4, 0x36) == 0x00);
	CHECK(last_write(4, 0x60) == 0x00 && last_write(4, 0x81) == 0x1B && last_write(4, 0x41) == 0x00);
	CHECK(lock_depth == 0);

	// The first interrupt only shows which half is filling; from the second on, halves in order, each sample once
	copy_half(0, true);
	CHECK(smg.calls == 0 && AIR->st.halves == 0);
	CHECK(last_write(4, 0x83) == 0x02);	// the flag cleared, or no later half interrupts
	uint32_t t1 = t_us + 32000;
	for (int k = 1; k <= 6; k++) {
		copy_half(k % 2, true);
	}
	CHECK(smg.calls == 6 && smg.n == 6 * HRC6000_AIR_COPY_HALF && AIR->st.halves == 6 && got.n_rx == 6);
	int bad = 0;
	for (int i = 0; i < smg.n; i++) {
		bad += smg.s[i] != (int16_t)(HRC6000_AIR_COPY_HALF + i - 3000);
	}
	CHECK(bad == 0);
	// A half's first sample is 255 samples before its interrupt
	CHECK(smg.t[0] == t1 - 255u * 125u && smg.t[1] - smg.t[0] == 32000 && smg.t[5] - smg.t[0] == 5 * 32000);
	CHECK(smg.resets == 1 && smg.reset_at[0] == 0);	// the set-up is a gap: timing starts at the first half
	CHECK(AIR->st.half_fixes <= 1 && AIR->st.overruns == 0);

	// A missed interrupt: the half whose last sample moved is the one taken (no half twice), and no reset
	// (the ring holds what came; the soft modem rides a 32 ms hole as a fade)
	int fixes = (int)AIR->st.half_fixes;
	copy_next += HRC6000_AIR_COPY_HALF;	// half 1 filled unseen ...
	int at = smg.n;
	copy_half(0, true);			// ... and half 0 after it
	CHECK(smg.n == at + HRC6000_AIR_COPY_HALF && smg.s[at] == (int16_t)(copy_next - HRC6000_AIR_COPY_HALF - 3000));
	CHECK((int)AIR->st.half_fixes == fixes + 1);
	copy_half(1, true);
	CHECK(smg.s[smg.n - 1] == (int16_t)(copy_next - 1 - 3000));

	// The task away: two halves wait in the ring, the third is dropped and what follows is marked a gap
	int resets = smg.resets;
	at = smg.n;
	copy_half(0, false);
	copy_half(1, false);
	CHECK(AIR->st.overruns == 0);
	copy_half(0, false);
	CHECK(AIR->st.overruns == 1);
	hrc6000_air_poll(AIR);
	CHECK(smg.n == at + 2 * HRC6000_AIR_COPY_HALF && smg.resets == resets);
	copy_half(1, true);
	CHECK(smg.n == at + 3 * HRC6000_AIR_COPY_HALF && smg.resets == resets + 1 && smg.reset_at[resets] == at + 2 * HRC6000_AIR_COPY_HALF);

	// An interrupt of another kind takes nothing
	uint32_t halves = AIR->st.halves;
	regs[4][0x82] = 0x01;
	hrc6000_air_isr_sys(AIR);
	CHECK(AIR->st.halves == halves);

	// The board's mode entry rewrote the chip: the copy set up again, and a gap
	nx = 0;
	resets = smg.resets;
	hrc6000_air_rx_restart(AIR);
	CHECK(last_write(4, 0x10) == 0x80 && last_write(4, 0x34) == 0x00 && AIR->copy);
	copy_half(0, true);
	copy_half(1, true);
	CHECK(smg.resets == resets + 1);

	// Close: the filters and the interrupt mask as they were, the chip in DMR
	nx = 0;
	hrc6000_air_close(AIR, true);
	CHECK(!AIR->copy && last_write(4, 0x34) == 0xE4 && last_write(4, 0x81) == 0x19);
	CHECK(last_write(4, 0x10) == dev.chip->dmr_mode(false) && last_write(4, 0x40) == 0xC3);
	regs[4][0x82] = 0x02;
	halves = AIR->st.halves;
	hrc6000_air_isr_sys(AIR);
	CHECK(AIR->st.halves == halves);	// a late interrupt after the close
	CHECK(lock_depth == 0);

	// A soft modem that refuses the profile: no session
	smg.refuse = true;
	CHECK(!hrc6000_air_open(AIR, &q, on_event, NULL));
	smg.refuse = false;

	// The tap: the same halves to a reader, with the time of the next sample; no session beside it
	fresh();
	fm_ram = true;
	copy_next = 0;
	regs[4][0x34] = 0xE4;
	regs[4][0x81] = 0x19;
	hrc6000_air_init(AIR, &dev, &ops, BUF, &sm_mem, &tap);
	int16_t rd[300];
	CHECK(hrc6000_air_tap_read(AIR, rd, 300) == 0);
	CHECK(hrc6000_air_tap_start(AIR));
	CHECK(!hrc6000_air_tap_start(AIR) && !hrc6000_air_open(AIR, &p, on_event, NULL));
	CHECK(last_write(4, 0x10) == 0x80);
	copy_half(0, false);
	copy_half(1, false);
	CHECK(hrc6000_air_tap_time(AIR) == t_us - 255u * 125u);
	CHECK(hrc6000_air_tap_read(AIR, rd, 100) == 100 && rd[0] == (int16_t)(256 - 3000) && rd[99] == (int16_t)(355 - 3000));
	CHECK(hrc6000_air_tap_time(AIR) == t_us - 155u * 125u);
	CHECK(hrc6000_air_tap_read(AIR, rd, 300) == 156 && rd[155] == (int16_t)(511 - 3000));
	CHECK(hrc6000_air_tap_read(AIR, rd, 300) == 0);
	nx = 0;
	hrc6000_air_tap_stop(AIR, true);
	CHECK(last_write(4, 0x34) == 0xE4 && last_write(4, 0x81) == 0x19 && last_write(4, 0x10) == dev.chip->dmr_mode(false));
	nx = 0;
	hrc6000_air_tap_stop(AIR, true);
	CHECK(nx == 0);
	// No tap on a board without the copy
	hrc6000_air_init(AIR, &dev, &ops, BUF, NULL, NULL);
	CHECK(!hrc6000_air_tap_start(AIR));
	fm_ram = false;
}

static void test_fuzz(void)
{
	fuzz_t f;
	fuzz_init(&f, 0xA1C6);
	air_profile_t p;
	open_p25(&p);
	air_tx_chunk_t c = chunk(0x5A, 144, 0);

	for (long i = 0; i < fuzz_iters(20000); i++) {
		uint32_t r = fuzz_next(&f);
		switch (r % 12) {
		case 0: case 1: case 2:
			for (int k = 0; k < HRC6000_AIR_RAW_BYTES; k++) {
				record[k] = (uint8_t)fuzz_next(&f);
			}
			regs[4][0x82] = (uint8_t)(fuzz_next(&f) & 0x0F);
			regs[4][0x51] = (uint8_t)fuzz_next(&f);
			t_us += fuzz_next(&f) % 70000;
			hrc6000_air_isr_sys(AIR);
			break;
		case 3: case 4:
			t_us += 20000 + fuzz_next(&f) % 20000;
			hrc6000_air_isr_timeslot(AIR);
			break;
		case 5:
			c.nsyms = (uint16_t)(fuzz_next(&f) % 160);
			c.flags = (uint8_t)(fuzz_next(&f) & 1u);
			(void)hrc6000_air_tx_queue(AIR, &c);
			break;
		case 6: (void)hrc6000_air_tx_begin(AIR); break;
		case 7: hrc6000_air_isr_rftx(AIR); break;
		case 8: if ((r >> 8) % 4 == 0) hrc6000_air_tx_end(AIR); else if ((r >> 8) % 4 == 1) hrc6000_air_tx_abort(AIR); break;
		case 9: if ((r >> 8) % 16 == 0) hrc6000_air_rx_restart(AIR); break;
		case 10: rf_ok = (r >> 8) % 8 != 0; break;
		default:
			t_us += fuzz_next(&f) % 50000;
			hrc6000_air_poll(AIR);
			break;
		}
		CHECK(lock_depth == 0);
		CHECK(hrc6000_air_tx_space(AIR) <= HRC6000_AIR_TXQ);
		CHECK((uint8_t)(AIR->rec_head - AIR->rec_tail) <= HRC6000_AIR_RXQ);
		CHECK((uint8_t)(AIR->ev_head - AIR->ev_tail) <= HRC6000_AIR_EVQ);
		if (!hrc6000_air_tx_active(AIR)) {
			CHECK(!AIR->pa);	// never keyed outside a transmission
		}
	}
	for (int k = 0; k < got.nsym && k < MAXSYMS; k++) {
		CHECK(got.soft[k] >= -127 && got.soft[k] <= 127);
	}
	hrc6000_air_close(AIR, true);
	CHECK(!AIR->pa);
}

int main(void)
{
	_Static_assert(sizeof(air_mem_t) <= sizeof(eng.evq), "the session fits the engine's event queue");
	test_fit();
	test_golden();
	test_receive();
	test_reassembly();
	test_transmit();
	test_abort_stall_suspend();
	test_open_close();
	test_copy();
	test_chip_description();
	test_fuzz();
	printf("hrc6000AirTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
