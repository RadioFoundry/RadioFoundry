// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 DMR engine (drivers/modem/hrc7000/hrc7000_dmr.c) on plain arrays standing in for the modem registers and
// buffers: RX_TYPE_INFO decode, send types, the receive path (events, slot numbers, voice access flag, CRC policy,
// late entry), the transmit schedule (one burst every other tick, PA one-shot, RF switch, drain, TX_DONE), polite
// access, underrun fill, abort and the stall watchdog (docs/spec/modem-hrc7000.md 2-6)
#include <stdio.h>
#include <string.h>
#include "modem/hrc7000/hrc7000_dmr.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t regs[0x600 / 4];
static uint8_t buf[0xA00];
static hrc7000_t dev;
static hrc7000_dmr_t eng;

static uint32_t now;
static int rf_on_calls, rf_off_calls, pa_arms, lock_depth, lock_max, notifies;
static bool rf_state;

#define REG(off)	regs[(off) / 4]

static void op_rf(void *ctx, bool tx)
{
	if (tx) {
		rf_on_calls++;
	} else {
		rf_off_calls++;
	}
	rf_state = tx;
}
static void op_pa_arm(void *ctx) { pa_arms++; }
static uint32_t op_now(void *ctx) { return now; }
static void op_lock(void *ctx) { if (++lock_depth > lock_max) lock_max = lock_depth; }
static void op_unlock(void *ctx) { lock_depth--; }
// The chip clears SYS_INTERP_LIST when the CPU writes the bits back; an array can't, so the event hook does it
static void op_notify(void *ctx) { notifies++; REG(C7_SYS_INTERP_LIST) = 0; }

static const hrc7000_dmr_ops_t ops = {
	.rf = op_rf, .pa_arm = op_pa_arm, .now_us = op_now, .lock = op_lock, .unlock = op_unlock, .notify = op_notify,
};

// RX_TYPE_INFO: voice burst number vb (1..6, 0 none), data type, CC, sync family index (0 MS, 1 BS, 2 TS1, 3 TS2),
// sync type (2 = data / voice sync), chip slot, TC
static uint32_t rti(uint8_t vb, uint8_t dt, uint8_t cc, uint8_t fam, uint8_t chip_slot, uint8_t tc)
{
	return (uint32_t)vb << 12 | (uint32_t)dt << 8 | (uint32_t)cc << 4 | (uint32_t)fam << 2 | 2u |
	       (uint32_t)chip_slot << 19 | (uint32_t)tc << 22;
}

static void setup(void)
{
	memset(regs, 0, sizeof regs);
	memset(buf, 0, sizeof buf);
	now = 1000000;
	rf_on_calls = rf_off_calls = pa_arms = lock_depth = lock_max = notifies = 0;
	rf_state = false;
	hrc7000_attach(&dev, regs, buf);
	hrc7000_dmr_init(&eng, &dev, &ops);
}

static int drain(dmr_event_t *evs, dmr_burst_t *bursts, int max)
{
	int n = 0;
	dmr_event_t ev;
	dmr_burst_t b;
	while (hrc7000_dmr_pop(&eng, &ev, &b)) {
		if (n < max) {
			evs[n] = ev;
			bursts[n] = b;
			n++;
		}
	}
	return n;
}

// Events of one kind left in the queue (the queue is emptied)
static int count_kind(uint8_t kind, dmr_burst_t *last)
{
	dmr_event_t evs[32];
	static dmr_burst_t bs[32];
	int n = drain(evs, bs, 32), c = 0;
	for (int i = 0; i < n; i++) {
		if (evs[i].kind == kind) {
			c++;
			if (last != NULL) {
				*last = bs[i];
			}
		}
	}
	return c;
}

static void deframe(uint32_t info)
{
	REG(C7_RX_TYPE_INFO) = info;
	REG(C7_SYS_INTERP_LIST) = 1u << 3;
	hrc7000_dmr_isr_sys(&eng);
	REG(C7_SYS_INTERP_LIST) = 0;
}

static void tick(bool tx_vector)
{
	now += 30000;
	hrc7000_dmr_isr_timeslot(&eng, tx_vector);
}

static void test_pure(void)
{
	hrc7000_rx_status_t s;
	hrc7000_dmr_decode(rti(2, 0, 5, 0, 1, 0), &s);
	CHECK(s.kind == DMR_BURST_VOICE && s.voice_index == 1 && s.cc == 5 && s.sync == DMR_SYNC_MS && s.chip_slot == 1 && s.crc_ok);
	hrc7000_dmr_decode(rti(2, 0, 5, 0, 1, 0) | (1u << 28), &s);	// QR error in the EMB
	CHECK(s.kind == DMR_BURST_VOICE && !s.crc_ok);
	hrc7000_dmr_decode(rti(1, 0, 5, 0, 0, 0) | (1u << 28), &s);	// burst A has no EMB to check
	CHECK(s.crc_ok);
	hrc7000_dmr_decode(rti(0, DMR_BURST_VOICE_LC_HEADER, 1, 1, 0, 1), &s);
	CHECK(s.kind == DMR_BURST_VOICE_LC_HEADER && s.sync == DMR_SYNC_BS && s.tc == 1 && s.crc_ok);
	hrc7000_dmr_decode(rti(0, DMR_BURST_VOICE_LC_HEADER, 1, 1, 0, 1) | (1u << 25), &s);	// RS error
	CHECK(!s.crc_ok);
	hrc7000_dmr_decode(rti(0, DMR_BURST_CSBK, 1, 2, 0, 0) | (1u << 26), &s);		// CRC-16 error
	CHECK(s.kind == DMR_BURST_CSBK && s.sync == DMR_SYNC_TS1 && !s.crc_ok);
	hrc7000_dmr_decode((rti(0, DMR_BURST_CSBK, 1, 0, 0, 0) & ~3u) | 1u, &s);		// RC sync: unusable
	CHECK(s.kind == HRC7000_KIND_NONE);
	hrc7000_dmr_decode(rti(0, 0xB, 1, 0, 0, 0), &s);					// reserved type
	CHECK(s.kind == HRC7000_KIND_NONE);

	// 0x08 + (n << 4) + LCSS (2.6). The user guide's example gives F as 0x50; the formula gives 0x58 (open item)
	static const uint8_t voice_types[6] = { 0x08, 0x19, 0x2B, 0x3B, 0x4A, 0x58 };
	for (uint8_t i = 0; i < 6; i++) {
		CHECK(hrc7000_dmr_send_type(DMR_BURST_VOICE, i, true) == voice_types[i]);
		CHECK(hrc7000_dmr_send_type(DMR_BURST_VOICE, i, false) == (uint32_t)(0x08 | i << 4));
	}
	CHECK(hrc7000_dmr_send_type(DMR_BURST_VOICE_LC_HEADER, 0, true) == 0x10);
	CHECK(hrc7000_dmr_send_type(DMR_BURST_TERMINATOR_LC, 0, true) == 0x20);
	CHECK(hrc7000_dmr_send_type(DMR_BURST_CSBK, 0, false) == 0x30);
	CHECK(hrc7000_dmr_payload_len(DMR_BURST_VOICE) == 27 && hrc7000_dmr_payload_len(DMR_BURST_RATE_34_DATA) == 18 &&
	      hrc7000_dmr_payload_len(DMR_BURST_RATE_1_DATA) == 24 && hrc7000_dmr_payload_len(DMR_BURST_CSBK) == 12);
	const uint8_t lc[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x01 };
	CHECK(hrc7000_dmr_emb_checksum(lc) == 10);
	const uint8_t lc2[9] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	CHECK(hrc7000_dmr_emb_checksum(lc2) == (9 * 255) % 31);
}

static void test_rx(void)
{
	setup();
	hrc7000_dmr_run(&eng);
	CHECK(lock_depth == 0 && lock_max == 1);
	CHECK(REG(C7_LAYER2_CONTROL) == 0xC0 && REG(C7_LAYER2_SLOTON) == 0x05 && REG(C7_LAYER2_TXRX_CTRL) == 0x40);
	CHECK(REG(C7_VOICE_EMB_CTRL) == 0x02 && REG(C7_LOCAL_CC) == 1);
	const dmr_config_t cfg = { .own_id = 1234, .cc = 3, .access = DMR_ACCESS_POLITE_CC, .repeater = false };
	hrc7000_dmr_configure(&eng, &cfg);
	CHECK(REG(C7_LOCAL_CC) == 3);

	// A voice LC header from the RX RAM, MS sync, chip slot 1: TS label 2
	for (int i = 0; i < 12; i++) {
		buf[C7_RX_RAM + i] = (uint8_t)(0x10 + i);
	}
	tick(false);
	deframe(rti(0, DMR_BURST_VOICE_LC_HEADER, 3, 0, 1, 0));
	dmr_burst_t b;
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1);
	CHECK(b.kind == DMR_BURST_VOICE_LC_HEADER && b.lc_valid && b.lc[0] == 0x10 && b.lc[8] == 0x18 && b.len == 12);
	CHECK(b.cc == 3 && b.ts == 2 && b.crc_ok && b.in_step);
	CHECK(eng.st.rx_lc_header == 1);

	// Voice A on chip slot 1: the next decision for chip slot 1 gets the voice access flag
	for (int i = 0; i < 27; i++) {
		buf[C7_VOICE_RAM + i] = (uint8_t)i;
	}
	deframe(rti(1, 0, 3, 0, 1, 0));
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1);
	CHECK(b.kind == DMR_BURST_VOICE && b.voice_index == 0 && b.len == 27 && b.payload[26] == 26);
	REG(C7_LAYER2_STATUS) = 0;		// slot 0 now: the coming one is slot 1 (HRC7000_RX_NEXT_SLOT_XOR)
	tick(false);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x8040);
	REG(C7_LAYER2_STATUS) = 1u << 30;
	tick(false);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x40);
	count_kind(DMR_EV_SLOT_TICK, NULL);

	// Late entry: the embedded LC from the RX RAM goes out with the next voice burst
	for (int i = 0; i < 9; i++) {
		buf[C7_RX_RAM + i] = (uint8_t)(0xA0 + i);
	}
	REG(C7_RX_TYPE_INFO) = rti(6, 0, 3, 0, 1, 0);
	REG(C7_SYS_INTERP_LIST) = (1u << 2) | (1u << 3);
	hrc7000_dmr_isr_sys(&eng);
	REG(C7_SYS_INTERP_LIST) = 0;
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1);
	CHECK(b.kind == DMR_BURST_VOICE && b.voice_index == 5 && b.lc_valid && b.lc[0] == 0xA0 && b.lc[8] == 0xA8);
	CHECK(eng.st.late_entry == 1);

	// A CSBK with a bad CRC is dropped; a voice burst with a bad EMB is delivered, flagged. (A dropped burst
	// commits no event, so the array's interrupt list isn't cleared and the ISR's loop sees it up to 4 times)
	deframe(rti(0, DMR_BURST_CSBK, 3, 0, 0, 0) | (1u << 26));
	CHECK(count_kind(DMR_EV_BURST_RX, NULL) == 0 && eng.st.rx_crc_err >= 1);
	uint32_t crc_errs = eng.st.rx_crc_err;
	deframe(rti(3, 0, 3, 0, 1, 0) | (1u << 28));
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1 && !b.crc_ok && eng.st.rx_crc_err == crc_errs + 1);

	// Repeater: BS sync with TC teaches the chip slots their TS numbers
	deframe(rti(0, DMR_BURST_IDLE, 3, 1, 0, 1));
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1 && b.ts == 2 && b.sync == DMR_SYNC_BS);
	deframe(rti(0, DMR_BURST_IDLE, 3, 0, 1, 0));	// MS sync on the other chip slot: TS 1 learned
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1 && b.ts == 1);
	// Slot lock
	hrc7000_dmr_rx_lock_slot(&eng, 2);
	deframe(rti(0, DMR_BURST_IDLE, 3, 0, 1, 0));
	CHECK(count_kind(DMR_EV_BURST_RX, NULL) == 0);
	hrc7000_dmr_rx_lock_slot(&eng, 0);

	// Voice lost after about 390 ms without voice on the call's slot
	deframe(rti(2, 0, 3, 0, 1, 0));
	count_kind(DMR_EV_BURST_RX, NULL);
	for (int i = 0; i < 15; i++) {
		tick(false);
	}
	CHECK(count_kind(DMR_EV_VOICE_LOST, NULL) == 1);
	// RC / no type bursts are counted and dropped; TX vector ticks are ignored while receiving
	deframe((rti(0, 0, 3, 0, 0, 0) & ~3u) | 1u);
	CHECK(count_kind(DMR_EV_BURST_RX, NULL) == 0 && eng.st.rx_unusable >= 1);
	uint32_t t = eng.tick;
	tick(true);
	CHECK(eng.tick == t);
}

static void test_cc_scan(void)
{
	setup();
	hrc7000_dmr_run(&eng);	// no filter: scan
	CHECK(REG(C7_LOCAL_CC) == 1);
	for (int i = 0; i < 4; i++) {
		tick(false);
	}
	CHECK(REG(C7_LOCAL_CC) == 2 && eng.st.cc_scan_steps == 1);
	deframe(rti(0, DMR_BURST_IDLE, 2, 0, 0, 0));	// heard: held for 5 s
	for (int i = 0; i < 100; i++) {
		tick(false);
	}
	CHECK(REG(C7_LOCAL_CC) == 2);
	hrc7000_dmr_set_cc_filter(&eng, 7);
	CHECK(REG(C7_LOCAL_CC) == 7);
	for (int i = 0; i < 300; i++) {
		tick(false);
	}
	CHECK(REG(C7_LOCAL_CC) == 7);
	count_kind(0xFF, NULL);
}

static dmr_tx_burst_t voice(uint8_t idx, uint8_t fill)
{
	dmr_tx_burst_t b = { .kind = DMR_BURST_VOICE, .voice_index = idx, .len = 27 };
	memset(b.payload, fill, 27);
	return b;
}

static void test_tx(void)
{
	setup();
	hrc7000_dmr_run(&eng);
	const dmr_config_t cfg = { .own_id = 1234, .cc = 1, .access = DMR_ACCESS_IMPOLITE, .repeater = false };
	hrc7000_dmr_configure(&eng, &cfg);
	tick(false);
	count_kind(0xFF, NULL);

	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	CHECK(hrc7000_dmr_is_transmitting(&eng));
	CHECK(REG(C7_LAYER2_CONTROL) == 0xC3 && REG(C7_LAYER2_TXRX_CTRL) == 0 && REG(C7_AUDIO_BUFFER_CLR) == 0x03);
	CHECK(!hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));	// already sending

	dmr_tx_burst_t h = { .kind = DMR_BURST_VOICE_LC_HEADER, .len = 9, .payload = { 0, 0, 0, 0, 0, 9, 0, 0, 1 } };
	CHECK(hrc7000_dmr_tx_queue(&eng, &h));
	dmr_tx_burst_t a = voice(0, 0x11), bb = voice(1, 0x22);
	CHECK(hrc7000_dmr_tx_queue(&eng, &a) && hrc7000_dmr_tx_queue(&eng, &bb));
	CHECK(hrc7000_dmr_tx_queue_space(&eng) == HRC7000_DMR_TXQ_DEPTH - 1 - 3);

	// RX vector ticks don't drive a transmission
	uint32_t t = eng.tick;
	tick(false);
	CHECK(eng.tick == t);

	// Tick 1: the header is armed (TX RAM: 9 LC bytes and 3 zeros), PA one-shot started
	tick(true);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x80 && REG(C7_LAYER2_SEND_TYPE) == 0x10 && pa_arms == 1);
	CHECK(buf[C7_TX_RAM + 5] == 9 && buf[C7_TX_RAM + 8] == 1 && buf[C7_TX_RAM + 9] == 0);
	// The RF TX interrupt switches the RF chip to TX
	hrc7000_dmr_isr_rftx(&eng);
	CHECK(rf_on_calls == 1 && rf_state);
	CHECK(count_kind(DMR_EV_TX_SLOT_STARTED, NULL) == 1);
	// Tick 2: the other slot, nothing; the PA stays (3 ms after the RF TX interrupt is less than 20 ms)
	now += 0;
	hrc7000_dmr_isr_timeslot(&eng, true);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0 && rf_state);
	// Tick 3: voice A, with the call's LC and its checksum in the TX RAM for the embedded LC; PA off (> 20 ms)
	tick(true);
	CHECK(!rf_state && rf_off_calls == 1);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x80 && REG(C7_LAYER2_SEND_TYPE) == 0x08 && pa_arms == 2);
	CHECK(buf[C7_VOICE_RAM] == 0x11 && buf[C7_VOICE_RAM + 26] == 0x11);
	CHECK(buf[C7_TX_RAM + 5] == 9 && buf[C7_TX_RAM + 9] == 10);
	CHECK(count_kind(DMR_EV_TX_SLOT_ENDED, NULL) == 1);
	tick(true);
	tick(true);	// voice B
	CHECK(REG(C7_LAYER2_SEND_TYPE) == 0x19 && buf[C7_VOICE_RAM] == 0x22);
	// Underrun: silence for voice C, counted
	tick(true);
	tick(true);
	CHECK(REG(C7_LAYER2_SEND_TYPE) == 0x2B && buf[C7_VOICE_RAM] == 0xB9 && eng.st.tx_underrun == 1);

	// End: a terminator queued, then tx_end; it goes out, then the drain, then TX_DONE and the receiver restart
	dmr_tx_burst_t term = { .kind = DMR_BURST_TERMINATOR_LC, .len = 9 };
	CHECK(hrc7000_dmr_tx_queue(&eng, &term));
	hrc7000_dmr_tx_end(&eng);
	CHECK(hrc7000_dmr_is_transmitting(&eng));
	tick(true);
	tick(true);
	CHECK(REG(C7_LAYER2_SEND_TYPE) == 0x20);
	uint32_t underruns = eng.st.tx_underrun;
	tick(true);	// other slot
	tick(true);	// our slot, nothing left and the end asked for: drain
	CHECK(hrc7000_dmr_is_transmitting(&eng) && eng.st.tx_underrun == underruns);
	tick(true);
	CHECK(!hrc7000_dmr_is_transmitting(&eng));
	CHECK(count_kind(DMR_EV_TX_DONE, NULL) == 1);
	CHECK(REG(C7_LAYER2_CONTROL) == 0xC0 && REG(C7_LAYER2_TXRX_CTRL) == 0x40 && REG(C7_LAYER2_SLOTON) == 0x05);
	CHECK(lock_depth == 0);
}

static void test_tx_edges(void)
{
	// tx_end with nothing queued and nothing sent finishes at once
	setup();
	hrc7000_dmr_run(&eng);
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	hrc7000_dmr_tx_end(&eng);
	CHECK(!hrc7000_dmr_is_transmitting(&eng) && count_kind(DMR_EV_TX_DONE, NULL) == 1);

	// A data list: the bursts go out, then the list end drains by itself
	dmr_tx_burst_t d = { .kind = DMR_BURST_DATA_HEADER, .len = 10 };
	dmr_tx_burst_t r12 = { .kind = DMR_BURST_RATE_12_DATA, .len = 12 };
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	hrc7000_dmr_tx_queue(&eng, &d);
	hrc7000_dmr_tx_queue(&eng, &r12);
	for (int i = 0; i < 8; i++) {
		tick(true);
	}
	CHECK(!hrc7000_dmr_is_transmitting(&eng) && eng.st.tx_bursts == 2 && count_kind(DMR_EV_TX_DONE, NULL) == 1);

	// Abort
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	dmr_tx_burst_t a = voice(0, 1);
	hrc7000_dmr_tx_queue(&eng, &a);
	tick(true);
	hrc7000_dmr_tx_abort(&eng);
	tick(true);
	CHECK(!hrc7000_dmr_is_transmitting(&eng) && count_kind(DMR_EV_TX_DONE, NULL) == 1);

	// Stall: no timeslot interrupt for 200 ms while sending
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	now += 250000;
	hrc7000_dmr_poll(&eng);
	CHECK(!hrc7000_dmr_is_transmitting(&eng) && eng.st.stalls == 1);
	dmr_burst_t b;
	CHECK(count_kind(DMR_EV_SYNC_LOST, &b) == 1);

	// Halt while sending: TX_DONE, RF off
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	hrc7000_dmr_tx_queue(&eng, &a);
	tick(true);
	hrc7000_dmr_isr_rftx(&eng);
	CHECK(rf_state);
	hrc7000_dmr_halt(&eng);
	CHECK(!rf_state && REG(C7_LAYER2_TXRX_CTRL) == 0 && count_kind(DMR_EV_TX_DONE, NULL) == 1);
	// Halted: interrupts do nothing
	{
		uint32_t t = eng.tick;
		tick(false);
		CHECK(eng.tick == t);
	}
}

static void test_polite_and_repeater(void)
{
	setup();
	hrc7000_dmr_run(&eng);
	const dmr_config_t cfg = { .own_id = 1, .cc = 1, .access = DMR_ACCESS_POLITE_CC, .repeater = true };
	hrc7000_dmr_configure(&eng, &cfg);
	tick(false);
	deframe(rti(1, 0, 1, 1, 0, 0));	// own color code heard just now
	CHECK(!hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 1, DMR_ACCESS_POLITE_CC));
	CHECK(count_kind(DMR_EV_TX_REJECTED, NULL) == 1 && eng.st.tx_rejected == 1);
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 1, DMR_ACCESS_IMPOLITE));
	hrc7000_dmr_tx_end(&eng);
	count_kind(0xFF, NULL);
	for (int i = 0; i < 13; i++) {
		tick(false);
	}
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 1, DMR_ACCESS_POLITE_CC));	// quiet for 390 ms

	// Repeater end of transmission: passive timing, receive from the next tick (keeps the repeater's timing)
	dmr_tx_burst_t c = { .kind = DMR_BURST_CSBK, .len = 10 };
	hrc7000_dmr_tx_queue(&eng, &c);
	for (int i = 0; i < 6 && hrc7000_dmr_is_transmitting(&eng); i++) {
		tick(true);
	}
	CHECK(!hrc7000_dmr_is_transmitting(&eng));
	CHECK(REG(C7_LAYER2_CONTROL) == 0xC0 && REG(C7_LAYER2_TXRX_CTRL) == 0);
	tick(false);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x40);
	count_kind(0xFF, NULL);
}

static void test_event_overflow(void)
{
	setup();
	hrc7000_dmr_run(&eng);
	for (int i = 0; i < HRC7000_DMR_EVQ_DEPTH + 5; i++) {
		tick(false);
	}
	CHECK(eng.st.ev_overflow == 6);
	dmr_event_t evs[32];
	static dmr_burst_t bs[32];
	CHECK(drain(evs, bs, 32) == HRC7000_DMR_EVQ_DEPTH - 1);
	CHECK(evs[0].kind == DMR_EV_SLOT_TICK && evs[0].arg == 1 && evs[1].arg == 2);
}

int main(void)
{
	test_pure();
	test_rx();
	test_cc_scan();
	test_tx();
	test_tx_edges();
	test_polite_and_repeater();
	test_event_overflow();
	if (failures) {
		printf("hrc7000DmrTest: %d failures\n", failures);
		return 1;
	}
	printf("hrc7000DmrTest: all passed\n");
	return 0;
}
