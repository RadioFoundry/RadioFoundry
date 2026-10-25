// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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
	CHECK(hrc7000_mod_dc(0) == 381);
	CHECK(hrc7000_mod_dc(128) == 512);
	CHECK(hrc7000_mod_dc(255) == 641);
	CHECK(hrc7000_sig_center_dc(128) == 0x80800000u);
	CHECK(hrc7000_sig_center_dc(0) == (381u & 0xFF) * 0x0101u);
	CHECK(hrc7000_mod_bias_dc(128, 45) == (512u << 16 | 45u));
	CHECK(hrc7000_mod_bias_dc(255, 0x4D4D) == (641u << 16 | 0x4D4Du));
	// The DB25-D owner's page: TX center 106 = 1.577 V = 489 (0x1E9), modulation balance 0x21 at 461.4625 MHz: the two
	// words the stock firmware writes at every receive set-up (hardware-db25d.md 4.6)
	CHECK(hrc7000_mod_dc(106) == 489);
	CHECK(hrc7000_sig_center_dc(106) == 0x6A6AE9E9u);
	CHECK(hrc7000_mod_bias_dc(106, 0x21) == 0x01E90021u);
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
	// The flag goes on the slot LAYER2_STATUS b30 shows at the interrupt (HRC7000_RX_NEXT_SLOT_XOR 0, every HR_C7000
	// radio measured). The old default 1 put it on the other slot: one radio played silence (2026-10-22)
	REG(C7_LAYER2_STATUS) = 1u << 30;
	tick(false);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x8040);
	REG(C7_LAYER2_STATUS) = 0;
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
	// TDMA direct mode sync names the slot too, and the voice after it on the same chip slot keeps that number (an SFR
	// user tells the talker's copy from the repeat on bursts B-F, which carry no sync)
	deframe(rti(1, 0, 3, 3, 0, 0));			// voice A, TS2 sync, chip slot 0
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1 && b.ts == 2 && b.sync == DMR_SYNC_TS2);
	deframe(rti(2, 0, 3, 0, 0, 0));			// voice B, no sync of its own
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1 && b.ts == 2);
	deframe(rti(0, DMR_BURST_IDLE, 3, 0, 1, 0));	// the other chip slot: TS 1
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

// An SFR user hears every call twice: the talker's copy on one chip slot, the repeat three slots (90 ms) later on the
// other. The chip writes the voice RAM only for a slot decided with the voice-access flag, else the burst reads the
// bytes of the last one decoded. Each burst here carries its own payload when the chip decodes it: every voice burst
// of both copies must come out with its own bytes (the flag once followed the last voice burst, so it was always on
// the other slot and the listener played one burst over and over, 2026-10-20)
static void test_rx_sfr_both_copies(void)
{
	setup();
	const dmr_config_t cfg = { .own_id = 1234, .cc = 2, .access = DMR_ACCESS_POLITE_CC, .repeater = false };
	hrc7000_dmr_configure(&eng, &cfg);
	hrc7000_dmr_run(&eng);
	count_kind(DMR_EV_SLOT_TICK, NULL);

	enum { BURSTS = 3 + 12 };	// 3 headers, two superframes
	unsigned fresh = 0, stale = 0, voice = 0;

	for (int n = 0; n < 2 * BURSTS + 3; n++) {
		uint8_t cs = (uint8_t)(n & 1);

		// The interrupt decides the slot LAYER2_STATUS b30 shows (gotchas 14.16): chip slot cs comes next
		REG(C7_LAYER2_STATUS) = (uint32_t)cs << 30;
		tick(false);
		bool flag = REG(C7_LAYER2_TXRX_CTRL) == 0x8040;
		int k = cs == 0 ? n / 2 : (n - 3) / 2;	// direct copy on chip slot 0, the repeat on chip slot 1

		if (k < 0 || (cs == 1 && n < 3) || k >= BURSTS) {
			count_kind(DMR_EV_SLOT_TICK, NULL);
			continue;
		}
		uint8_t fam = cs == 0 ? 2 : 3;	// TS1 sync on the talker's copy, TS2 on the repeat
		if (k < 3) {
			deframe(rti(0, DMR_BURST_VOICE_LC_HEADER, 2, fam, cs, 0));
			count_kind(DMR_EV_BURST_RX, NULL);
			continue;
		}
		int v = (k - 3) % 6;
		uint8_t mark = (uint8_t)(0x40 + (k - 3) * 2 + cs);	// burst and copy

		if (flag) {
			memset(&buf[C7_VOICE_RAM], mark, 27);
		}
		deframe(rti((uint8_t)(v + 1), 0, 2, v == 0 ? fam : 0, cs, 0));
		dmr_burst_t b;

		CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1 && b.kind == DMR_BURST_VOICE && b.voice_index == v);
		CHECK(b.ts == (cs == 0 ? 1 : 2));
		voice++;
		if (b.payload[0] == mark && b.payload[26] == mark) {
			fresh++;
		} else {
			stale++;
		}
	}
	CHECK(voice == 24 && fresh == 24 && stale == 0);
	// One copy ends with its terminator: its slot loses the flag, the other keeps it
	REG(C7_LAYER2_STATUS) = 0;
	tick(false);
	deframe(rti(0, DMR_BURST_TERMINATOR_LC, 2, 2, 0, 0));
	count_kind(DMR_EV_BURST_RX, NULL);
	REG(C7_LAYER2_STATUS) = 1u << 30;
	tick(false);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x8040);	// chip slot 1, the repeat still running
	REG(C7_LAYER2_STATUS) = 0;
	tick(false);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x40);	// chip slot 0, the talker's copy over
	// And with nothing more, the repeat's slot loses it too after the voice-lost time
	for (int i = 0; i < 16; i++) {
		REG(C7_LAYER2_STATUS) = (uint32_t)(i & 1) << 30;
		tick(false);
	}
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x40);
}

// The receiver drops its slot timing when it stops giving bursts: at the end of a reception and when the chip keeps
// the timing of a sender that left, reporting only unusable slots (bench: no later call was taken)
static void test_rx_resync(void)
{
	setup();
	const dmr_config_t cfg = { .own_id = 1234, .cc = 2, .access = DMR_ACCESS_POLITE_CC, .repeater = false };
	hrc7000_dmr_configure(&eng, &cfg);
	hrc7000_dmr_run(&eng);
	uint32_t rs = eng.st.rx_restarts;

	// A call: no restart while bursts come in, one about 390 ms after the last
	for (int i = 0; i < 40; i++) {
		tick(false);
		deframe(rti((uint8_t)(i % 6 + 1), 0, 2, 0, 0, 0));
	}
	CHECK(eng.st.rx_restarts == rs);
	for (int i = 0; i < 13; i++) {
		tick(false);
	}
	CHECK(eng.st.rx_restarts == rs && eng.st.rx_lost == 0);
	tick(false);
	CHECK(eng.st.rx_restarts == rs + 1 && eng.st.rx_lost == 1 && !eng.rx_any);
	CHECK(REG(C7_LAYER2_SLOTON) == 0x05 && REG(C7_LAYER2_CONTROL) == 0xC0);

	// Stale timing: ticks and unusable bursts only, a restart every 600 ms
	for (int i = 0; i < 20; i++) {
		tick(false);
		deframe((rti(0, 0, 2, 0, 0, 0) & ~3u) | 1u);
	}
	CHECK(eng.st.rx_stale == 0);
	tick(false);
	CHECK(eng.st.rx_stale == 1 && eng.st.rx_restarts == rs + 2);
	for (int i = 0; i < 21; i++) {
		tick(false);
	}
	CHECK(eng.st.rx_stale == 2);

	// A new call is taken and holds the receiver
	for (int i = 0; i < 30; i++) {
		tick(false);
		deframe(rti((uint8_t)(i % 6 + 1), 0, 2, 0, 0, 0));
	}
	CHECK(eng.st.rx_stale == 2 && eng.st.rx_lost == 1);

	// A restart after a transmission starts the count again (no restart at the first tick after it)
	count_kind(0xFF, NULL);
	for (int i = 0; i < 14; i++) {
		tick(false);
	}
	CHECK(eng.st.rx_lost == 2);
	hrc7000_dmr_rx_restart(&eng);
	tick(false);
	CHECK(eng.st.rx_stale == 2);
	count_kind(0xFF, NULL);
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
	for (uint8_t tc = 0; tc < 4; tc++) {	// own color code heard just now, the TC locked (test_tx_repeater_lock)
		tick(false);
		deframe(rti(1, 0, 1, 1, tc & 1u, tc & 1u));
	}
	CHECK(!hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 1, DMR_ACCESS_POLITE_CC));
	CHECK(count_kind(DMR_EV_TX_REJECTED, NULL) == 1 && eng.st.tx_rejected == 1);
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 1, DMR_ACCESS_IMPOLITE));
	hrc7000_dmr_tx_end(&eng);
	count_kind(0xFF, NULL);
	for (int i = 0; i < 13; i++) {
		tick(false);
	}
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 1, DMR_ACCESS_POLITE_CC));	// quiet for 390 ms
	CHECK(REG(C7_LAYER2_TXRX_CTRL) != 0);	// the TC lock went stale with the quiet: still receiving, waiting
	hrc7000_dmr_tx_abort(&eng);
	tick(false);
	CHECK(!hrc7000_dmr_is_transmitting(&eng));

	// Repeater end of transmission: the receiver restarts as stock does (both slot syncs released, 0xC4, receive), and
	// the next transmission waits for a new TC lock
	count_kind(0xFF, NULL);
	for (uint8_t tc = 0; tc < 4; tc++) {
		tick(false);
		deframe(rti(1, 0, 1, 1, tc & 1u, tc & 1u));
		count_kind(0xFF, NULL);		// a full event queue would leave the interrupt list set
	}
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 1, DMR_ACCESS_IMPOLITE));
	tick(false);				// locked: armed
	dmr_tx_burst_t c = { .kind = DMR_BURST_CSBK, .len = 10 };
	hrc7000_dmr_tx_queue(&eng, &c);
	for (int i = 0; i < 6 && hrc7000_dmr_is_transmitting(&eng); i++) {
		tick(true);
	}
	CHECK(!hrc7000_dmr_is_transmitting(&eng));
	CHECK(REG(C7_LAYER2_CONTROL) == 0xC4 && REG(C7_LAYER2_TXRX_CTRL) == 0x40 && REG(C7_LAYER2_SLOTON) == 0x05);
	CHECK(eng.tc_agree == 0);
	count_kind(0xFF, NULL);
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 1, DMR_ACCESS_IMPOLITE));
	tick(false);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) != 0 && !rf_state);	// waiting for the lock, still receiving
	hrc7000_dmr_tx_abort(&eng);
	tick(false);
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

// Retuning FM receive keeps a running PCM stream's bits: a beep's stream stalled when fm_rx cleared the mic PCM bit
static void test_fm_rx_keeps_pcm(void)
{
	setup();
	hrc7000_audio_bits(&dev, C7_AUDIO_PCM_MIC | C7_AUDIO_PCM_SPK, 0);
	hrc7000_fm_rx(&dev, false);
	CHECK((REG(C7_AUDIO_CONTROL) & (C7_AUDIO_PCM_MIC | C7_AUDIO_PCM_SPK)) == (C7_AUDIO_PCM_MIC | C7_AUDIO_PCM_SPK));
	CHECK(REG(C7_AUDIO_CONTROL) & C7_AUDIO_FM_PLAY);
}

// The AUDIO_CONTROL shadow changes only under the lock (hrc7000_set_crit): the FM and DMR entries too, so an
// interrupt (the PCM mic meter clearing b4) that comes at the unlock is never undone. The "interrupt" here runs in
// the exit hook, as a pending one does when the CPU's interrupts come back on
static int crit_depth, crit_enters, crit_unlocked_changes, crit_unwritten;
static uint32_t crit_seen;
static bool irq_pending;

static uint32_t crit_enter(void)
{
	if (crit_depth == 0 && dev.audio_ctl != crit_seen) {
		crit_unlocked_changes++;
	}
	crit_enters++;
	return (uint32_t)crit_depth++;
}

static void crit_exit(uint32_t s)
{
	if (REG(C7_AUDIO_CONTROL) != dev.audio_ctl) {
		crit_unwritten++;	// the register must hold the shadow before the lock goes
	}
	crit_depth = (int)s;
	crit_seen = dev.audio_ctl;
	if (crit_depth == 0 && irq_pending) {
		irq_pending = false;
		hrc7000_audio_bits(&dev, 0, C7_AUDIO_PCM_MIC);	// the meter's interrupt, nested in its own lock
	}
}

static void test_audio_lock(void)
{
	setup();
	CHECK(dev.crit_enter == NULL && dev.crit_exit == NULL);
	hrc7000_audio_bits(&dev, C7_AUDIO_PCM_MIC, 0);	// no lock set: plain update
	CHECK(REG(C7_AUDIO_CONTROL) & C7_AUDIO_PCM_MIC);
	hrc7000_set_crit(&dev, crit_enter, crit_exit);
	crit_depth = crit_enters = crit_unlocked_changes = crit_unwritten = 0;
	crit_seen = dev.audio_ctl;

	irq_pending = true;
	hrc7000_fm_rx(&dev, false);
	CHECK(!irq_pending && (REG(C7_AUDIO_CONTROL) & C7_AUDIO_PCM_MIC) == 0);	// the meter's clear stays
	CHECK(REG(C7_AUDIO_CONTROL) & C7_AUDIO_FM_PLAY);
	hrc7000_audio_bits(&dev, C7_AUDIO_PCM_MIC, 0);
	irq_pending = true;
	hrc7000_fm_tx(&dev, true);
	CHECK((REG(C7_AUDIO_CONTROL) & (C7_AUDIO_PCM_MIC | C7_AUDIO_FM_PLAY)) == 0);
	hrc7000_fm_idle(&dev);
	hrc7000_dmr_enter(&dev, 0, 11);
	uint32_t outer = crit_enter();	// a caller that holds the lock already: the nested update leaves it held
	hrc7000_audio_bits(&dev, C7_AUDIO_RING_PLAY, 0);
	CHECK(crit_depth == 1);
	crit_exit(outer);
	CHECK(crit_depth == 0 && crit_enters >= 7);
	CHECK(crit_unlocked_changes == 0 && crit_unwritten == 0);
	CHECK(REG(C7_AUDIO_CONTROL) == dev.audio_ctl && (dev.audio_ctl & C7_AUDIO_RING_PLAY));
}

// Mic gain: the board's value plus the user's offset, clamped to GCR_ADCL's 0..42 dB, written with bit 7 (shared)
static void test_mic_gain(void)
{
	setup();
	CHECK(hrc7000_mic_db(11, 0) == 11 && hrc7000_mic_db(11, 10) == 21 && hrc7000_mic_db(11, -10) == 1);
	CHECK(hrc7000_mic_db(5, -10) == 0 && hrc7000_mic_db(40, 10) == HRC7000_MIC_GAIN_MAX_DB);
	hrc7000_set_mic_gain(&dev, hrc7000_mic_db(11, 4));
	CHECK(buf[C7_CODEC + C7_CODEC_GCR_ADCL] == 0x8Fu);
	hrc7000_set_mic_gain(&dev, 60);
	CHECK(buf[C7_CODEC + C7_CODEC_GCR_ADCL] == (0x80u | HRC7000_MIC_GAIN_MAX_DB));
	hrc7000_dmr_enter(&dev, 0, hrc7000_mic_db(11, -6));
	CHECK(buf[C7_CODEC + C7_CODEC_GCR_ADCL] == 0x85u);
}

// Line out gain (GCR_DACL bits 5:0, bit 7 shared): dB to code both ways of 0 dB and clamped; a soft volume's steps (0 dB
// at the default step, 3 dB a step below, top_db spread above); the gain survives power-on init and FM receive, which
// wrote 0 dB over it before (the DB25-D's DMR voice ignored its volume)
static void test_dac_gain(void)
{
	CHECK(hrc7000_dac_code(0) == 0 && hrc7000_dac_code(-7) == 7 && hrc7000_dac_code(-31) == 31);
	CHECK(hrc7000_dac_code(-40) == 31 && hrc7000_dac_code(1) == 63 && hrc7000_dac_code(16) == 48);
	CHECK(hrc7000_dac_code(32) == 32 && hrc7000_dac_code(50) == 32);
	CHECK(hrc7000_volume_db(8, 8, 16, 12) == 0 && hrc7000_volume_db(7, 8, 16, 12) == -3);
	CHECK(hrc7000_volume_db(1, 8, 16, 12) == -21 && hrc7000_volume_db(0, 8, 16, 12) == HRC7000_DAC_MIN_DB);
	CHECK(hrc7000_volume_db(12, 8, 16, 12) == 6 && hrc7000_volume_db(16, 8, 16, 12) == 12);
	CHECK(hrc7000_volume_db(40, 8, 16, 12) == 12 && hrc7000_volume_db(1, 16, 16, 0) == HRC7000_DAC_MIN_DB);
	for (unsigned s = 2; s <= 16; s++) {
		CHECK(hrc7000_volume_db(s, 8, 16, 12) > hrc7000_volume_db(s - 1u, 8, 16, 12));	// every step is louder
	}
	setup();
	CHECK(dev.dac_gain == 0);
	hrc7000_set_dac_gain(&dev, hrc7000_dac_code(-9));
	CHECK(buf[C7_CODEC + C7_CODEC_GCR_DACL] == 0x89u && dev.dac_gain == 9);
	buf[C7_CODEC + C7_CODEC_GCR_DACL] = 0;
	hrc7000_power_on_init(&dev, 128, 20);
	CHECK(buf[C7_CODEC + C7_CODEC_GCR_DACL] == 0x89u);
	buf[C7_CODEC + C7_CODEC_GCR_DACL] = 0;
	hrc7000_fm_rx(&dev, false);
	CHECK(buf[C7_CODEC + C7_CODEC_GCR_DACL] == 0x89u);
	hrc7000_set_dac_gain(&dev, hrc7000_dac_code(12));
	CHECK(buf[C7_CODEC + C7_CODEC_GCR_DACL] == (0x80u | 52u));
}

// Codec mixer route for CPU-made FM TX audio: the last mixer write on is MIX_2 mix_rec (MIX_3 went first), the mic
// mute bit is left alone (the mix zeroes the ADC's input instead), and off leaves the mixer disabled
static void test_codec_dac_to_tx(void)
{
	setup();
	buf[C7_CODEC + C7_CODEC_CR_ADC] = 0x00;
	hrc7000_codec_dac_to_tx(&dev, true);
	CHECK(buf[C7_CODEC + C7_CODEC_CR_MIX] == 0xC2u && buf[C7_CODEC + C7_CODEC_DR_MIX] == 0x01u);
	CHECK(buf[C7_CODEC + C7_CODEC_CR_ADC] == 0x00);
	hrc7000_codec_dac_to_tx(&dev, false);
	CHECK(buf[C7_CODEC + C7_CODEC_CR_MIX] == 0x00 && buf[C7_CODEC + C7_CODEC_DR_MIX] == 0x00);
}

// FM TX limiter: the KP value unless the board set one, per bandwidth
static void test_fm_tx_limit(void)
{
	setup();
	hrc7000_fm_tx(&dev, true);
	CHECK(REG(C7_FM_DEV_COEF) == 0x40A20u);
	hrc7000_fm_tx(&dev, false);
	CHECK(REG(C7_FM_DEV_COEF) == 0x40A25u);
	dev.fm_tx_limit[0] = 5;
	dev.fm_tx_limit[1] = 0x0F;
	hrc7000_fm_tx(&dev, true);
	CHECK(REG(C7_FM_DEV_COEF) == 0xF0A20u);
	hrc7000_fm_tx(&dev, false);
	CHECK(REG(C7_FM_DEV_COEF) == 0x50A25u);
	dev.fm_tx_limit[0] = dev.fm_tx_limit[1] = 0;
}

// RF_CONTROL: the RF TX interrupt 3.1 ms ahead unless the board set its lead, the other fields unchanged. The setter
// reaches the chip after power-on (a board once set the field after it, and kept 3.1 ms), and power-on keeps it
static void test_rf_tx_lead(void)
{
	setup();
	hrc7000_power_on_init(&dev, 128, 20);
	CHECK(REG(C7_RF_CONTROL) == 0x00041F1Au);
	hrc7000_set_rf_tx_lead(&dev, 4);	// a board's 0.4 ms
	CHECK(REG(C7_RF_CONTROL) == 0x0004041Au);
	hrc7000_power_on_init(&dev, 128, 20);
	CHECK(REG(C7_RF_CONTROL) == 0x0004041Au);
	hrc7000_set_rf_tx_lead(&dev, 0);
	CHECK(REG(C7_RF_CONTROL) == 0x00041F1Au);
}

// FM TX tone: DTMF digit 1 with both tones at the step for hz, the DTMF deviation while it plays, everything back after
static void test_tx_tone(void)
{
	setup();
	REG(C7_SUBVOICE_COEF) = 0x07D00930u;
	hrc7000_tx_tone(&dev, 1750, hrc7000_dtmf_dev(467375000u, false));
	CHECK(REG(C7_FM_TONE1) >> 16 == 3584u && REG(C7_FM_TONE3) >> 16 == 3584u);	// round(1750 x 2.048)
	CHECK((REG(C7_FM_TONE1) & 0xFFFFu) == 0x0629u && (REG(C7_FM_TONE3) & 0xFFFFu) == 0x0AB0u);
	CHECK(REG(C7_SUBVOICE_COEF) == 0x07D0C030u);
	CHECK(REG(C7_FM_MOD) & 0x2u);
	CHECK(REG(C7_TONE_SEND_CTRL) == 0x81u);	// digit 1 on, voice muted
	hrc7000_tx_tone(&dev, 1200, 0xC0);	// a new tone keeps the saved deviation
	hrc7000_tx_tone_key(&dev, false);
	CHECK(REG(C7_TONE_SEND_CTRL) == 0x01u);
	hrc7000_tx_tone_key(&dev, true);
	CHECK(REG(C7_TONE_SEND_CTRL) == 0x81u);
	hrc7000_fm_idle(&dev);
	CHECK(REG(C7_TONE_SEND_CTRL) == 0 && (REG(C7_FM_MOD) & 0x2u) == 0);
	CHECK(REG(C7_FM_TONE1) == 0x05930629u && REG(C7_FM_TONE3) == 0x09AC0AB0u);
	CHECK(REG(C7_SUBVOICE_COEF) == 0x07D00930u && !dev.tone_on);
	hrc7000_tx_tone_key(&dev, true);	// no tone: nothing
	CHECK(REG(C7_TONE_SEND_CTRL) == 0);
	CHECK(hrc7000_dtmf_dev(446000000u, false) == 0xC0u && hrc7000_dtmf_dev(446000000u, true) == 0xFFu);

	// DTMF (radio-features.md 3.7): the low tone's step in FM_TONE1, the high one's in FM_TONE3, digit 1 sent with
	// the voice muted (b6 = 0) while it sounds; another digit retunes; 0 ends it with the microphone back and the
	// defaults restored; a digit outside the table changes nothing
	setup();
	REG(C7_SUBVOICE_COEF) = 0x07D00930u;
	CHECK(hrc7000_tx_dtmf(&dev, 'D', 0xC0));
	CHECK(REG(C7_FM_TONE1) >> 16 == 1927u && REG(C7_FM_TONE3) >> 16 == 3344u);	// round(941 / 1633 x 2.048)
	CHECK(REG(C7_TONE_SEND_CTRL) == 0x81u && (REG(C7_FM_MOD) & 0x2u) && REG(C7_SUBVOICE_COEF) == 0x07D0C030u);
	CHECK(hrc7000_tx_dtmf(&dev, '1', 0xC0));
	CHECK(REG(C7_FM_TONE1) >> 16 == 1427u && REG(C7_FM_TONE3) >> 16 == 2476u && REG(C7_TONE_SEND_CTRL) == 0x81u);
	CHECK(!hrc7000_tx_dtmf(&dev, 'E', 0xC0) && REG(C7_FM_TONE1) >> 16 == 1427u && dev.tone_on);
	CHECK(hrc7000_tx_dtmf(&dev, 0, 0));
	CHECK(REG(C7_TONE_SEND_CTRL) == 0 && (REG(C7_FM_MOD) & 0x2u) == 0 && !dev.tone_on);
	CHECK(REG(C7_FM_TONE1) == 0x05930629u && REG(C7_FM_TONE3) == 0x09AC0AB0u && REG(C7_SUBVOICE_COEF) == 0x07D00930u);
	CHECK(hrc7000_tx_dtmf(&dev, 0, 0) && REG(C7_TONE_SEND_CTRL) == 0);	// no tone: nothing
}

// Sub-audio: DCS TX code in CDCSS_ADDR b8:0 and RX in b24:16 (the guide); the sub-audio deviation doubles at 25 kHz
static void test_subtone(void)
{
	setup();
	hrc7000_set_subtone(&dev, true, HRC7000_TONE_DCS, 0023);
	CHECK(REG(C7_CDCSS_ADDR) == 0023u && (REG(C7_FM_MOD) & 0x4u));
	hrc7000_set_subtone(&dev, false, HRC7000_TONE_DCS, 0754);
	CHECK(REG(C7_CDCSS_ADDR) == ((0754u << 16) | 0023u));
	hrc7000_set_subtone(&dev, true, HRC7000_TONE_DCS_INV, 0023);
	CHECK(REG(C7_FM_MOD) & (1u << 11));
	// The turn-off tail: b9 on with DCS; FM_PTT off for the tail, on again for PTT again. None without DCS
	CHECK(REG(C7_FM_MOD) & (1u << 9));
	REG(C7_FM_PTT) = 1;
	CHECK(hrc7000_tx_tail(&dev, true) && REG(C7_FM_PTT) == 0);
	CHECK(hrc7000_tx_tail(&dev, false) && REG(C7_FM_PTT) == 1);
	hrc7000_set_subtone(&dev, true, HRC7000_TONE_CTCSS, 1000);
	CHECK(!hrc7000_tx_tail(&dev, true) && REG(C7_FM_PTT) == 1);
	hrc7000_set_subtone(&dev, true, HRC7000_TONE_DCS_INV, 0023);
	// DCS gets three times the CTCSS value (the field sends it at a third of the deviation, 100 Hz at 0x28), up to
	// 0x7F: DCS reads the field as signed, and 0xF0 sent the code inverted
	dev.subaudio_dev = 0x28;
	REG(C7_SUBVOICE_COEF) = 0x07D02810u;
	hrc7000_fm_tx(&dev, true);
	CHECK(REG(C7_SUBVOICE_COEF) == 0x07D07F10u);
	hrc7000_fm_tx(&dev, false);
	CHECK(REG(C7_SUBVOICE_COEF) == 0x07D07810u);
	hrc7000_set_subtone(&dev, true, HRC7000_TONE_CTCSS, 1000);
	hrc7000_fm_tx(&dev, true);
	CHECK(REG(C7_SUBVOICE_COEF) == 0x07D05010u);
	hrc7000_fm_tx(&dev, false);
	CHECK(REG(C7_SUBVOICE_COEF) == 0x07D02810u);
	dev.subaudio_dev = 0x90;
	hrc7000_fm_tx(&dev, true);	// capped at the field's top
	CHECK(REG(C7_SUBVOICE_COEF) == 0x07D0FF10u);
	// A warm restart finds the last 25 kHz value in the register: the init sets the reset value, not that
	hrc7000_power_on_init(&dev, 128, 20);
	CHECK(dev.subaudio_dev == 0x28 && REG(C7_SUBVOICE_COEF) == 0x07D02810u);
	hrc7000_fm_tx(&dev, false);
	CHECK(REG(C7_SUBVOICE_COEF) == 0x07D02810u);
}

// FM receive audio (8.5): the half FM_ADDR_SW names, high byte first; the interrupt bit in the mask; the engine's
// system interrupt calls the hook for bit 14 and clears it, running or not
static int fm_hook_calls;
static void fm_hook(void) { fm_hook_calls++; REG(C7_SYS_INTERP_LIST) = 0; }	// the chip clears it on the write back

static void test_fm_rx_audio(void)
{
	setup();
	int16_t out[HRC7000_FM_RX_SAMPLES];
	for (uint32_t i = 0; i < HRC7000_FM_RX_SAMPLES; i++) {
		uint16_t a = (uint16_t)(i * 100u), b = (uint16_t)(0x8000u + i);
		buf[C7_FM_RX_AUDIO + 2u * i] = (uint8_t)(a >> 8);
		buf[C7_FM_RX_AUDIO + 2u * i + 1u] = (uint8_t)a;
		buf[C7_FM_RX_AUDIO + 512u + 2u * i] = (uint8_t)(b >> 8);
		buf[C7_FM_RX_AUDIO + 512u + 2u * i + 1u] = (uint8_t)b;
	}
	CHECK(C7_FM_RX_AUDIO == 0x4E0u && C7_FM_RX_AUDIO + 1024u == 0x8E0u);	// the datasheet's 0x160004E0..0x160008DF
	REG(C7_FM_ADDR_SW) = 0;
	CHECK(hrc7000_fm_rx_audio(&dev, out) == 0);
	REG(C7_FM_ADDR_SW) = 1;
	CHECK(hrc7000_fm_rx_audio(&dev, out) == HRC7000_FM_RX_SAMPLES);
	CHECK(out[0] == 0 && out[1] == 100 && out[255] == 25500);
	REG(C7_FM_ADDR_SW) = 2;
	CHECK(hrc7000_fm_rx_audio(&dev, out) == HRC7000_FM_RX_SAMPLES);
	CHECK(out[0] == -32768 && out[255] == -32768 + 255);
	REG(C7_FM_ADDR_SW) = 3;
	CHECK(hrc7000_fm_rx_audio(&dev, out) == 0);

	REG(C7_SYS_INTERP_MASK) = 0x1187Fu;
	hrc7000_fm_rx_audio_irq(&dev, true);
	CHECK(REG(C7_SYS_INTERP_MASK) == (0x1187Fu | C7_SYS_FM_RX));
	hrc7000_fm_rx_audio_irq(&dev, false);
	CHECK(REG(C7_SYS_INTERP_MASK) == 0x1187Fu);

	fm_hook_calls = 0;
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX;
	hrc7000_dmr_isr_sys(&eng);	// no hook: cleared, nothing else
	CHECK(REG(C7_SYS_INTERP_CLEAR) == C7_SYS_FM_RX && fm_hook_calls == 0);
	eng.fm_rx = fm_hook;
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX;
	REG(C7_SYS_INTERP_CLEAR) = 0;
	hrc7000_dmr_isr_sys(&eng);	// engine not running (FM): the hook still runs
	CHECK(fm_hook_calls == 1 && REG(C7_SYS_INTERP_CLEAR) == C7_SYS_FM_RX);
	REG(C7_SYS_INTERP_LIST) = 1u << 5;
	hrc7000_dmr_isr_sys(&eng);	// another bit: no call
	CHECK(fm_hook_calls == 1);
}

// Prompt FIFO push (8.4): 32 samples low byte first while AUDIO_CONTROL b31 is clear; nothing written when full
static void test_prompt_push(void)
{
	setup();
	int16_t s[C7_PROMPT_CHUNK];
	for (uint32_t i = 0; i < C7_PROMPT_CHUNK; i++) {
		s[i] = (int16_t)(0x1234 + (int32_t)i - 0x1000 * (int32_t)(i & 1u));
	}
	CHECK(hrc7000_prompt_push(&dev, s));
	CHECK(buf[C7_PROMPT_FIFO] == 0x34 && buf[C7_PROMPT_FIFO + 1] == 0x12);
	CHECK(buf[C7_PROMPT_FIFO + 2] == (uint8_t)(s[1] & 0xFF) && buf[C7_PROMPT_FIFO + 3] == (uint8_t)((uint16_t)s[1] >> 8));
	CHECK(buf[C7_PROMPT_FIFO + 62] == (uint8_t)(s[31] & 0xFF));
	memset(&buf[C7_PROMPT_FIFO], 0, 64);
	REG(C7_AUDIO_CONTROL) = C7_AUDIO_RING_FULL;
	CHECK(!hrc7000_prompt_push(&dev, s) && buf[C7_PROMPT_FIFO] == 0);
}

// An SFR's repeat: the send sync follows each burst's family (MS when none is asked), and with tx_listen the other
// slot is received between our bursts; without it, nothing is taken while sending
static void test_tx_sync_and_listen(void)
{
	setup();
	hrc7000_dmr_run(&eng);
	const dmr_config_t cfg = { .own_id = 1234, .cc = 1, .access = DMR_ACCESS_IMPOLITE, .repeater = false };
	hrc7000_dmr_configure(&eng, &cfg);
	tick(false);
	count_kind(0xFF, NULL);

	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 2, DMR_ACCESS_IMPOLITE));
	dmr_tx_burst_t h = { .kind = DMR_BURST_VOICE_LC_HEADER, .len = 9, .sync_override = DMR_SYNC_TS2 };
	dmr_tx_burst_t a = voice(0, 0x11);
	CHECK(hrc7000_dmr_tx_queue(&eng, &h) && hrc7000_dmr_tx_queue(&eng, &a));
	tick(true);
	CHECK(REG(C7_SEND_DATA_SYNC_H) == 0xD7557F && REG(C7_SEND_DATA_SYNC_L) == 0x5FF7F5);
	// The other slot: closed, and a burst heard there is not taken
	tick(true);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0);
	deframe(rti(0, DMR_BURST_VOICE_LC_HEADER, 1, 2, 0, 0));
	CHECK(count_kind(DMR_EV_BURST_RX, NULL) == 0);
	// Voice A without an override: MS sync again
	tick(true);
	CHECK(REG(C7_SEND_DATA_SYNC_H) == 0xD5D7F7 && REG(C7_SEND_DATA_SYNC_L) == 0x7FD757);

	CHECK(REG(C7_LAYER2_CONTROL) == 0xC3);	// aligned with nothing received: our own timing
	hrc7000_dmr_tx_listen(&eng);
	CHECK(REG(C7_LAYER2_CONTROL) == 0xCB);	// the receiver follows the other station's slot edges
	tick(true);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x40);
	deframe(rti(0, DMR_BURST_VOICE_LC_HEADER, 1, 2, 0, 0));
	dmr_burst_t b;
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1 && b.ts == 1 && b.sync == DMR_SYNC_TS1);
	// The call's voice follows: its slot gets the voice-access flag (0x40 alone loses bursts B-F), until the terminator
	tick(true);
	tick(true);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x8040);
	// Its terminator with the chip's RS flag (wrong at times while we send, gotchas 14.1): passed on, marked bad
	deframe(rti(0, DMR_BURST_TERMINATOR_LC, 1, 2, 0, 0) | (1u << 25));
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1 && b.kind == DMR_BURST_TERMINATOR_LC && !b.crc_ok);
	tick(true);
	tick(true);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x40);

	// A new transmission listens only when asked again, and writes its first sync even if unchanged
	hrc7000_dmr_tx_abort(&eng);
	tick(true);
	CHECK(!hrc7000_dmr_is_transmitting(&eng));
	REG(C7_SEND_DATA_SYNC_H) = 0;
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	CHECK(REG(C7_LAYER2_CONTROL) == 0xC3);
	CHECK(hrc7000_dmr_tx_queue(&eng, &a));
	tick(true);
	CHECK(REG(C7_SEND_DATA_SYNC_H) == 0xD5D7F7);
	tick(true);
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0);
	hrc7000_dmr_tx_listen(&eng);
	CHECK(REG(C7_LAYER2_CONTROL) == 0xCB);	// own timing: the receiver follows the other station's slot edges
	hrc7000_dmr_tx_abort(&eng);
	tick(true);
	hrc7000_dmr_tx_listen(&eng);	// not sending: ignored
	CHECK(!eng.tx_listen);
	count_kind(0xFF, NULL);
}

// An aligned start (an SFR's repeat): LAYER2_CONTROL left passive so the receiver keeps its slot sync, the TX slot
// built from the receive slot (SLOTON b5), and our slot picked by its number in LAYER2_STATUS b31: the other chip slot
// than the received bursts' for another TS, the same one for the same TS
static void test_tx_aligned(void)
{
	for (int same_ts = 0; same_ts < 2; same_ts++) {
		setup();
		hrc7000_dmr_run(&eng);
		const dmr_config_t cfg = { .own_id = 1234, .cc = 1, .access = DMR_ACCESS_IMPOLITE, .repeater = false };
		hrc7000_dmr_configure(&eng, &cfg);
		tick(false);
		deframe(rti(0, DMR_BURST_VOICE_LC_HEADER, 1, 2, 1, 0));	// TS1 sync, chip slot 1
		dmr_burst_t b;
		CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1 && b.ts == 1);
		count_kind(0xFF, NULL);
		REG(C7_LAYER2_CONTROL) = 0xC0;
		REG(C7_LAYER2_SLOTON) = 0;
		CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, same_ts ? 1 : 2, DMR_ACCESS_IMPOLITE));
		CHECK(REG(C7_LAYER2_CONTROL) == 0xC0 && REG(C7_LAYER2_SLOTON) == 0x25);
		hrc7000_dmr_tx_listen(&eng);
		CHECK(REG(C7_LAYER2_CONTROL) == 0xC0);
		dmr_tx_burst_t a = voice(0, 0x11);
		CHECK(hrc7000_dmr_tx_queue(&eng, &a));
		REG(C7_LAYER2_STATUS) = 1u << 31;	// the coming slot: chip slot 1, the received one
		tick(true);
		CHECK(REG(C7_LAYER2_TXRX_CTRL) == (same_ts ? 0x80u : 0x40u));
		// What the HIL test reads (diag txh): the first TX tick a slot after the last receive one, b31 and whether
		// the next slot was ours
		CHECK(eng.st.txa_rx_us == 300 && eng.st.txa_est == (same_ts ? 3u : 1u));
		tick(true);
		CHECK(REG(C7_LAYER2_TXRX_CTRL) == (same_ts ? 0x40u : 0x80u));
		hrc7000_dmr_tx_abort(&eng);
		tick(true);
		count_kind(0xFF, NULL);
	}
}

// Through a repeater (BS-sourced bursts): the TX slot comes from the CACH TC, locked over TC_LOCK bursts that
// alternate; an aligned start before the lock keeps receiving and arms once locked, and is refused (nothing sent)
// when the lock never comes. A single burst's TC is not trusted (issue 72)
static void deframe_at(uint32_t info, uint32_t us)	// us after the last timeslot interrupt
{
	now += us;
	deframe(info);
	now -= us;
}

static void test_tx_repeater_lock(void)
{
	// The repeater's slots: slot n (started at the n-th tick from here) carries TC n & 1. Its burst's deframe comes
	// late in slot n, or just after the interrupt that starts slot n + 1; the first TX-vector interrupt a slot after
	// the last receive one, or 5 ms after it in the same slot: every way the same lock and TX slot
	for (int mode = 0; mode < 4; mode++) {
		int early = mode & 1, same = mode >> 1;
		for (int ts = 1; ts <= 2; ts++) {
			setup();
			hrc7000_dmr_run(&eng);
			const dmr_config_t cfg = { .own_id = 1234, .cc = 1, .access = DMR_ACCESS_IMPOLITE, .repeater = true };
			hrc7000_dmr_configure(&eng, &cfg);
			CHECK(REG(C7_LAYER2_CONTROL) == 0xC4);	// through a repeater: TX slots follow the receiver
			int n = -1;
			REG(C7_LAYER2_SLOTON) = 0;
			for (int k = 0; k < 4; k++) {		// four bursts, TC alternating: locked with the last
				tick(false);
				n++;
				if (early) {
					if (k > 0) {
						deframe_at(rti(0, 9, 1, 1, 0, (uint8_t)((k - 1) & 1)), 2000);
					}
				} else {
					deframe_at(rti(0, 9, 1, 1, 0, (uint8_t)(k & 1)), 25000);
				}
				if (k == early) {	// the call starts after the first burst: it waits, receiving, for the lock
					CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, (int8_t)ts, DMR_ACCESS_IMPOLITE));
					dmr_tx_burst_t a = voice(0, 0x11);
					CHECK(hrc7000_dmr_tx_queue(&eng, &a));
				}
				count_kind(0xFF, NULL);
			}
			if (early) {
				tick(false);
				n++;
				deframe_at(rti(0, 9, 1, 1, 0, 1), 2000);
			}
			CHECK(eng.st.rx_lc_header == 0 && hrc7000_dmr_is_transmitting(&eng) && REG(C7_LAYER2_SLOTON) == 0);
			tick(false);				// arms: no LAYER2 write but TXRX
			n++;
			CHECK(REG(C7_LAYER2_SLOTON) == 0 && REG(C7_LAYER2_CONTROL) == 0xC4 && REG(C7_LAYER2_TXRX_CTRL) == 0);
			REG(C7_LAYER2_STATUS) = 1u << 31;	// not used through a repeater
			for (int i = 0; i < 2; i++) {		// the TX slot decided at each tick is the next one, n + 1
				if (i == 0 && same) {
					now -= 25000;		// 5 ms after the receive tick: the same slot
				} else {
					n++;
				}
				tick(true);
				CHECK(REG(C7_LAYER2_TXRX_CTRL) == (((n + 1) & 1) == ts - 1 ? 0x80u : 0u));
			}
			hrc7000_dmr_tx_abort(&eng);
			tick(true);
			count_kind(0xFF, NULL);
		}
	}

	// No lock (TC stuck): refused after the wait, nothing armed
	setup();
	hrc7000_dmr_run(&eng);
	const dmr_config_t cfg = { .own_id = 1234, .cc = 1, .access = DMR_ACCESS_IMPOLITE, .repeater = true };
	hrc7000_dmr_configure(&eng, &cfg);
	tick(false);
	deframe(rti(0, 9, 1, 1, 0, 0));
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 1, DMR_ACCESS_IMPOLITE));
	int rejected = 0;
	for (int i = 0; i < 40 && hrc7000_dmr_is_transmitting(&eng); i++) {
		tick(false);
		deframe(rti(0, 9, 1, 1, (uint8_t)(i & 1), 0));
		rejected += count_kind(DMR_EV_TX_REJECTED, NULL);
	}
	CHECK(!hrc7000_dmr_is_transmitting(&eng) && eng.st.tx_rejected == 1 && rf_on_calls == 0 && rejected == 1);
}

// An SFR's gap: a queued DMR_BURST_EMPTY leaves our slot empty and received (nothing armed, no PA), still counts as
// our slot for the caller, and tx_fill puts a burst in the oldest placeholder (gotchas 14.4, 14.10)
static void test_tx_empty_and_fill(void)
{
	setup();
	hrc7000_dmr_run(&eng);
	const dmr_config_t cfg = { .own_id = 1234, .cc = 1, .access = DMR_ACCESS_IMPOLITE, .repeater = false };
	hrc7000_dmr_configure(&eng, &cfg);
	tick(false);
	count_kind(0xFF, NULL);

	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 2, DMR_ACCESS_IMPOLITE));
	hrc7000_dmr_tx_listen(&eng);
	dmr_tx_burst_t a = voice(0, 0x11), b = voice(1, 0x22), c = voice(2, 0x33);
	dmr_tx_burst_t empty = { .kind = DMR_BURST_EMPTY };
	CHECK(hrc7000_dmr_tx_queue(&eng, &a));
	tick(true);	// A armed
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x80);
	uint32_t bursts = eng.st.tx_bursts;
	tick(true);	// the other slot
	CHECK(hrc7000_dmr_tx_queue(&eng, &empty) && hrc7000_dmr_tx_queue(&eng, &empty));
	CHECK(hrc7000_dmr_tx_fill(&eng, &b) == 1);	// the first placeholder taken, one queued after it
	tick(true);	// B armed in it
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x80 && eng.st.tx_bursts == bursts + 1);
	tick(true);
	count_kind(0xFF, NULL);
	tick(true);	// the second placeholder: received, not sent, still a slot of ours
	CHECK(REG(C7_LAYER2_TXRX_CTRL) == 0x40 || REG(C7_LAYER2_TXRX_CTRL) == 0x8040);
	CHECK(eng.st.tx_bursts == bursts + 1 && eng.st.tx_empty == 1);
	CHECK(count_kind(DMR_EV_TX_SLOT_STARTED, NULL) == 1);
	CHECK(hrc7000_dmr_tx_fill(&eng, &c) == -1);	// no placeholder left
	CHECK(hrc7000_dmr_is_transmitting(&eng));
	hrc7000_dmr_tx_abort(&eng);
	tick(true);
	count_kind(0xFF, NULL);
}

// A received burst's arg is the tick of the slot after its own (hrc7000_dmr.h): a deframe late in a slot belongs to
// that slot, one early in a slot to the one before. The split duplex RX half times its labels from it; the slot clock
// gives the last boundary
static void test_rx_slot_arg(void)
{
	setup();
	hrc7000_dmr_run(&eng);
	uint32_t c, t;
	int8_t ts;
	CHECK(!hrc7000_dmr_slot_clock(&eng, &c, &t, &ts));	// no slot clock before the first tick
	tick(false);
	tick(false);
	CHECK(hrc7000_dmr_slot_clock(&eng, &c, &t, &ts) && c == 2 && t == now && ts == 0);
	count_kind(DMR_EV_SLOT_TICK, NULL);
	dmr_event_t evs[4];
	dmr_burst_t bs[4];
	now += 3000;			// 3 ms into slot 2: the burst of slot 1
	deframe(rti(1, 0, 1, 0, 0, 0));
	int n = drain(evs, bs, 4);
	CHECK(n == 1 && evs[0].kind == DMR_EV_BURST_RX && evs[0].arg == 2);
	now += 24000;			// 27 ms into slot 2: its own burst
	deframe(rti(2, 0, 1, 0, 0, 0));
	n = drain(evs, bs, 4);
	CHECK(n == 1 && evs[0].arg == 3);
}

// A whole call through a chip model: the chip writes a burst's voice into the voice RAM only when the voice-access flag
// (LAYER2_TXRX_CTRL b15) was set at the timeslot interrupt that decided that burst's slot, the one LAYER2_STATUS b30
// shows then. Every burst's payload must be the voice the chip wrote for it. With the flag on the other slot
// (HRC7000_RX_NEXT_SLOT_XOR 1, one board's old default) the RAM keeps the bytes it had and every burst of
// the call reads them again: silence with every counter clean
static void test_rx_voice_ram_call(void)
{
	setup();
	hrc7000_dmr_run(&eng);
	const dmr_config_t cfg = { .own_id = 1234, .cc = 3, .access = DMR_ACCESS_POLITE_CC, .repeater = false };
	hrc7000_dmr_configure(&eng, &cfg);
	memset(buf + C7_VOICE_RAM, 0xA5, 27);		// burst A's voice: the chip takes it with the sync, no flag needed
	tick(false);
	deframe(rti(1, 0, 3, 0, 1, 0));			// voice A on chip slot 1 opens the call
	dmr_burst_t b;
	CHECK(count_kind(DMR_EV_BURST_RX, &b) == 1 && b.payload[0] == 0xA5);
	int fresh = 0, bursts = 0;
	for (int k = 1; k < 12; k++) {			// B-F, then a whole superframe more
		REG(C7_LAYER2_STATUS) = 1u << 30;	// chip slot 1 starts: the decision for it
		tick(false);
		bool flag = (REG(C7_LAYER2_TXRX_CTRL) & 0x8000u) != 0;
		if (flag) {
			memset(buf + C7_VOICE_RAM, k, 27);	// the chip writes this burst's voice
		}
		deframe(rti((uint8_t)(k % 6 + 1), 0, 3, 0, 1, 0));
		if (count_kind(DMR_EV_BURST_RX, &b) == 1) {
			bursts++;
			fresh += b.len == 27 && b.payload[0] == k && b.payload[26] == k;
		}
		REG(C7_LAYER2_STATUS) = 0;		// chip slot 0 starts: nothing for us there
		tick(false);
		CHECK((REG(C7_LAYER2_TXRX_CTRL) & 0x8000u) == 0);
		count_kind(DMR_EV_SLOT_TICK, NULL);
	}
	CHECK(bursts == 11);
	CHECK(fresh == 11);	// every burst's own voice, never the RAM's old bytes
	if (fresh != 11) {
		printf("  voice RAM call: %d of %d bursts carried their own voice (the flag on the wrong slot?)\n", fresh,
		       bursts);
	}
}

// LAYER2_BS_MODE survives a chip reset (modem-hrc7000.md 2.9): the power-on set-up writes it back to its reset value
static void test_bs_mode_cleared(void)
{
	setup();
	REG(C7_LAYER2_BS_MODE) = 0x904;	// BS TX left on by an earlier build
	REG(C7_BS_CACH) = 3;
	hrc7000_power_on_init(&dev, 128, 0);
	CHECK(REG(C7_LAYER2_BS_MODE) == C7_BS_MODE_RESET);
	CHECK(REG(C7_BS_CACH) == 0);
}

int main(void)
{
	test_pure();
	test_rx();
	test_rx_sfr_both_copies();
	test_rx_slot_arg();
	test_rx_voice_ram_call();
	test_bs_mode_cleared();
	test_rx_resync();
	test_cc_scan();
	test_tx();
	test_tx_edges();
	test_tx_sync_and_listen();
	test_tx_aligned();
	test_tx_repeater_lock();
	test_tx_empty_and_fill();
	test_polite_and_repeater();
	test_event_overflow();
	test_fm_rx_keeps_pcm();
	test_mic_gain();
	test_dac_gain();
	test_codec_dac_to_tx();
	test_audio_lock();
	test_fm_tx_limit();
	test_rf_tx_lead();
	test_tx_tone();
	test_subtone();
	test_fm_rx_audio();
	test_prompt_push();
	if (failures) {
		printf("hrc7000DmrTest: %d failures\n", failures);
		return 1;
	}
	printf("hrc7000DmrTest: all passed\n");
	return 0;
}
