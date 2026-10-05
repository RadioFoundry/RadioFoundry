// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Host test of the MMDVM hotspot's SFR mode (core/hotspot/hotspotSfr.c on core/dmr/sfrRunner.c and core/dmr/sfr.c),
// through the MMDVM protocol (mmdvm.c) and the hotspot engine (hotspot.c), against the simulated DMR modem: the
// trigger (duplex, RX = TX), the version suffix, the status bytes, an RF call repeated on the outbound slot and sent
// up as DMR_DATA1, a network stream on the outbound slot, RF priority (NAK 6, status 0x80), own echo, late entry,
// command 0xD5, DMR_ABORT, and back to simplex. docs/spec/mmdvm-protocol.md 6, docs/spec/sfr.md.
#include <stdio.h>
#include <string.h>
#include "hotspot/mmdvm.h"
#include "hotspot/hotspot.h"
#include "hotspot/hotspotSfr.h"
#include "hotspot/dmrDataFrame.h"
#include "dmr/dmrData.h"
#include "dmr/fec/dmrFec.h"
#include "dmr/call/dmr_lc.h"
#include "sim/dmr_sim_modem.h"
#if CONFIG_HOTSPOT_SPLIT
#include "hotspot/hotspotSplit.h"
#define SUITE "hotspotSfrTestSplit"
#else
#define SUITE "hotspotSfrTest"
#endif

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// ---- Frames to the host ----

#define MAX_OUT 1024
static struct { uint8_t b[MMDVM_FRAME_MAX]; size_t len; } out[MAX_OUT];
static int out_count;

static void io_write(const uint8_t *buf, size_t len)
{
	if (out_count < MAX_OUT) {
		memcpy(out[out_count].b, buf, len);
		out[out_count].len = len;
		out_count++;
	}
}

static const mmdvm_io_t io = { .write = io_write, .description = "OpenDigitalRadio Test 0123abcd" };

static bool r_enter(uint32_t rx) { (void)rx; return true; }
static bool r_set_freq(uint32_t rx, uint32_t tx) { (void)rx; (void)tx; return true; }
static void r_leave(void) {}
static const hotspot_radio_ops_t radio_ops = { .enter = r_enter, .set_freq = r_set_freq, .leave = r_leave };

static uint32_t now = 1000;

static void host(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
	uint8_t f[MMDVM_FRAME_MAX];

	f[0] = 0xE0;
	f[1] = (uint8_t)(len + 3);
	f[2] = cmd;
	if (len) {
		memcpy(f + 3, payload, len);
	}
	mmdvm_frame(f, (uint16_t)(len + 3), now);
}

// One 30 ms slot of the simulated modem, then the host link's poll
static void slot(void)
{
	sim_slot();
	now += 30;
	mmdvm_poll(now);
}

static void slots(int n)
{
	while (n-- > 0) {
		slot();
	}
}

static const uint8_t *last_out(uint8_t cmd)
{
	for (int i = out_count - 1; i >= 0; i--) {
		if (out[i].b[2] == cmd) {
			return out[i].b;
		}
	}
	return NULL;
}

static int count_out(uint8_t cmd, int from)
{
	int n = 0;

	for (int i = from; i < out_count; i++) {
		n += out[i].b[2] == cmd;
	}
	return n;
}

static bool reply(uint8_t kind, uint8_t cmd, int from)
{
	for (int i = from; i < out_count; i++) {
		if ((out[i].b[2] == kind) && (out[i].b[3] == cmd)) {
			return true;
		}
	}
	return false;
}

static int nak_reason(uint8_t cmd, int from)
{
	for (int i = from; i < out_count; i++) {
		if ((out[i].b[2] == MMDVM_NAK) && (out[i].b[3] == cmd)) {
			return out[i].b[4];
		}
	}
	return -1;
}

static void configure(bool simplex, uint8_t cc)
{
	uint8_t p[23] = { simplex ? 0x80 : 0x00, 0x02, 10, MMDVM_MODE_DMR, 128, 128, cc, 0, 128, 0, 128 };

	host(MMDVM_SET_CONFIG, p, sizeof(p));
}

static void set_freq(uint32_t rx, uint32_t tx)
{
	uint8_t p[17] = { 0 };

	for (int i = 0; i < 4; i++) {
		p[1 + i] = (uint8_t)(rx >> (8 * i));
		p[5 + i] = (uint8_t)(tx >> (8 * i));
	}
	p[9] = 255;
	host(MMDVM_SET_FREQ, p, sizeof(p));
}

static const uint8_t *status(void)
{
	host(MMDVM_GET_STATUS, NULL, 0);
	return last_out(MMDVM_GET_STATUS);
}

static const uint8_t LC1[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x2F, 0x5A, 0x01 };	// TG 9 from 3103233
static const uint8_t LC2[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x5B, 0x2F, 0x5A, 0x02 };	// TG 91 from 3103234

static void reset_world(void)
{
	sim_reset();
	out_count = 0;
	now = 1000;
	hotspot_init(&sim_modem, mmdvm_send);
	hotspot_set_radio(&radio_ops);
	mmdvm_init(&io);
	hotspot_sfr_init();
#if CONFIG_HOTSPOT_SPLIT
	hotspot_split_init();	// hotspotSfrTestSplit: the same suite with the split duplex extension registered
#endif
}

// SFR on: duplex, RX = TX, colour code 1
static void sfr_on(void)
{
	reset_world();
	host(MMDVM_GET_VERSION, NULL, 0);	// takes the modem
	configure(false, 1);
	set_freq(438500000, 438500000);
	slots(2);
}

// ---- Bursts heard ----

static void rx_lc(uint8_t kind, const uint8_t *lc, int8_t ts, uint8_t sync)
{
	dmr_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.sync = sync;
	b.cc = 1;
	b.ts = ts;
	b.crc_ok = true;
	b.in_step = true;
	b.lc_valid = true;
	b.len = 12;
	b.rssi_dbm = -80;
	dmr_lc_encode_full(lc, (kind == DMR_BURST_VOICE_LC_HEADER) ? DMR_RS_MASK_VOICE_HEADER : DMR_RS_MASK_TERMINATOR,
			   b.payload);
	memcpy(b.lc, lc, 9);
	sim_rx(&b);
}

static void rx_voice(uint8_t index, uint8_t seed, const uint8_t *lc_at_f, int8_t ts)
{
	dmr_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE;
	b.voice_index = index;
	b.sync = (index == 0) ? DMR_SYNC_TS1 : DMR_SYNC_NONE;
	b.cc = 1;
	b.ts = ts;
	b.crc_ok = true;
	b.in_step = true;
	b.len = DMR_VOICE_BYTES;
	b.rssi_dbm = -80;
	for (int i = 0; i < DMR_VOICE_BYTES; i++) {
		b.payload[i] = (uint8_t)(seed + i);
	}
	if ((index == 5) && (lc_at_f != NULL)) {
		b.lc_valid = true;
		memcpy(b.lc, lc_at_f, 9);
	}
	sim_rx(&b);
}

// The burst heard, then the 60 ms until the next one on the inbound slot
static void rx_then_wait(void)
{
	slots(2);
}

static int air_find(uint8_t kind, uint8_t sync, unsigned from)
{
	for (unsigned i = from; i < sim.air_count; i++) {
		if ((sim.air[i].kind == kind) && (sim.air[i].sync == sync)) {
			return (int)i;
		}
	}
	return -1;
}

static int air_count_kind(uint8_t kind, unsigned from)
{
	int n = 0;

	for (unsigned i = from; i < sim.air_count; i++) {
		n += sim.air[i].kind == kind;
	}
	return n;
}

// ---- Frames from the host ----

static void lc_frame(uint8_t dt, const uint8_t *lc, uint8_t *frame)
{
	dmrBurst_t b;

	memset(frame, 0, 33);
	b.dataType = dt;
	b.length = 12;
	b.flags = 0;
	dmr_lc_encode_full(lc, (dt == DT_VOICE_LC_HEADER) ? DMR_RS_MASK_VOICE_HEADER : DMR_RS_MASK_TERMINATOR, b.payload);
	dmrDataBurstToFrame(&b, 1, frame);
}

static void net_lc(uint8_t cmd, uint8_t dt, const uint8_t *lc)
{
	uint8_t p[34];

	p[0] = (uint8_t)(MMDVM_DMR_SYNC_DATA | dt);
	lc_frame(dt, lc, p + 1);
	host(cmd, p, sizeof(p));
}

static void net_voice(uint8_t cmd, uint8_t index, uint8_t seed)
{
	uint8_t p[34];

	memset(p, 0, sizeof(p));
	p[0] = (index == 0) ? MMDVM_DMR_SYNC_AUDIO : index;
	for (int i = 1; i < 34; i++) {
		p[i] = (uint8_t)(seed + i);
	}
	host(cmd, p, sizeof(p));
}

// ---- Tests ----

static void test_trigger(void)
{
	char d[128];
	const uint8_t *v, *st;

	reset_world();
	host(MMDVM_GET_VERSION, NULL, 0);
	v = last_out(MMDVM_GET_VERSION);
	CHECK(v != NULL);
	memset(d, 0, sizeof(d));
	memcpy(d, v + 4, (size_t)(v[1] - 4));
	CHECK(strstr(d, "OpenDigitalRadio Test 0123abcd SFR TS1>TS2 TS") != NULL);	// announced, on or not (6.1)

	// Simplex: no SFR, no extra status bytes, DATA1 refused
	configure(true, 1);
	set_freq(438500000, 438500000);
	slots(2);
	CHECK(!hotspot_alt_active());
	st = status();
	CHECK(st && st[1] == 13 && (st[5] & MMDVM_STATUS_INBOUND_BUSY) == 0);
	int o = out_count;
	net_lc(MMDVM_DMR_DATA1, DT_VOICE_LC_HEADER, LC1);
	CHECK(nak_reason(MMDVM_DMR_DATA1, o) == MMDVM_NAK_WRONG_MODE);

	// Duplex with RX != TX: not SFR either
	configure(false, 1);
	set_freq(438500000, 430500000);
	CHECK(!hotspot_alt_active());

	// Duplex and RX = TX, either order (gotchas 8.12)
	set_freq(438500000, 438500000);
	CHECK(hotspot_alt_active());
	st = status();
	CHECK(st && st[1] == 13 + HOTSPOT_SFR_STATUS_EXTRA);
	CHECK(st && st[7] == SFR_QUEUE_SIZE && st[8] == SFR_QUEUE_SIZE);	// both carry the outbound slot's space
	CHECK(st && st[13 + 47] == 0xFF && st[13 + 23] == 0xFF);		// raw area -1, reg12 the driver's

	// Back to simplex: off
	configure(true, 1);
	CHECK(!hotspot_alt_active());
	st = status();
	CHECK(st && st[1] == 13);
	configure(false, 1);
	CHECK(hotspot_alt_active());

	// The session's end turns it off; a new session starts in simplex
	mmdvm_end();
	CHECK(!hotspot_alt_active());
	host(MMDVM_GET_VERSION, NULL, 0);
	slots(2);
	CHECK(hotspot_running() && !hotspot_alt_active());
}

static void test_rf_call(void)
{
	const uint8_t *st;
	int o;

	sfr_on();
	CHECK(hotspot_alt_active());
	o = out_count;
	rx_lc(DMR_BURST_VOICE_LC_HEADER, LC1, 1, DMR_SYNC_TS1);
	slot();
	st = status();
	CHECK(st && (st[5] & MMDVM_STATUS_INBOUND_BUSY));	// an inbound call is being repeated
	CHECK(st && (st[5] & MMDVM_STATUS_TX));
	slot();
	for (int k = 0; k < 3; k++) {	// the rest of the header run: identical copies, not our echo
		rx_lc(DMR_BURST_VOICE_LC_HEADER, LC1, 1, DMR_SYNC_TS1);
		rx_then_wait();
	}
	uint32_t rx_e_slot = 0;
	for (int sf = 0; sf < 2; sf++) {
		for (uint8_t i = 0; i < 6; i++) {
			if (sf == 1 && i == 3) {
				slots(2);	// voice D lost on the inbound slot: a gap in the call
				continue;
			}
			if (sf == 1 && i == 4) {
				rx_e_slot = sim.slot;
			}
			rx_voice(i, (uint8_t)(0x10 * sf + i), LC1, 1);
			rx_then_wait();
		}
	}
	rx_lc(DMR_BURST_TERMINATOR_LC, LC1, 1, DMR_SYNC_TS1);
	rx_then_wait();
	unsigned in_call = sim.air_count;
	rx_lc(DMR_BURST_TERMINATOR_LC, LC1, 1, DMR_SYNC_TS1);	// the second terminator: not repeated again
	slots(6);

	// Up as DMR_DATA1 (inbound TS1): header, 11 voice, terminators, never on DATA2
	CHECK(count_out(MMDVM_DMR_DATA1, o) >= 13);
	CHECK(count_out(MMDVM_DMR_DATA2, o) == 0);
	for (int i = o; i < out_count; i++) {
		if (out[i].b[2] == MMDVM_DMR_DATA1) {
			uint8_t info[12], lc[9];

			CHECK(out[i].b[3] == (MMDVM_DMR_SYNC_DATA | DT_VOICE_LC_HEADER));
			dmrFecBptcDecode(out[i].b + 4, info);
			CHECK(dmr_lc_decode_full(info, DMR_RS_MASK_VOICE_HEADER, lc) >= 0 && memcmp(lc, LC1, 9) == 0);
			break;
		}
	}
	CHECK(count_out(MMDVM_DMR_LOST1, o) == 0);

	// Repeated on the outbound slot: aligned to the call, TS2 sync, the header from its LC, the voice in order
	CHECK(sim.first_timing == DMR_TIMING_ALIGNED && sim.first_ts == 2);
	CHECK(sim.tx_listens == sim.tx_begins);	// the inbound slot heard between our bursts (sfr.md 3 item 1)
	int h = air_find(DMR_BURST_VOICE_LC_HEADER, DMR_SYNC_TS2, 0);
	CHECK(h >= 0 && sim.air[h].host_crc && memcmp(sim.air[h].payload, LC1, 9) == 0);
	CHECK(air_count_kind(DMR_BURST_VOICE_LC_HEADER, 0) == 4);	// the whole header run
	// After the gap, E takes a placeholder's slot: on air at our first slot after it, not behind the placeholders
	for (unsigned i = 0; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_VOICE && sim.air[i].payload[0] == 0x14) {
			CHECK(sim.air[i].slot - rx_e_slot <= 2);
		}
	}
	CHECK(sim.fills >= 1);
	CHECK(air_count_kind(DMR_BURST_VOICE, 0) == 11);
	int n = 0;
	for (unsigned i = 0; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_VOICE) {
			n += n == 9;	// the lost D of the second superframe
			CHECK(sim.air[i].voice_index == n % 6 && sim.air[i].payload[0] == (uint8_t)(0x10 * (n / 6) + n % 6));
			CHECK(sim.air[i].sync == DMR_SYNC_TS2);
			n++;
		}
	}
	// The gap and the slots before the repeat started stay empty (received), never an idle burst (gotchas 14.4)
	CHECK(air_count_kind(DMR_BURST_IDLE, 0) == 0 || air_find(DMR_BURST_IDLE, DMR_SYNC_TS2, 0) >= (int)in_call);
	CHECK(sim.empty_slots >= 1);
	CHECK(sim.emb_lc_set && memcmp(sim.emb_lc, LC1, 9) == 0);
	CHECK(air_count_kind(DMR_BURST_TERMINATOR_LC, 0) == 1);
	st = status();
	CHECK(st && (st[5] & MMDVM_STATUS_INBOUND_BUSY) == 0);	// busy ends with the terminator repeated, not the hang
	CHECK(st && (st[13 + 11] & 0x01));			// still keyed: the hang

	// The hang (1 s) with idle bursts, then unkeyed
	slots(40);
	CHECK(air_count_kind(DMR_BURST_IDLE, 0) > 10);
	CHECK(!sim.tx_active && !hotspot_transmitting());
	hotspot_sfr_view_t v;
	hotspot_sfr_get_view(&v);
	CHECK(v.on && !v.keyed && v.stats.rx_repeated == 16 && v.stats.up_lost == 0 && v.stats.tx_gaps >= 1);
}

static void test_net_stream(void)
{
	int o;

	sfr_on();
	o = out_count;
	net_lc(MMDVM_DMR_DATA2, DT_VOICE_LC_HEADER, LC2);
	for (uint8_t i = 0; i < 6; i++) {
		net_voice(MMDVM_DMR_DATA2, i, (uint8_t)(0x40 + i));
		slot();
	}
	net_lc(MMDVM_DMR_DATA2, DT_TERMINATOR_WITH_LC, LC2);
	net_lc(MMDVM_DMR_DATA2, DT_TERMINATOR_WITH_LC, LC2);	// a repeat: ACKed and dropped
	CHECK(count_out(MMDVM_ACK, o) == 9 && nak_reason(MMDVM_DMR_DATA2, o) < 0);
	slots(30);
	CHECK(sim.first_timing == DMR_TIMING_OWN && sim.first_ts == 2);	// nothing heard: our own slot clock
	int h = air_find(DMR_BURST_VOICE_LC_HEADER, DMR_SYNC_TS2, 0);
	CHECK(h >= 0 && memcmp(sim.air[h].payload, LC2, 9) == 0);
	CHECK(air_count_kind(DMR_BURST_VOICE, 0) == 6 && air_count_kind(DMR_BURST_TERMINATOR_LC, 0) == 1);
	for (unsigned i = 0; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_VOICE && sim.air[i].voice_index == 2) {
			uint8_t frame[33], voice[27];

			for (int k = 0; k < 33; k++) {
				frame[k] = (uint8_t)(0x42 + k + 1);
			}
			hotspot_frame_voice(frame, voice);
			CHECK(memcmp(sim.air[i].payload, voice, 27) == 0);
		}
	}
	CHECK(count_out(MMDVM_DMR_DATA1, o) == 0);	// our own traffic never goes up

	// DATA1 is taken too (both offsets carry the outbound space)
	o = out_count;
	net_lc(MMDVM_DMR_DATA1, DT_VOICE_LC_HEADER, LC2);
	CHECK(reply(MMDVM_ACK, MMDVM_DMR_DATA1, o));
	o = out_count;
	host(MMDVM_DMR_ABORT, NULL, 0);
	CHECK(reply(MMDVM_ACK, MMDVM_DMR_ABORT, o));
	CHECK(status()[8] == SFR_QUEUE_SIZE);
	slots(60);
}

static void test_rf_priority(void)
{
	int o;

	sfr_on();
	rx_lc(DMR_BURST_VOICE_LC_HEADER, LC1, 1, DMR_SYNC_TS1);
	slot();
	o = out_count;
	net_lc(MMDVM_DMR_DATA2, DT_VOICE_LC_HEADER, LC2);
	CHECK(nak_reason(MMDVM_DMR_DATA2, o) == MMDVM_NAK_RF_BUSY);
	slot();
	for (uint8_t i = 0; i < 6; i++) {
		rx_voice(i, i, LC1, 1);
		rx_then_wait();
	}
	rx_lc(DMR_BURST_TERMINATOR_LC, LC1, 1, DMR_SYNC_TS1);
	slots(6);
	// The terminator repeated: a new stream from the host is taken (it goes out in the hang)
	o = out_count;
	net_lc(MMDVM_DMR_DATA2, DT_VOICE_LC_HEADER, LC2);
	CHECK(reply(MMDVM_ACK, MMDVM_DMR_DATA2, o));
	net_lc(MMDVM_DMR_DATA2, DT_TERMINATOR_WITH_LC, LC2);
	slots(6);
	int h = -1;
	for (unsigned i = 0; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_VOICE_LC_HEADER && memcmp(sim.air[i].payload, LC2, 9) == 0) {
			h = (int)i;
		}
	}
	CHECK(h >= 0);
	CHECK(sim.tx_begins == 1);	// one transmission: the stream went out in the repeat's hang
	slots(60);
}

static void test_echo_and_late_entry(void)
{
	hotspot_sfr_view_t v;
	int o;

	sfr_on();
	// A lone voice A outside a call: noise, not repeated, nothing up
	o = out_count;
	rx_voice(0, 0x70, NULL, 1);
	slots(6);
	hotspot_sfr_get_view(&v);
	CHECK(v.stats.rx_late_held == 1 && v.stats.rx_repeated == 0 && sim.tx_begins == 0);
	CHECK(count_out(MMDVM_DMR_DATA1, o) == 0);

	// A, then B 60 ms later: the call starts with B; once F gives the LC, a header made from it goes up first
	rx_voice(0, 0x80, NULL, 1);
	rx_then_wait();
	for (uint8_t i = 1; i < 6; i++) {
		rx_voice(i, (uint8_t)(0x80 + i), LC1, 1);
		rx_then_wait();
	}
	rx_voice(0, 0x90, NULL, 1);
	rx_then_wait();
	hotspot_sfr_get_view(&v);
	CHECK(v.stats.rx_repeated >= 5 && sim.tx_begins == 1);
	int first = -1;
	for (int i = o; i < out_count; i++) {
		if (out[i].b[2] == MMDVM_DMR_DATA1) {
			first = i;
			break;
		}
	}
	CHECK(first >= 0 && out[first].b[3] == (MMDVM_DMR_SYNC_DATA | DT_VOICE_LC_HEADER));

	// Our own burst reported back as heard (the chip does, in an empty inbound slot): dropped
	unsigned last_voice = 0;
	for (unsigned i = 0; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_VOICE) {
			last_voice = i;
		}
	}
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE;
	b.voice_index = sim.air[last_voice].voice_index;
	b.cc = 1;
	b.len = DMR_VOICE_BYTES;
	memcpy(b.payload, sim.air[last_voice].payload, DMR_VOICE_BYTES);
	uint32_t echo0 = v.stats.rx_echo;
	sim_rx(&b);
	slot();
	hotspot_sfr_get_view(&v);
	CHECK(v.stats.rx_echo == echo0 + 1);

	// A burst on the outbound slot (ts 2) is never repeated
	uint32_t slot0 = v.stats.rx_ignored_slot;
	rx_voice(1, 0x33, NULL, 2);
	slot();
	hotspot_sfr_get_view(&v);
	CHECK(v.stats.rx_ignored_slot == slot0 + 1);

	// Lost without its terminator: DMR_LOST1 after 300 ms
	o = out_count;
	slots(14);
	CHECK(count_out(MMDVM_DMR_LOST1, o) == 1);
	slots(60);
}

static void test_vendor(void)
{
	const uint8_t *v;
	char d[128];
	int o;
	uint8_t p[7] = { 0x00, SFR_FRAMING_MS, 0x00, 0x20, 0x00, 0x00, 0x40 };

	sfr_on();
	o = out_count;
	host(MMDVM_ODR_SFR_DEBUG, p, 1);
	CHECK(nak_reason(MMDVM_ODR_SFR_DEBUG, o) == MMDVM_NAK_INVALID);
	o = out_count;
	host(MMDVM_ODR_SFR_DEBUG, p, 7);
	CHECK(reply(MMDVM_ACK, MMDVM_ODR_SFR_DEBUG, o));
	host(MMDVM_GET_VERSION, NULL, 0);
	v = last_out(MMDVM_GET_VERSION);
	memset(d, 0, sizeof(d));
	memcpy(d, v + 4, (size_t)(v[1] - 4));
	CHECK(strstr(d, " SFR TS1>TS2 MS") != NULL);
	const uint8_t *st = status();
	CHECK(st[13 + 23] == 0x20 && st[13 + 94] == 0x40);
	CHECK(hotspot_alt_active());

	// MS framing: the outbound bursts carry MS sync
	net_lc(MMDVM_DMR_DATA2, DT_VOICE_LC_HEADER, LC2);
	net_lc(MMDVM_DMR_DATA2, DT_TERMINATOR_WITH_LC, LC2);
	slots(8);
	CHECK(air_find(DMR_BURST_VOICE_LC_HEADER, DMR_SYNC_MS, 0) >= 0);
	slots(60);
}

// The idle beacon (dmr-roaming.md 7): off by default; on, idle bursts on the outbound slot for its duration every
// interval while nothing is repeated, and a call heard during one is repeated as usual
static void test_beacon(void)
{
	hotspot_sfr_view_t v;

	sfr_on();
	slots(400);	// 12 s
	hotspot_sfr_get_view(&v);
	CHECK(v.on && v.stats.tx_beacons == 0 && v.stats.tx_starts == 0);	// off: nothing keyed

	hotspot_sfr_set_beacon(10, 2);
	configure(true, 1);	// simplex and back: SFR mode starts again and takes the beacon
	configure(false, 1);
	CHECK(hotspot_alt_active());
	int keyed_slots = 0, first = -1, last = -1;
	for (int i = 0; i < 1100; i++) {	// 33 s
		slot();
		hotspot_sfr_get_view(&v);
		if (v.keyed) {
			keyed_slots++;
			if (first < 0) {
				first = i;
			}
			last = i;
		}
	}
	CHECK(v.stats.tx_beacons == 3 && v.stats.tx_starts == 3);
	CHECK(first >= 320 && first <= 340);			// the first one 10 s after the start
	CHECK(last >= 960 && last <= 1080);
	CHECK(keyed_slots >= 3 * 60 && keyed_slots <= 3 * 75);	// 2 s each (66 slots), plus the modem's drain
	CHECK(v.stats.tx_idle >= 3 * 30 && v.stats.tx_bursts == 0);	// idle bursts only (every other slot)
	hotspot_sfr_set_beacon(0, 2);
}

int main(void)
{
	test_trigger();
	test_rf_call();
	test_net_stream();
	test_rf_priority();
	test_echo_and_late_entry();
	test_vendor();
	test_beacon();
	printf("# " SUITE ": %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
