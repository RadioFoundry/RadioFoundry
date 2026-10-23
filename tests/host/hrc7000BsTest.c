// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 split duplex BS outbound (drivers/modem/hrc7000/hrc7000_bs.c): the takeover of the slot engine, the physical
// layer set-up and its variants, the slot clock from either timeslot vector or PHY_RDY (one boundary per slot),
// key-up on an outbound TS1 slot, per-slot queues in order with idle fill, the slots in TX RAM (both halves or
// alternating), the PA keyed once and dropped after the last slot, the stall watchdog, and the way back to the slot
// engine (also when DMR is left). On plain arrays standing in for the modem registers and buffers
#include <stdio.h>
#include <string.h>
#include "modem/hrc7000/hrc7000_bs.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while (0)

static uint32_t regs[0x600 / 4];
static uint8_t buf[0xA00];
static hrc7000_t dev;
static hrc7000_dmr_t eng;
static hrc7000_bs_t bs;

static uint32_t now;
static int rf_on_calls, rf_off_calls, pa_arms;
static bool rf_state;

#define REG(off)	regs[(off) / 4]

static void op_rf(void *ctx, bool tx)
{
	(void)ctx;
	if (tx) {
		rf_on_calls++;
	} else {
		rf_off_calls++;
	}
	rf_state = tx;
}
static void op_pa_arm(void *ctx) { (void)ctx; pa_arms++; }
static uint32_t now_step;	// the clock moves this much per read (a busy wait ends)
static uint32_t op_now(void *ctx) { (void)ctx; now += now_step; return now; }
static void op_notify(void *ctx) { (void)ctx; REG(C7_SYS_INTERP_LIST) = 0; }

static const hrc7000_dmr_ops_t ops = { .rf = op_rf, .pa_arm = op_pa_arm, .now_us = op_now, .notify = op_notify };

// The slot builder: a recognizable slot (ts, kind, CACH or not, the burst's first byte, the CACH's TC)
static int built;
static bool fake_raw(const dmr_tx_burst_t *b, const dmr_cach_t *cach, int8_t ts, uint8_t cc, const uint8_t *emb_lc,
		     uint8_t *out)
{
	(void)emb_lc;
	memset(out, 0xEE, HRC7000_BS_RAW_BYTES);
	out[0] = (uint8_t)ts;
	out[1] = b->kind;
	out[2] = cach != NULL;
	out[3] = b->payload[0];
	out[4] = cc;
	out[5] = (cach != NULL) ? cach->tc : 0xFF;
	built++;
	return true;
}

static void setup(void)
{
	memset(regs, 0, sizeof regs);
	memset(buf, 0, sizeof buf);
	now = 1000000;
	rf_on_calls = rf_off_calls = pa_arms = built = 0;
	rf_state = false;
	hrc7000_attach(&dev, regs, buf);
	hrc7000_dmr_init(&eng, &dev, &ops);
	hrc7000_dmr_run(&eng);
}

static dmr_bs_config_t cfg_of(uint8_t variant)
{
	dmr_bs_config_t c;
	memset(&c, 0, sizeof(c));
	c.cc = 3;
	c.variant = variant;
	return c;
}

static void ts_int(bool tx_vector)
{
	hrc7000_dmr_isr_timeslot(&eng, tx_vector);
}

static void slot(void)
{
	now += 30000;
	ts_int(true);
}

static void drain(void)
{
	dmr_event_t ev;
	dmr_burst_t b;
	while (hrc7000_dmr_pop(&eng, &ev, &b)) {
	}
}

static dmr_tx_burst_t burst(uint8_t kind, uint8_t tag)
{
	dmr_tx_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.len = 12;
	b.payload[0] = tag;
	return b;
}

static void test_start_and_clock(void)
{
	setup();
	dmr_bs_config_t c = cfg_of(0);
	CHECK(!hrc7000_bs_start(&bs, &eng, &c, NULL));		// no slot builder
	hrc7000_dmr_halt(&eng);
	CHECK(!hrc7000_bs_start(&bs, &eng, &c, fake_raw));	// not in DMR
	hrc7000_dmr_run(&eng);
	CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
	CHECK(hrc7000_bs_active(&eng));
	CHECK(!hrc7000_bs_start(&bs, &eng, &c, fake_raw));	// one at a time
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_PHY);
	CHECK(REG(C7_LAYER2_CONTROL) == 0x82u);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0);
	CHECK(!hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));	// the slot engine is out
	uint32_t cnt, t;
	int8_t ts;
	CHECK(!hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts));	// a new slot clock: none until the first boundary

	// Unkeyed: the clock runs, nothing is sent, the outbound slot numbers alternate. The slot a boundary sets up is
	// on air from just after it, so the clock names that one: TS2 at the first boundary (base_tick + 1)
	slot();
	CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts) && t == now && ts == 2);
	uint32_t c0 = cnt;
	// The other vector and PHY_RDY in the same slot are not new boundaries
	now += 2000;
	ts_int(false);
	REG(C7_SYS_INTERP_LIST) = 1u << 12;
	hrc7000_dmr_isr_sys(&eng);
	CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts) && cnt == c0 && ts == 2);
	// (the array can't clear SYS_INTERP_LIST on the write back, so the ISR sees PHY_RDY in each of its loops)
	CHECK(bs.st.phy_ints >= 1 && bs.st.ts_ints == 2 && bs.st.ts_ticks == 1);
	// PHY_RDY alone keeps the clock running when no timeslot interrupt comes
	now += 28000;
	REG(C7_SYS_INTERP_LIST) = 1u << 12;
	hrc7000_dmr_isr_sys(&eng);
	CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts) && cnt == c0 + 1 && ts == 1);
	slot();
	CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts) && ts == 2);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0 && built == 0 && rf_on_calls == 0);
	CHECK(!hrc7000_dmr_is_transmitting(&eng));
	// A received burst meanwhile is not taken (nothing is received in the TX half)
	REG(C7_RX_TYPE_INFO) = 0x1012u;
	REG(C7_SYS_INTERP_LIST) = 1u << 3;
	hrc7000_dmr_isr_sys(&eng);
	CHECK(eng.st.rx_voice == 0);
	drain();
	hrc7000_bs_stop(&eng);
}

static void test_keyed_outbound(void)
{
	setup();
	dmr_bs_config_t c = cfg_of(0);
	CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
	slot();			// TS2 on air; the next one is TS1
	dmr_tx_burst_t h1 = burst(DMR_BURST_VOICE_LC_HEADER, 0x11), h2 = burst(DMR_BURST_CSBK, 0x22);
	dmr_cach_t cach;
	memset(&cach, 0, sizeof(cach));
	cach.tc = 0;
	CHECK(hrc7000_bs_queue(&eng, 1, &h1, &cach, NULL));
	CHECK(hrc7000_bs_queue(&eng, 2, &h2, NULL, NULL));
	CHECK(hrc7000_bs_space(&eng, 1) == HRC7000_BS_Q - 1 && hrc7000_bs_space(&eng, 2) == HRC7000_BS_Q - 1);
	hrc7000_bs_key(&eng, true);
	CHECK(hrc7000_dmr_is_transmitting(&eng));	// keying up
	slot();			// TS1, the key-up's first slot, set up and on air
	CHECK(built == 1 && buf[0] == 1 && buf[1] == DMR_BURST_VOICE_LC_HEADER && buf[2] == 1 && buf[3] == 0x11);
	uint32_t cnt, t;
	int8_t ts;
	// The slot clock names the slot just set up: the epoch the host relays is where that slot is on air
	CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts) && ts == buf[0]);
	CHECK(buf[4] == 3);						// the color code
	CHECK(memcmp(buf, buf + 0x24, HRC7000_BS_RAW_BYTES) == 0);	// both halves (variant 0)
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x80u);
	// The RF TX interrupt keys the RF chip and starts the PA one-shot once
	hrc7000_dmr_isr_rftx(&eng);
	CHECK(rf_on_calls == 1 && pa_arms == 1 && rf_state);
	CHECK(hrc7000_bs_keyed(&eng));
	slot();			// TS2 set up and on air
	hrc7000_dmr_isr_rftx(&eng);
	CHECK(rf_on_calls == 1 && pa_arms == 1);
	CHECK(built == 2 && buf[0] == 2 && buf[1] == DMR_BURST_CSBK && buf[2] == 0 && buf[3] == 0x22);
	CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts) && ts == 2);
	// Empty queues: idle bursts, counted
	slot();
	CHECK(built == 3 && buf[0] == 1 && buf[1] == DMR_BURST_IDLE && bs.st.underruns == 1);
	CHECK(bs.st.bursts == 3 && bs.st.cach_given == 1 && bs.st.cach_written == 1 && bs.st.keyups == 1);
	// A queue that fills
	dmr_tx_burst_t v = burst(DMR_BURST_VOICE, 0x33);
	for (int i = 0; i < HRC7000_BS_Q; i++) {
		CHECK(hrc7000_bs_queue(&eng, 2, &v, NULL, NULL));
	}
	CHECK(!hrc7000_bs_queue(&eng, 2, &v, NULL, NULL));
	CHECK(hrc7000_bs_space(&eng, 2) == 0);
	// Unkey: no more slots set up; the PA stays up for the slot set up last, then goes off; the queues are dropped
	hrc7000_bs_key(&eng, false);
	CHECK(hrc7000_bs_space(&eng, 2) == HRC7000_BS_Q);
	slot();
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0 && rf_state && built == 3);
	slot();
	CHECK(!rf_state && rf_off_calls == 1 && !hrc7000_bs_keyed(&eng));
	CHECK(!hrc7000_dmr_is_transmitting(&eng));
	// A second key-up waits for a TS1 slot again
	hrc7000_bs_key(&eng, true);
	CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts));
	if (ts == 1) {
		slot();		// TS2 now: not set up
		CHECK(built == 3);
	}
	slot();
	CHECK(built == 4 && buf[0] == 1);
	CHECK(bs.st.keyups == 2);
	hrc7000_bs_stop(&eng);
	CHECK(!rf_state);
}

static void test_variants(void)
{
	setup();
	dmr_bs_config_t c = cfg_of(HRC7000_BS_V_CONTINUOUS | HRC7000_BS_V_AUTOTEST | HRC7000_BS_V_HALF_ALT1);
	CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
	CHECK(REG(C7_WORK_MODE) == 0x02u);
	hrc7000_bs_key(&eng, true);
	slot();
	slot();
	CHECK(built == 1 && REG(C7_LAYER2_TXRX_CTRL) == 0x81u);
	CHECK(buf[0x24] == 1 && buf[0] == 0);		// the first slot in SEND_DATA1 only
	slot();
	CHECK(built == 2 && buf[0] == 2 && buf[0x24] == 1);	// then SEND_DATA0
	hrc7000_bs_stop(&eng);

	setup();
	c = cfg_of(HRC7000_BS_V_HALF_ALT0);
	CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_PHY);
	hrc7000_bs_key(&eng, true);
	slot();
	slot();
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x80u && buf[0] == 1 && buf[0x24] == 0);
	hrc7000_bs_stop(&eng);
}

static void test_stall_and_release(void)
{
	setup();
	dmr_bs_config_t c = cfg_of(0);
	CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
	hrc7000_bs_key(&eng, true);
	slot();
	slot();
	hrc7000_dmr_isr_rftx(&eng);
	CHECK(rf_state);
	now += 100000;
	hrc7000_dmr_poll(&eng);
	CHECK(rf_state);
	now += 150000;		// 220 ms without a boundary
	hrc7000_dmr_poll(&eng);
	CHECK(!rf_state && bs.st.stalls == 1 && !bs.keyed && REG(C7_LAYER2_TXRX_CTRL) == 0);
	uint32_t cnt, t;
	int8_t ts;
	CHECK(!hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts));
	hrc7000_bs_stop(&eng);

	// Keyed while the clock never started: unkeyed by the watchdog
	setup();
	CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
	hrc7000_bs_key(&eng, true);
	now += 250000;
	hrc7000_dmr_poll(&eng);
	CHECK(!bs.keyed && bs.st.stalls == 1);
	hrc7000_bs_stop(&eng);

	// Back to the slot engine: layer 2, receiving, a normal transmission works again
	setup();
	CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
	hrc7000_bs_key(&eng, true);
	slot();
	slot();
	hrc7000_dmr_isr_rftx(&eng);
	hrc7000_bs_stop(&eng);
	CHECK(!hrc7000_bs_active(&eng) && !rf_state);
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_DMR && REG(C7_LAYER2_SLOTON) == 0x05u && REG(C7_LAYER2_TXRX_CTRL) == 0x40u);
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	hrc7000_dmr_tx_abort(&eng);
	slot();

	// A color code set while the outbound ran reaches the receiver when it restarts
	setup();
	CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
	dmr_config_t dc;
	memset(&dc, 0, sizeof(dc));
	dc.cc = 7;
	hrc7000_dmr_configure(&eng, &dc);
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_PHY);	// untouched meanwhile
	hrc7000_bs_stop(&eng);
	CHECK(REG(C7_LOCAL_CC) == 7);

	// Leaving DMR ends the outbound: PA off, the chip back in layer 2
	setup();
	CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
	hrc7000_bs_key(&eng, true);
	slot();
	slot();
	hrc7000_dmr_isr_rftx(&eng);
	hrc7000_dmr_halt(&eng);
	CHECK(!hrc7000_bs_active(&eng) && !rf_state && REG(C7_WORK_MODE) == C7_WORK_MODE_DMR);
	hrc7000_bs_stop(&eng);		// the hotspot's own stop afterwards does nothing more
	CHECK(!hrc7000_bs_active(&eng));
}

// bs_queue_at: a burst for one numbered slot (the slot set up at that boundary): dropped once it has passed, held while
// it is ahead
static void test_queue_at(void)
{
	setup();
	dmr_bs_config_t c = cfg_of(0);
	CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
	slot();
	hrc7000_bs_key(&eng, true);
	slot();			// TS1 set up: on air
	uint32_t cnt, t;
	int8_t ts;
	CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts) && ts == 1);
	dmr_tx_burst_t old = burst(DMR_BURST_DATA_HEADER, 0x01), cs = burst(DMR_BURST_CSBK, 0x5A);
	// Boundary cnt + 2 sets up TS1 again
	CHECK(hrc7000_bs_queue_at(&eng, cnt, 1, &old, NULL, NULL));
	CHECK(hrc7000_bs_queue_at(&eng, cnt + 2u, 1, &cs, NULL, NULL));
	slot();
	slot();
	CHECK(bs.st.at_late == 1 && buf[0] == 1 && buf[1] == DMR_BURST_CSBK && buf[3] == 0x5A);
	// For cnt + 6: cnt + 4 goes idle meanwhile, then it plays
	CHECK(hrc7000_bs_queue_at(&eng, cnt + 6u, 1, &cs, NULL, NULL));
	slot();
	slot();
	CHECK(bs.st.at_wait == 1 && buf[0] == 1 && buf[1] == DMR_BURST_IDLE);
	slot();
	slot();
	CHECK(buf[0] == 1 && buf[1] == DMR_BURST_CSBK && bs.st.at_late == 1);
	CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts) && ts == 1);
	hrc7000_bs_stop(&eng);
}

// bs_restart: not keyed only; own TX timing off (passive), on again at the boundary asked for, slot ts there
static void test_restart(void)
{
	for (int8_t want = 1; want <= 2; want++) {
		setup();
		dmr_bs_config_t c = cfg_of(0);
		CHECK(hrc7000_bs_start(&bs, &eng, &c, fake_raw));
		slot();
		hrc7000_bs_key(&eng, true);
		CHECK(!hrc7000_bs_restart(&eng, now + 10000u, want));		// keyed
		hrc7000_bs_key(&eng, false);
		slot();
		slot();
		REG(C7_LAYER2_CONTROL) = 0;
		now_step = 7;
		uint32_t target = now + 10000u - 30000u * 3u;	// the next of target + k x 30 ms: 10 ms from now
		CHECK(hrc7000_bs_restart(&eng, target, want));
		now_step = 0;
		CHECK((int32_t)(now - (target + 90000u)) >= 0 && (int32_t)(now - (target + 90000u)) < 30);
		CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_PHY && REG(C7_LAYER2_CONTROL) == 0x82u);
		CHECK(bs.st.restarts == 1 && !eng.timing);
		uint32_t cnt, t;
		int8_t ts;
		CHECK(!hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts));
		uint32_t ticks = eng.tick;
		bs.restarting = true;		// a boundary of the old timer while a restart waits: not taken
		slot();
		bs.restarting = false;
		CHECK(eng.tick == ticks && !eng.timing);
		slot();			// the new clock's first boundary
		CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts) && ts == want);
		slot();
		CHECK(hrc7000_dmr_slot_clock(&eng, &cnt, &t, &ts) && ts == 3 - want);
		hrc7000_bs_stop(&eng);
	}
}

int main(void)
{
	test_start_and_clock();
	test_keyed_outbound();
	test_variants();
	test_stall_and_release();
	test_queue_at();
	test_restart();
	if (failures) {
		printf("hrc7000BsTest: %d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("hrc7000BsTest: all %d checks passed\n", checks);
	return 0;
}
