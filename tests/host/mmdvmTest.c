// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Host test of the MMDVM protocol (core/hotspot/mmdvm.c) and the simplex hotspot engine (core/hotspot/hotspot.c),
// against a fake DMR modem (hal_dmr_modem_t) and a fake radio core. Frames are checked with independent decoding:
// the RS(12,9) check evaluates the codeword at a, a^2, a^3 rather than dividing like the encoder; the uplink EMB and
// embedded LC are read back with the decoders (QR(16,7,6) in core/dmr/fec, the embedded LC reassembly of
// core/dmr/call/dmr_lc.c). Also the host link's rule for leaving hotspot mode (DTR, idle, exit).
#include <stdio.h>
#include <string.h>
#include "hotspot/mmdvm.h"
#include "hotspot/hotspot.h"
#include "dmr/dmrData.h"
#include "dmr/fec/dmrFec.h"
#include "hotspot/dmrDataFrame.h"
#include "hotspot/hotspotUplink.h"
#include "dmr/call/dmr_lc.h"
#include "hostlink/hostlink.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// ---- Frames from the radio ----

#define MAX_OUT 256
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

// ---- Fake modem ----

#define LOG_MAX 128
static struct {
	bool started, start_ok, begin_ok;
	int starts, stops, begins, ends, aborts, restarts, configures;
	int8_t cc, cc_filter;
	uint8_t pending;	// queued and not yet "on air"
	dmr_tx_burst_t log[LOG_MAX];
	int log_count;
	char ops[LOG_MAX];	// B begin, Q queue, E end, A abort
	int op_count;
	uint8_t elc[9];
} m;

static void op(char c)
{
	if (m.op_count < LOG_MAX - 1) {
		m.ops[m.op_count++] = c;
		m.ops[m.op_count] = '\0';
	}
}

static bool f_start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	m.starts++;
	m.cc = cfg->cc;
	m.started = m.start_ok;
	return m.start_ok;
}
static void f_stop(void) { m.stops++; m.started = false; }
static void f_configure(const dmr_config_t *cfg) { m.configures++; m.cc = cfg->cc; }
static void f_rx_restart(void) { m.restarts++; }
static void f_rx_lock_slot(int8_t ts) { (void)ts; }
static bool f_tx_begin(dmr_timing_t t, int8_t ts, dmr_access_t a) { m.begins++; op('B'); return m.begin_ok; }
static bool f_tx_queue(const dmr_tx_burst_t *b)
{
	if (m.pending >= 8) {
		return false;
	}
	m.pending++;
	if (m.log_count < LOG_MAX) {
		m.log[m.log_count++] = *b;
	}
	op('Q');
	return true;
}
static uint8_t f_tx_queue_space(void) { return (uint8_t)(8 - m.pending); }
static void f_tx_end(void) { m.ends++; op('E'); }
static void f_tx_abort(void) { m.aborts++; m.pending = 0; op('A'); }
static void f_set_embedded_lc(const uint8_t lc[9]) { memcpy(m.elc, lc, 9); }
static void f_set_cc_filter(int8_t cc) { m.cc_filter = cc; }
static bool f_is_transmitting(void) { return false; }

static const hal_dmr_modem_t fake_modem = {
	.start = f_start, .stop = f_stop, .configure = f_configure, .rx_restart = f_rx_restart,
	.rx_lock_slot = f_rx_lock_slot, .tx_begin = f_tx_begin, .tx_queue = f_tx_queue,
	.tx_queue_space = f_tx_queue_space, .tx_end = f_tx_end, .tx_abort = f_tx_abort,
	.set_embedded_lc = f_set_embedded_lc, .set_cc_filter = f_set_cc_filter, .is_transmitting = f_is_transmitting,
};

static void air(void)
{
	m.pending = 0;	// everything queued has gone out
}

static void tx_done(void)
{
	dmr_event_t ev = { .kind = DMR_EV_TX_DONE };

	hotspot_modem_event(&ev, NULL);
}

// ---- Fake radio core ----

static struct {
	bool enter_ok;
	int enters, leaves, freqs;
	uint32_t rx, tx;
	uint8_t level;
	int out_at_enter;	// frames written before the last enter (the reply goes first)
	int16_t rssi;
	int cw_calls;
	char cw[64];
	bool cw_ok, cw_keyed;
} r;

static bool r_enter(uint32_t rx);
static bool r_set_freq(uint32_t rx, uint32_t tx) { r.freqs++; r.rx = rx; r.tx = tx; return rx >= 100000000; }
static void r_set_power(uint8_t level) { r.level = level; }
static void r_leave(void) { r.leaves++; }
static int16_t r_rssi(void) { return r.rssi; }
static bool r_cw_id(const char *c, uint8_t len)
{
	r.cw_calls++;
	memcpy(r.cw, c, len);
	r.cw[len] = '\0';
	r.cw_keyed = r.cw_ok;
	return r.cw_ok;
}
static bool r_cw_busy(void) { return r.cw_keyed; }

static const hotspot_radio_ops_t radio_ops = {
	.enter = r_enter, .set_freq = r_set_freq, .set_power = r_set_power, .leave = r_leave,
};
// The same with the optional RSSI and CW ID ops
static const hotspot_radio_ops_t radio_ops_full = {
	.enter = r_enter, .set_freq = r_set_freq, .set_power = r_set_power, .leave = r_leave,
	.rssi_dbm = r_rssi, .cw_id = r_cw_id, .cw_busy = r_cw_busy,
};

// ---- Helpers ----

static uint32_t now = 1000;

static bool r_enter(uint32_t rx) { r.enters++; r.out_at_enter = out_count; return r.enter_ok; }

static void host(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
	uint8_t f[MMDVM_FRAME_MAX];

	f[0] = 0xE0;
	f[1] = (uint8_t)(len + 3);
	f[2] = cmd;
	memcpy(f + 3, payload, len);
	mmdvm_frame(f, (uint16_t)(len + 3), now);
}

static void poll_for(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		mmdvm_poll(now);
	}
}

// The last frame with this command, or NULL
static const uint8_t *last_out(uint8_t cmd)
{
	for (int i = out_count - 1; i >= 0; i--) {
		if (out[i].b[2] == cmd) {
			return out[i].b;
		}
	}
	return NULL;
}

static int count_out(uint8_t cmd)
{
	int n = 0;

	for (int i = 0; i < out_count; i++) {
		n += out[i].b[2] == cmd;
	}
	return n;
}

static bool nak_for(uint8_t cmd, uint8_t reason)
{
	for (int i = 0; i < out_count; i++) {
		if ((out[i].b[2] == MMDVM_NAK) && (out[i].b[3] == cmd) && (out[i].b[4] == reason)) {
			return true;
		}
	}
	return false;
}

static bool ack_for(uint8_t cmd)
{
	for (int i = 0; i < out_count; i++) {
		if ((out[i].b[2] == MMDVM_ACK) && (out[i].b[3] == cmd)) {
			return true;
		}
	}
	return false;
}

// GF(256) with 0x11D
static uint8_t gmul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;

	for (int i = 0; i < 8; i++) {
		if (b & 1) {
			p ^= a;
		}
		uint8_t hi = a & 0x80;
		a <<= 1;
		if (hi) {
			a ^= 0x1D;
		}
		b >>= 1;
	}
	return p;
}

// Codeword c(x) = lc[0] x^11 + ... + p[2]: zero at a, a^2, a^3 when valid
static bool rs_valid(const uint8_t *info, uint8_t mask)
{
	uint8_t c[12];

	memcpy(c, info, 12);
	c[9] ^= mask;
	c[10] ^= mask;
	c[11] ^= mask;
	for (uint8_t root = 2, k = 0; k < 3; k++, root = gmul(root, 2)) {
		uint8_t v = 0;

		for (int i = 0; i < 12; i++) {
			v = (uint8_t)(gmul(v, root) ^ c[i]);
		}
		if (v != 0) {
			return false;
		}
	}
	return true;
}

// Test-side RS(12,9) parity for the frames the test sends (checked by rs_valid below)
static void rs_make(const uint8_t *lc, uint8_t mask, uint8_t *info)
{
	uint8_t rem[3] = { 0, 0, 0 };
	static const uint8_t G[3] = { 14, 56, 64 };

	memcpy(info, lc, 9);
	for (int i = 0; i < 9; i++) {
		uint8_t fb = (uint8_t)(lc[i] ^ rem[0]);

		rem[0] = (uint8_t)(rem[1] ^ gmul(fb, G[0]));
		rem[1] = (uint8_t)(rem[2] ^ gmul(fb, G[1]));
		rem[2] = gmul(fb, G[2]);
	}
	info[9] = (uint8_t)(rem[0] ^ mask);
	info[10] = (uint8_t)(rem[1] ^ mask);
	info[11] = (uint8_t)(rem[2] ^ mask);
}

static const uint8_t LC1[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x5B, 0x31, 0x84, 0x2A };	// group 91 from 3245098
static const uint8_t LC2[9] = { 0x00, 0x00, 0x00, 0x00, 0x0B, 0xBF, 0x31, 0x84, 0x2B };	// group 3007 from 3245099

static void data_frame(uint8_t dt, const uint8_t *info12, uint8_t cc, uint8_t *frame)
{
	dmrBurst_t b = { .dataType = dt, .length = 12, .flags = 0 };

	memset(frame, 0, 33);
	memcpy(b.payload, info12, 12);
	dmrDataBurstToFrame(&b, cc, frame);
}

static void lc_frame(uint8_t dt, const uint8_t *lc, uint8_t *frame)
{
	uint8_t info[12];

	rs_make(lc, (dt == DT_VOICE_LC_HEADER) ? 0x96 : 0x99, info);
	data_frame(dt, info, 2, frame);
}

static const uint8_t MS_VOICE_SYNC[6] = { 0x7F, 0x7D, 0x5D, 0xD5, 0x7D, 0xFD };

static void put_middle(uint8_t *frame, const uint8_t *mid6)
{
	frame[13] = (uint8_t)((frame[13] & 0xF0) | (mid6[0] >> 4));
	for (int i = 0; i < 5; i++) {
		frame[14 + i] = (uint8_t)((mid6[i] << 4) | (mid6[i + 1] >> 4));
	}
	frame[19] = (uint8_t)((mid6[5] << 4) | (frame[19] & 0x0F));
}

// A voice frame as MMDVMHost sends it: control 0, voice sync on A, an EMB-ish middle on B-F
static void voice_frame(uint8_t index, uint8_t seed, uint8_t *frame)
{
	static const uint8_t EMB[6] = { 0x13, 0x10, 0x00, 0x00, 0x00, 0x05 };

	for (int i = 0; i < 33; i++) {
		frame[i] = (uint8_t)(seed + i * 7);
	}
	put_middle(frame, (index == 0) ? MS_VOICE_SYNC : EMB);
}

static uint8_t net(const uint8_t *frame33)
{
	uint8_t p[34];
	int before = out_count;

	p[0] = 0;	// MMDVMHost's control byte
	memcpy(p + 1, frame33, 33);
	host(MMDVM_DMR_DATA2, p, 34);
	for (int i = before; i < out_count; i++) {
		if ((out[i].b[2] == MMDVM_NAK) && (out[i].b[3] == MMDVM_DMR_DATA2)) {
			return out[i].b[4];
		}
	}
	return 0;
}

static void net_lc(uint8_t dt, const uint8_t *lc)
{
	uint8_t f[33];

	lc_frame(dt, lc, f);
	net(f);
}

static void net_voice(uint8_t index, uint8_t seed)
{
	uint8_t f[33];

	voice_frame(index, seed, f);
	net(f);
}

static void reset_world(void)
{
	memset(&m, 0, sizeof(m));
	memset(&r, 0, sizeof(r));
	m.start_ok = true;
	m.begin_ok = true;
	r.enter_ok = true;
	out_count = 0;
	hotspot_init(&fake_modem, mmdvm_send);
	hotspot_set_radio(&radio_ops);
	mmdvm_init(&io);
	mmdvm_set_ext(NULL);
}

static void configure(uint8_t cc)
{
	// RadioDesk rfgw's config_v1: 23 payload bytes, simplex, DMR enabled, state DMR
	uint8_t p[23] = { 0x80, 0x02, 10, MMDVM_MODE_DMR, 128, 128, cc, 0, 128, 0, 128, 0, 0, 128, 128 };
	host(MMDVM_SET_CONFIG, p, sizeof(p));
}

static void set_freq(uint32_t rx, uint32_t tx, uint8_t level)
{
	uint8_t p[17] = { 0 };

	for (int i = 0; i < 4; i++) {
		p[1 + i] = (uint8_t)(rx >> (8 * i));
		p[5 + i] = (uint8_t)(tx >> (8 * i));
		p[13 + i] = (uint8_t)(tx >> (8 * i));
	}
	p[9] = level;
	host(MMDVM_SET_FREQ, p, sizeof(p));
}

static const uint8_t *status(void)
{
	host(MMDVM_GET_STATUS, NULL, 0);
	return last_out(MMDVM_GET_STATUS);
}

// ---- Tests ----

static void test_framer(void)
{
	mmdvm_framer_t f;
	uint8_t stream[600];
	size_t n = 0;
	int frames = 0;
	uint8_t cmds[8];

	mmdvm_framer_reset(&f);
	f.junk = 0;
	f.skipped = 0;
	stream[n++] = 0x41;			// junk (a stray CPS byte)
	stream[n++] = 0x00;
	stream[n++] = 0xE0; stream[n++] = 0x03; stream[n++] = 0x00;		// GET_VERSION
	stream[n++] = 0xE0; stream[n++] = 0x01;				// bad length: resync
	stream[n++] = 0xE0; stream[n++] = 0x04; stream[n++] = 0x03; stream[n++] = 0x02;	// SET_MODE DMR
	stream[n++] = 0xE0; stream[n++] = 0x00; stream[n++] = 0x02;		// long frame of 257 bytes: skipped
	for (int i = 0; i < 257 - 3; i++) {
		stream[n++] = 0xE0;		// even if full of start bytes
	}
	stream[n++] = 0xE0; stream[n++] = 0x03; stream[n++] = 0x01;		// GET_STATUS
	for (size_t i = 0; i < n; i++) {
		uint16_t len = mmdvm_framer_byte(&f, stream[i]);

		if (len > 0) {
			CHECK(f.buf[0] == 0xE0 && f.buf[1] == len);
			if (frames < 8) {
				cmds[frames] = f.buf[2];
			}
			frames++;
		}
	}
	CHECK(frames == 3);
	CHECK(cmds[0] == MMDVM_GET_VERSION && cmds[1] == MMDVM_SET_MODE && cmds[2] == MMDVM_GET_STATUS);
	CHECK(f.junk == 3);	// the two junk bytes and the bad length byte
	CHECK(f.skipped == 2);
}

static void test_version_status(void)
{
	reset_world();
	r.enter_ok = false;	// the radio can't hand over the modem yet
	host(MMDVM_GET_VERSION, NULL, 0);
	const uint8_t *v = last_out(MMDVM_GET_VERSION);
	CHECK(v != NULL);
	if (v != NULL) {
		CHECK(v[3] == 1);
		CHECK(v[1] == 4 + strlen(io.description));
		CHECK(memcmp(v + 4, "OpenDigitalRadio Test 0123abcd", strlen(io.description)) == 0);
	}
	const uint8_t *s = status();
	CHECK(s != NULL && s[1] == 13);
	if (s != NULL) {
		CHECK(s[3] == 0x02);	// DMR
		CHECK(s[4] == MMDVM_MODE_IDLE);
		CHECK(s[5] == 0);
		CHECK(s[8] == 0);	// no space: the hotspot isn't running
	}
	CHECK(!hotspot_running());
	CHECK(r.enters == 1);		// tried once, after the version reply had gone out
	CHECK(r.out_at_enter == 1);

	// Now the radio lets go of the modem; the next try comes HOTSPOT_ENTER_RETRY_MS after the refused one
	r.enter_ok = true;
	now += 500;
	status();
	CHECK(!hotspot_running() && r.enters == 1);
	now += HOTSPOT_ENTER_RETRY_MS - 500;
	int before = out_count;
	s = status();
	CHECK(hotspot_running() && m.starts == 1 && r.enters == 2);
	CHECK(r.out_at_enter == before + 1);	// that status reply went out first (showing no space yet)
	s = status();
	CHECK(s != NULL && s[8] == 22 && s[7] == 0);	// 24 places less the margin of 2 (spec 2.4)
	CHECK(s != NULL && s[12] == 0);			// POCSAG space
}

// A-15: a radio that keeps refusing (or posts the handover and returns): enter is tried at most once a second while
// MMDVMHost polls every 250 ms, and every poll is answered
static void test_slow_enter(void)
{
	reset_world();
	r.enter_ok = false;
	host(MMDVM_GET_VERSION, NULL, 0);
	for (int i = 0; i < 20; i++) {	// 5 s
		int before = out_count;

		poll_for(250);
		host(MMDVM_GET_STATUS, NULL, 0);
		CHECK(out_count == before + 1 && out[before].b[2] == MMDVM_GET_STATUS);
	}
	CHECK(r.enters >= 5 && r.enters <= 6);
	// The handover completes: running within a second, without another frame (mmdvm_poll retries)
	r.enter_ok = true;
	poll_for(1000);
	CHECK(hotspot_running());
	// After the host left, a new session tries at once
	mmdvm_end();
	r.enter_ok = false;
	int e = r.enters;
	host(MMDVM_GET_VERSION, NULL, 0);
	CHECK(r.enters == e + 1);
}

static void test_config(void)
{
	reset_world();
	configure(2);
	CHECK(ack_for(MMDVM_SET_CONFIG));
	CHECK(m.cc == 2 && m.cc_filter == 2);
	set_freq(438800000, 438800000, 128);
	CHECK(ack_for(MMDVM_SET_FREQ));
	CHECK(r.rx == 438800000 && r.tx == 438800000 && r.level == 128);
	set_freq(50000, 50000, 128);	// out of range for the fake radio
	CHECK(nak_for(MMDVM_SET_FREQ, MMDVM_NAK_INVALID));

	uint8_t mode = MMDVM_MODE_DMR;
	host(MMDVM_SET_MODE, &mode, 1);
	CHECK(ack_for(MMDVM_SET_MODE));
	mode = MMDVM_MODE_YSF;
	host(MMDVM_SET_MODE, &mode, 1);
	CHECK(nak_for(MMDVM_SET_MODE, MMDVM_NAK_INVALID));
	CHECK(status()[4] == MMDVM_MODE_DMR);

	uint8_t junk[40] = { 0 };
	host(MMDVM_DSTAR_HEADER, junk, 41 - 3);
	CHECK(nak_for(MMDVM_DSTAR_HEADER, MMDVM_NAK_WRONG_MODE));
	host(MMDVM_YSF_DATA, junk, 10);
	CHECK(nak_for(MMDVM_YSF_DATA, MMDVM_NAK_WRONG_MODE));
	host(MMDVM_DMR_DATA1, junk, 34);
	CHECK(nak_for(MMDVM_DMR_DATA1, MMDVM_NAK_WRONG_MODE));
	host(MMDVM_DMR_DATA2, junk, 20);
	CHECK(nak_for(MMDVM_DMR_DATA2, MMDVM_NAK_INVALID));
	// D-6: exactly 37 bytes
	out_count = 0;
	host(MMDVM_DMR_DATA2, junk, 33);
	host(MMDVM_DMR_DATA2, junk, 35);
	CHECK(out_count == 2 && nak_for(MMDVM_DMR_DATA2, MMDVM_NAK_INVALID));
	// D-8, D-13 (A-5, I-2, A-13): taken silently
	out_count = 0;
	host(MMDVM_DMR_START, junk, 1);
	host(MMDVM_DMR_SHORTLC, junk, 9);
	host(MMDVM_SERIAL, junk, 3);
	host(MMDVM_TRANSPARENT, junk, 2);
	CHECK(out_count == 0);
	// C-15: FM parameters ACKed and ignored
	host(MMDVM_FM_PARAMS1, junk, 7);
	host(MMDVM_FM_PARAMS4, junk, 3);
	CHECK(out_count == 2 && ack_for(MMDVM_FM_PARAMS1) && ack_for(MMDVM_FM_PARAMS4));
	// C-4: colour code above 15
	out_count = 0;
	configure(16);
	CHECK(nak_for(MMDVM_SET_CONFIG, MMDVM_NAK_INVALID) && m.cc == 2);
	// C-6: state 01
	uint8_t c6[23] = { 0x80, 0x02, 10, MMDVM_MODE_DSTAR, 128, 128, 1 };
	out_count = 0;
	host(MMDVM_SET_CONFIG, c6, sizeof(c6));
	CHECK(nak_for(MMDVM_SET_CONFIG, MMDVM_NAK_INVALID));
	host(0xC3, NULL, 0);
	CHECK(nak_for(0xC3, MMDVM_NAK_UNKNOWN));
	host(MMDVM_ODR_SFR_DEBUG, junk, 2);
	CHECK(nak_for(MMDVM_ODR_SFR_DEBUG, MMDVM_NAK_UNKNOWN));
	int before = out_count;
	host(MMDVM_QSO_INFO, junk, 10);
	CHECK(out_count == before);	// never answered
}

static uint8_t ext_vendor(uint8_t command, const uint8_t *p, uint8_t len) { return (len >= 2) ? 0 : 4; }
static uint8_t ext_extra(uint8_t *buf, uint8_t max) { buf[0] = 0xAA; buf[1] = 0x55; return 2; }
static uint8_t ext_suffix(char *buf, uint8_t max) { memcpy(buf, " SFR TS1>TS2 TS", 15); return 15; }
static uint8_t ext_flags(void) { return MMDVM_STATUS_INBOUND_BUSY; }
static bool ext_simplex;
static uint32_t ext_rx, ext_tx;
static void ext_config(bool simplex, uint32_t rx, uint32_t tx) { ext_simplex = simplex; ext_rx = rx; ext_tx = tx; }

static void test_extension(void)
{
	static const mmdvm_ext_t ext = {
		.vendor = ext_vendor, .status_extra = ext_extra, .version_suffix = ext_suffix,
		.config_changed = ext_config, .status_flags = ext_flags,
	};

	reset_world();
	mmdvm_set_ext(&ext);
	uint8_t p[2] = { 1, 2 };
	host(MMDVM_ODR_SFR_DEBUG, p, 2);
	CHECK(ack_for(MMDVM_ODR_SFR_DEBUG));
	host(MMDVM_ODR_SFR_DEBUG, p, 1);
	CHECK(nak_for(MMDVM_ODR_SFR_DEBUG, 4));
	const uint8_t *s = status();
	CHECK(s[1] == 15 && s[13] == 0xAA && s[14] == 0x55 && (s[5] & 0x80));
	host(MMDVM_GET_VERSION, NULL, 0);
	const uint8_t *v = last_out(MMDVM_GET_VERSION);
	CHECK(memcmp(v + 4 + strlen(io.description), " SFR TS1>TS2 TS", 15) == 0);
	// SET_CONFIG duplex and SET_FREQ RX = TX reach the extension, in either order
	uint8_t c[23] = { 0x00, 0x02, 10, 0, 128, 128, 1 };
	host(MMDVM_SET_CONFIG, c, sizeof(c));
	CHECK(!ext_simplex);
	set_freq(446000000, 446000000, 255);
	CHECK(ext_rx == 446000000 && ext_tx == 446000000 && !ext_simplex);
}

static bool no_dmr_nak(void)
{
	for (int i = 0; i < out_count; i++) {
		if ((out[i].b[2] == MMDVM_NAK) && (out[i].b[3] == MMDVM_DMR_DATA2)) {
			return false;
		}
	}
	return true;
}

// A network call as MMDVMHost sends it: idle frames, 2 headers, 2 superframes, 3 terminators, all control 0
static void test_net_call(void)
{
	reset_world();
	configure(2);
	set_freq(438800000, 438800000, 255);

	uint8_t idle[12] = { 0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91 };
	uint8_t f[33];
	data_frame(DT_IDLE, idle, 2, f);
	for (int i = 0; i < 6; i++) {
		CHECK(net(f) == 0);	// taken and dropped, no NAK
		poll_for(60);
	}
	CHECK(m.begins == 0);

	net_lc(DT_VOICE_LC_HEADER, LC1);
	poll_for(60);
	net_lc(DT_VOICE_LC_HEADER, LC1);
	poll_for(60);
	CHECK(m.begins == 0);	// buffering
	CHECK(status()[5] & MMDVM_STATUS_TX);	// transmitting from buffering on
	for (int i = 0; i < 12; i++) {
		net_voice((uint8_t)(i % 6), (uint8_t)i);
		poll_for(60);
		air();
	}
	CHECK(m.begins == 1);
	CHECK(memcmp(m.elc, LC1, 9) == 0);
	for (int i = 0; i < 3; i++) {
		net_lc(DT_TERMINATOR_WITH_LC, LC1);
		poll_for(60);
	}
	CHECK(m.ends == 1);
	CHECK(no_dmr_nak());
	// Modem saw: 2 headers (9-byte LC), 12 voice A-F A-F with their payload, 1 terminator
	CHECK(m.log_count == 15);
	CHECK(m.log[0].kind == DMR_BURST_VOICE_LC_HEADER && m.log[0].len == 9 && !m.log[0].host_crc);
	CHECK(memcmp(m.log[0].payload, LC1, 9) == 0 && m.log[1].kind == DMR_BURST_VOICE_LC_HEADER);
	for (int i = 0; i < 12; i++) {
		const dmr_tx_burst_t *b = &m.log[2 + i];
		uint8_t vf[33];

		voice_frame((uint8_t)(i % 6), (uint8_t)i, vf);
		CHECK(b->kind == DMR_BURST_VOICE && b->voice_index == i % 6 && b->len == 27);
		CHECK(memcmp(b->payload, vf, 13) == 0 && memcmp(b->payload + 14, vf + 20, 13) == 0);
		CHECK(b->payload[13] == ((vf[13] & 0xF0) | (vf[19] & 0x0F)));
	}
	CHECK(m.log[14].kind == DMR_BURST_TERMINATOR_LC && memcmp(m.log[14].payload, LC1, 9) == 0);
	CHECK(status()[5] & MMDVM_STATUS_TX);	// until the modem says the terminator is out
	tx_done();
	poll_for(10);
	CHECK(!(status()[5] & MMDVM_STATUS_TX));
	hotspot_stats_t st;
	hotspot_get_stats(&st);
	CHECK(st.net_calls == 1);
}

// A call shorter than the prebuffer starts on its terminator
static void test_short_call(void)
{
	reset_world();
	configure(1);
	net_lc(DT_VOICE_LC_HEADER, LC1);
	net_voice(0, 1);
	net_voice(1, 2);
	poll_for(20);
	CHECK(m.begins == 0);
	net_lc(DT_TERMINATOR_WITH_LC, LC1);
	poll_for(5);
	CHECK(m.begins == 1 && m.ends == 1 && m.log_count == 4);
}

// MMDVMHost sends the next call's headers right behind the last call (or a call loses its terminator):
// the old call ends with a terminator and the new one keys up again with its own header and LC
static void test_back_to_back(void)
{
	reset_world();
	configure(1);
	net_lc(DT_VOICE_LC_HEADER, LC1);
	for (int i = 0; i < 6; i++) {
		net_voice((uint8_t)i, (uint8_t)i);
	}
	poll_for(10);
	CHECK(m.begins == 1);
	net_lc(DT_VOICE_LC_HEADER, LC2);	// no terminator for call 1
	for (int i = 0; i < 6; i++) {
		net_voice((uint8_t)i, (uint8_t)(40 + i));
	}
	poll_for(10);
	air();
	poll_for(10);
	CHECK(m.ends == 1);
	CHECK(m.begins == 1);	// call 2 waits for call 1 to be on air
	CHECK(m.log[7].kind == DMR_BURST_TERMINATOR_LC && memcmp(m.log[7].payload, LC1, 9) == 0);
	tx_done();
	poll_for(10);
	CHECK(m.begins == 2);
	CHECK(memcmp(m.elc, LC2, 9) == 0);
	CHECK(m.log[8].kind == DMR_BURST_VOICE_LC_HEADER && memcmp(m.log[8].payload, LC2, 9) == 0);
	CHECK(strncmp(m.ops, "BQQQQQQQQEBQ", 12) == 0);
}

// Never overwrite unsent frames: NAK 5 when full, the space never wraps (docs/spec/gotchas.md 8.4)
static void test_full(void)
{
	reset_world();
	configure(1);
	// Frames faster than anything can leave (no polling)
	net_lc(DT_VOICE_LC_HEADER, LC1);
	int naks = 0;
	for (int i = 0; i < 40; i++) {
		uint8_t f[33];

		voice_frame((uint8_t)(i % 6), (uint8_t)i, f);
		naks += net(f) == MMDVM_NAK_FULL;
	}
	CHECK(naks > 10);
	const uint8_t *s = status();
	CHECK(s[8] == 0);
	CHECK(s[5] & MMDVM_STATUS_TX_OVERFLOW);
	CHECK(!(status()[5] & MMDVM_STATUS_TX_OVERFLOW));	// cleared once read
}

// The host stops mid-call: our terminator ends it
static void test_net_lost(void)
{
	reset_world();
	configure(1);
	net_lc(DT_VOICE_LC_HEADER, LC1);
	for (int i = 0; i < 6; i++) {
		net_voice((uint8_t)i, (uint8_t)i);
	}
	poll_for(100);
	CHECK(m.begins == 1 && m.ends == 0);
	poll_for(600);
	CHECK(m.ends == 1);
	CHECK(m.log[m.log_count - 1].kind == DMR_BURST_TERMINATOR_LC);
}

static void test_abort(void)
{
	reset_world();
	configure(1);
	net_lc(DT_VOICE_LC_HEADER, LC1);
	for (int i = 0; i < 6; i++) {
		net_voice((uint8_t)i, (uint8_t)i);
	}
	poll_for(10);
	uint8_t slot = 2;
	host(MMDVM_DMR_ABORT, &slot, 1);
	CHECK(m.aborts == 1);
	CHECK(!(status()[5] & MMDVM_STATUS_TX));
	CHECK(last_out(MMDVM_ACK) == NULL || last_out(MMDVM_ACK)[3] != MMDVM_DMR_ABORT);
}

// ---- RF -> host ----

static void rf(const dmr_burst_t *b)
{
	dmr_event_t ev = { .kind = DMR_EV_BURST_RX, .ts = 2, .burst = b };

	hotspot_modem_event(&ev, NULL);
	now += 60;
	hotspot_poll(now);
}

static void rf_lc(uint8_t kind, const uint8_t *lc)
{
	dmr_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.crc_ok = true;
	b.lc_valid = true;
	b.len = 12;
	rs_make(lc, (kind == DMR_BURST_VOICE_LC_HEADER) ? 0x96 : 0x99, b.payload);
	memcpy(b.lc, lc, 9);
	rf(&b);
}

static void rf_voice(uint8_t index, uint8_t seed, const uint8_t *lc)
{
	dmr_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE;
	b.voice_index = index;
	b.crc_ok = true;
	b.len = 27;
	for (int i = 0; i < 27; i++) {
		b.payload[i] = (uint8_t)(seed * 3 + i);
	}
	if (lc != NULL) {
		b.lc_valid = true;
		memcpy(b.lc, lc, 9);
	}
	rf(&b);
}

static void check_lc_frame(const uint8_t *fr, uint8_t dt, const uint8_t *lc)
{
	uint8_t info[12], fcc = 0xFF, fdt = 0xFF;

	CHECK(fr[1] == 37 && fr[3] == (0x40 | dt));
	CHECK(dmrFecSlotTypeDecode(fr + 4, &fcc, &fdt) && fcc == 2 && fdt == dt);
	CHECK(dmrFecBptcDecode(fr + 4, info) == 0);
	CHECK(memcmp(info, lc, 9) == 0);
	CHECK(rs_valid(info, (dt == DT_VOICE_LC_HEADER) ? 0x96 : 0x99));
}

static void test_uplink(void)
{
	reset_world();
	configure(2);
	set_freq(438800000, 438800000, 255);
	out_count = 0;

	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC2);
	CHECK(status()[5] & MMDVM_STATUS_CD);
	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC2);
	for (int i = 0; i < 6; i++) {
		rf_voice((uint8_t)i, (uint8_t)i, (i == 5) ? LC2 : NULL);
	}
	rf_lc(DMR_BURST_TERMINATOR_LC, LC2);

	// Up: header, header, A-F, terminator
	int n = 0;
	const uint8_t *f[16];
	for (int i = 0; i < out_count && n < 16; i++) {
		if (out[i].b[2] == MMDVM_DMR_DATA2) {
			f[n++] = out[i].b;
		}
	}
	CHECK(n == 9);
	if (n == 9) {
		check_lc_frame(f[0], DT_VOICE_LC_HEADER, LC2);
		check_lc_frame(f[1], DT_VOICE_LC_HEADER, LC2);
		for (int i = 0; i < 6; i++) {
			const uint8_t *fr = f[2 + i] + 4;

			CHECK(f[2 + i][3] == ((i == 0) ? 0x20 : i));
			for (int k = 0; k < 13; k++) {
				CHECK(fr[k] == (uint8_t)(i * 3 + k));
				CHECK(fr[20 + k] == (uint8_t)(i * 3 + 14 + k));
			}
			CHECK(((fr[13] & 0xF0) | (fr[19] & 0x0F)) == (uint8_t)(i * 3 + 13));
			if (i == 0) {
				CHECK(fr[14] == 0xF7 && fr[15] == 0xD5);	// MS voice sync 7F7D5DD57DFD, shifted by a nibble
			} else {
				CHECK((fr[13] & 0x0F) == 2);			// EMB color code
			}
		}
		check_lc_frame(f[8], DT_TERMINATOR_WITH_LC, LC2);
	}
	CHECK(count_out(MMDVM_DMR_LOST2) == 0);

	// A call that just stops: DMR_LOST2 500 ms after its last burst
	out_count = 0;
	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC1);
	rf_voice(0, 9, NULL);
	poll_for(300);
	CHECK(count_out(MMDVM_DMR_LOST2) == 0);
	poll_for(300);
	CHECK(count_out(MMDVM_DMR_LOST2) == 1);

	// Late entry: voice from C with the LC known: a header made from it, silence A-B, then C
	out_count = 0;
	poll_for(1000);
	rf_voice(2, 5, LC1);
	CHECK(count_out(MMDVM_DMR_DATA2) == 4);
	if (count_out(MMDVM_DMR_DATA2) == 4) {
		check_lc_frame(out[0].b, DT_VOICE_LC_HEADER, LC1);
		CHECK(out[1].b[3] == 0x20 && out[2].b[3] == 1 && out[3].b[3] == 2);
	}

	// While we transmit, what the modem reports is dropped
	reset_world();
	configure(2);
	net_lc(DT_VOICE_LC_HEADER, LC1);
	net_lc(DT_TERMINATOR_WITH_LC, LC1);
	poll_for(5);
	CHECK(m.begins == 1);
	out_count = 0;
	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC2);
	CHECK(count_out(MMDVM_DMR_DATA2) == 0);
}

static void test_data(void)
{
	// RF: a CSBK with a good CRC goes up as heard
	reset_world();
	configure(2);
	out_count = 0;
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_CSBK;
	b.crc_ok = true;
	b.len = 12;
	for (int i = 0; i < 12; i++) {
		b.payload[i] = (uint8_t)(0xB0 + i);
	}
	rf(&b);
	const uint8_t *fr = last_out(MMDVM_DMR_DATA2);
	CHECK(fr != NULL);
	if (fr != NULL) {
		uint8_t info[12];

		CHECK(fr[3] == 0x43);
		CHECK(dmrFecBptcDecode(fr + 4, info) == 0 && memcmp(info, b.payload, 12) == 0);
	}
	b.crc_ok = false;
	out_count = 0;
	rf(&b);
	CHECK(count_out(MMDVM_DMR_DATA2) == 0);
	// A Rate 1/2 block without a data header before it is noise
	b.kind = DMR_BURST_RATE_12_DATA;
	poll_for(1000);
	rf(&b);
	CHECK(count_out(MMDVM_DMR_DATA2) == 0);

	// Host: a lone CSBK goes out as a data transmission, its CRC as the host made it
	reset_world();
	configure(2);
	uint8_t csbk[12] = { 0xBD, 0x00, 0x80, 0x00, 0x00, 0x5B, 0x31, 0x84, 0x2A, 0x00, 0x12, 0x34 };
	uint8_t f[33];
	data_frame(DT_CSBK, csbk, 2, f);
	CHECK(net(f) == 0);
	poll_for(10);
	CHECK(m.begins == 1 && m.ends == 1 && m.log_count == 1);
	CHECK(m.log[0].kind == DMR_BURST_CSBK && m.log[0].host_crc && m.log[0].len == 12);
	CHECK(memcmp(m.log[0].payload, csbk, 12) == 0);
	CHECK(status()[5] & MMDVM_STATUS_TX);
	tx_done();
	poll_for(10);
	CHECK(!(status()[5] & MMDVM_STATUS_TX));
}


// ---- Audit fixes (docs/testing/mmdvm-audit.md) ----

static uint8_t burst_bit(const uint8_t *f, int n)
{
	return (f[n / 8] >> (7 - (n % 8))) & 1;
}

// Voice B-F as sent up: the EMB (bits 108-115 and 148-155) decoded with QR(16,7,6), the 32 embedded bits between
static void read_emb(const uint8_t *frame33, uint16_t *emb, uint8_t frag[4])
{
	uint16_t e = 0;

	for (int i = 0; i < 8; i++) {
		e = (uint16_t)((e << 1) | burst_bit(frame33, 108 + i));
	}
	for (int i = 0; i < 8; i++) {
		e = (uint16_t)((e << 1) | burst_bit(frame33, 148 + i));
	}
	memset(frag, 0, 4);
	for (int i = 0; i < 32; i++) {
		frag[i / 8] |= (uint8_t)(burst_bit(frame33, 116 + i) << (7 - (i % 8)));
	}
	*emb = e;
}

// Checks every voice B-F frame up in out[from..]: the EMB decodes cleanly with our colour code, PI 0 and the LCSS of
// its position, and B-E reassemble to embedded LCs. Returns the number of embedded LCs; the last ones in lcs[]
static int check_uplink_emb(int from, uint8_t cc, uint8_t lcs[][9], int max)
{
	dmr_emb_rx_t rx;
	int n = 0;

	dmr_emb_rx_reset(&rx);
	for (int i = from; i < out_count; i++) {
		const uint8_t *f = out[i].b;
		uint8_t ctl = f[3];

		if ((f[2] != MMDVM_DMR_DATA2) || (ctl < 1) || (ctl > 5)) {
			continue;
		}
		uint16_t emb;
		uint8_t frag[4], fcc = 0xFF, lcss = 0xFF, lc[9];
		bool pi = true;

		read_emb(f + 4, &emb, frag);
		CHECK(dmrFecEmbDecode(emb, &fcc, &pi, &lcss) == 0);
		CHECK(fcc == cc && !pi && lcss == dmr_emb_lcss(ctl));
		if (ctl == 5) {
			CHECK(frag[0] == 0 && frag[1] == 0 && frag[2] == 0 && frag[3] == 0);	// F: null embedded signalling
		}
		if (dmr_emb_rx_push(&rx, frag, lcss, lc)) {
			if (n < max) {
				memcpy(lcs[n], lc, 9);
			}
			n++;
		}
	}
	return n;
}

// A-3: B-F carry a correct EMB and the call's embedded LC; a talker alias block heard goes up in the next superframe
static void test_uplink_emb(void)
{
	static const uint8_t TA[9] = { 0x04, 0x00, 0x51, 0x50, 0x55, 0x4C, 0x53, 0x45, 0x00 };	// FLCO 4, "PULSE"
	uint8_t lcs[8][9];

	reset_world();
	configure(5);
	out_count = 0;
	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC2);
	for (int sf = 0; sf < 3; sf++) {
		for (int i = 0; i < 6; i++) {
			rf_voice((uint8_t)i, (uint8_t)(sf * 6 + i), ((sf == 0) && (i == 5)) ? TA : NULL);
		}
	}
	rf_lc(DMR_BURST_TERMINATOR_LC, LC2);
	CHECK(count_out(MMDVM_DMR_DATA2) == 20);
	int n = check_uplink_emb(0, 5, lcs, 8);
	CHECK(n == 3);
	if (n == 3) {
		CHECK(memcmp(lcs[0], LC2, 9) == 0);
		CHECK(memcmp(lcs[1], TA, 9) == 0);	// the alias heard in superframe 1, passed on in superframe 2
		CHECK(memcmp(lcs[2], LC2, 9) == 0);
	}

	// Late entry: the silence from A and the voice after it carry the same well-formed signalling
	reset_world();
	configure(5);
	out_count = 0;
	rf_voice(3, 1, LC1);
	rf_voice(4, 2, NULL);
	rf_voice(5, 3, NULL);
	for (int i = 0; i < 6; i++) {
		rf_voice((uint8_t)i, (uint8_t)(4 + i), NULL);
	}
	n = check_uplink_emb(0, 5, lcs, 8);
	CHECK(n == 2 && memcmp(lcs[0], LC1, 9) == 0 && memcmp(lcs[1], LC1, 9) == 0);
}

// A-4: E heard, F and A missed, then B: silence F and A at their positions (a wrapped gap), then B
static void test_uplink_wrapped_gap(void)
{
	uint8_t lcs[4][9];

	reset_world();
	configure(2);
	out_count = 0;
	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC1);
	for (int i = 0; i < 5; i++) {
		rf_voice((uint8_t)i, (uint8_t)i, NULL);
	}
	int from = out_count;
	now += 120;		// F and A not heard
	rf_voice(1, 7, NULL);
	CHECK(out_count - from == 3);
	if (out_count - from == 3) {
		CHECK(out[from].b[3] == 5 && out[from + 1].b[3] == 0x20 && out[from + 2].b[3] == 1);
		CHECK(memcmp(out[from].b + 4, HOTSPOT_UPLINK_SILENCE, 13) == 0);
		CHECK(memcmp(out[from + 1].b + 4, HOTSPOT_UPLINK_SILENCE, 13) == 0);
		CHECK(out[from + 2].b[4] == (uint8_t)(7 * 3));
	}
	for (int i = 2; i < 6; i++) {
		rf_voice((uint8_t)i, (uint8_t)(8 + i), NULL);
	}
	CHECK(check_uplink_emb(0, 2, lcs, 4) == 2);	// the sequence MMDVMHost checks carried on: both superframes' LCs
	CHECK(count_out(MMDVM_DMR_LOST2) == 0);
}

// A-9: RSSI bytes 00, raw (AT1846S scale) after bursts heard, when known; none on bursts we made
static void test_uplink_rssi(void)
{
	reset_world();
	configure(2);
	set_freq(438800000, 438800000, 255);
	out_count = 0;
	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC1);	// rssi_dbm 0: unknown, no bytes
	CHECK(out[out_count - 1].b[1] == 37);

	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE;
	b.voice_index = 2;
	b.crc_ok = true;
	b.len = 27;
	b.rssi_dbm = -80;
	b.lc_valid = true;
	memcpy(b.lc, LC2, 9);
	poll_for(1000);
	out_count = 0;
	rf(&b);			// late entry at C: a header, silence A and B (all ours, no RSSI), then C with its RSSI
	CHECK(out_count == 4);
	if (out_count == 4) {
		CHECK(out[0].b[1] == 37 && out[1].b[1] == 37 && out[2].b[1] == 37);
		CHECK(out[3].b[1] == 39 && out[3].b[37] == 0 && out[3].b[38] == 71);	// -80 + 151
	}

	// VHF: rfgw's -164 + raw x 32 / 27 gives the dBm back; the radio's own reading when the burst has none
	reset_world();
	hotspot_set_radio(&radio_ops_full);
	configure(2);
	set_freq(145500000, 145500000, 255);
	r.rssi = -100;
	out_count = 0;
	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC1);
	const uint8_t *f = last_out(MMDVM_DMR_DATA2);
	CHECK(f != NULL && f[1] == 39 && f[37] == 0);
	if ((f != NULL) && (f[1] == 39)) {
		int dbm = -164 + f[38] * 32 / 27;

		CHECK(dbm >= -101 && dbm <= -99);
	}
}

// A-10 / C-16: SET_FREQ during an uplink call: DMR_LOST2 before the ACK, nothing from the old frequency after it
static void test_retune_lost(void)
{
	reset_world();
	configure(2);
	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC1);
	rf_voice(0, 1, NULL);
	dmr_burst_t b;	// heard on the old frequency, still in the event queue
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE;
	b.voice_index = 1;
	b.crc_ok = true;
	b.len = 27;
	dmr_event_t ev = { .kind = DMR_EV_BURST_RX, .ts = 2, .burst = &b };
	hotspot_modem_event(&ev, NULL);
	out_count = 0;
	set_freq(439000000, 439000000, 255);
	CHECK(out_count == 2 && out[0].b[2] == MMDVM_DMR_LOST2 && out[1].b[2] == MMDVM_ACK);
	poll_for(1000);
	CHECK(count_out(MMDVM_DMR_DATA2) == 0 && count_out(MMDVM_DMR_LOST2) == 1);
	// No call open: just the ACK
	out_count = 0;
	set_freq(439100000, 439100000, 255);
	CHECK(out_count == 1 && out[0].b[2] == MMDVM_ACK);
}

// A-11 / D-12: SET_MODE idle mid-call ends the call with its terminator and drops the queue
static void test_mode_idle(void)
{
	reset_world();
	configure(1);		// state DMR
	net_lc(DT_VOICE_LC_HEADER, LC1);
	for (int i = 0; i < 18; i++) {
		net_voice((uint8_t)(i % 6), (uint8_t)i);
	}
	poll_for(10);
	CHECK(m.begins == 1 && m.log_count == 8);	// the modem's queue is full, the rest waits in ours
	uint8_t mode = MMDVM_MODE_IDLE;
	out_count = 0;
	host(MMDVM_SET_MODE, &mode, 1);
	CHECK(ack_for(MMDVM_SET_MODE));
	air();
	poll_for(10);
	CHECK(m.log_count == 9 && m.ends == 1);
	CHECK(m.log[8].kind == DMR_BURST_TERMINATOR_LC && memcmp(m.log[8].payload, LC1, 9) == 0);
	tx_done();
	poll_for(10);
	const uint8_t *st = status();
	CHECK(!(st[5] & MMDVM_STATUS_TX) && st[4] == MMDVM_MODE_IDLE && st[8] == 22);
	// Buffering only (nothing keyed yet): dropped, no transmission
	reset_world();
	configure(1);
	net_lc(DT_VOICE_LC_HEADER, LC1);
	host(MMDVM_SET_MODE, &mode, 1);
	poll_for(500);
	CHECK(m.begins == 0 && !(status()[5] & MMDVM_STATUS_TX));
}

// A-12: POCSAG state accepted: no pages, DMR heard but not passed up, POCSAG space 0
static void test_pocsag(void)
{
	reset_world();
	configure(2);
	uint8_t mode = MMDVM_MODE_POCSAG;
	out_count = 0;
	host(MMDVM_SET_MODE, &mode, 1);
	CHECK(ack_for(MMDVM_SET_MODE));
	const uint8_t *st = status();
	CHECK(st[4] == MMDVM_MODE_POCSAG && st[12] == 0);
	out_count = 0;
	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC1);
	CHECK(count_out(MMDVM_DMR_DATA2) == 0);
	CHECK(status()[5] & MMDVM_STATUS_CD);	// still heard
	uint8_t junk[33] = { 0 };
	host(MMDVM_POCSAG_DATA, junk, 20);
	CHECK(nak_for(MMDVM_POCSAG_DATA, MMDVM_NAK_WRONG_MODE));
	uint8_t f[33];
	lc_frame(DT_VOICE_LC_HEADER, LC1, f);
	CHECK(net(f) == MMDVM_NAK_WRONG_MODE);
	// C-11: 06, 00, 02, 02
	mode = MMDVM_MODE_IDLE;
	host(MMDVM_SET_MODE, &mode, 1);
	mode = MMDVM_MODE_DMR;
	host(MMDVM_SET_MODE, &mode, 1);
	host(MMDVM_SET_MODE, &mode, 1);
	CHECK(count_out(MMDVM_ACK) == 3);
	poll_for(1000);
	out_count = 0;
	rf_lc(DMR_BURST_VOICE_LC_HEADER, LC2);
	CHECK(count_out(MMDVM_DMR_DATA2) == 1);	// passed up again
	// SET_CONFIG with state 06 (and the POCSAG enable bit)
	uint8_t c[23] = { 0x80, 0x22, 10, MMDVM_MODE_POCSAG, 128, 128, 1 };
	out_count = 0;
	host(MMDVM_SET_CONFIG, c, sizeof(c));
	CHECK(ack_for(MMDVM_SET_CONFIG));
	st = status();
	CHECK(st[3] == 0x22 && st[4] == MMDVM_MODE_POCSAG);
}

// A-6 / C-13 / C-14: SEND_CWID ACKed in idle (keyed when the radio can), NAK 5 otherwise
static void test_cwid(void)
{
	static const uint8_t CALL[5] = { 'T', 'E', 'S', 'T', '1' };
	hotspot_stats_t st;

	// No CW keying on this radio: ACK, counted, nothing keyed
	reset_world();
	configure(1);		// state DMR
	out_count = 0;
	host(MMDVM_SEND_CWID, CALL, 5);
	CHECK(out_count == 1 && nak_for(MMDVM_SEND_CWID, MMDVM_NAK_FULL));
	uint8_t mode = MMDVM_MODE_IDLE;
	host(MMDVM_SET_MODE, &mode, 1);
	out_count = 0;
	host(MMDVM_SEND_CWID, CALL, 5);
	CHECK(out_count == 1 && ack_for(MMDVM_SEND_CWID));
	hotspot_get_stats(&st);
	CHECK(st.cwid_requests == 1 && st.cwid_unsent == 1);
	CHECK(!(status()[5] & MMDVM_STATUS_TX));

	// With cw_id: keyed, the TX flag set while it runs
	reset_world();
	hotspot_set_radio(&radio_ops_full);
	r.cw_ok = true;
	configure(1);
	host(MMDVM_SET_MODE, &mode, 1);
	out_count = 0;
	host(MMDVM_SEND_CWID, CALL, 5);
	CHECK(ack_for(MMDVM_SEND_CWID) && r.cw_calls == 1 && strcmp(r.cw, "TEST1") == 0);
	CHECK(status()[5] & MMDVM_STATUS_TX);
	host(MMDVM_SEND_CWID, CALL, 5);		// still keying: not idle
	CHECK(nak_for(MMDVM_SEND_CWID, MMDVM_NAK_FULL) && r.cw_calls == 1);
	r.cw_keyed = false;
	CHECK(!(status()[5] & MMDVM_STATUS_TX));
	hotspot_get_stats(&st);
	CHECK(st.cwid_requests == 1 && st.cwid_unsent == 0);
}

// A-8: the extension sets the space bytes (SFR: the outbound slot's space at offsets 7 and 8)
static void ext_space(uint8_t *slot1, uint8_t *slot2) { *slot1 = *slot2; }

static void test_ext_space(void)
{
	static const mmdvm_ext_t ext = { .space = ext_space };

	reset_world();
	configure(1);
	CHECK(status()[7] == 0);
	mmdvm_set_ext(&ext);
	const uint8_t *st = status();
	CHECK(st[7] == 22 && st[8] == 22);
}

// A-2 / T-USB-2: hotspot mode never depends on DTR being set, only on it dropping
static void test_hostlink_dtr(void)
{
	hostlink_session_t hs;

	// Windows MMDVMHost: DTR clear all along, polling: stays
	hostlink_session_start(&hs, false);
	for (int i = 0; i < 100; i++) {
		CHECK(hostlink_session_check(&hs, false, true, false, 250) == HOSTLINK_STAY);
	}
	CHECK(hostlink_session_check(&hs, false, true, false, HOSTLINK_HOTSPOT_IDLE_MS + 1) == HOSTLINK_LEAVE_IDLE);
	// DTR raised later (a host that sets it after opening): still stays; then dropped: the port was closed
	hostlink_session_start(&hs, false);
	CHECK(hostlink_session_check(&hs, true, true, false, 10) == HOSTLINK_STAY);
	CHECK(hostlink_session_check(&hs, true, true, false, 10) == HOSTLINK_STAY);
	CHECK(hostlink_session_check(&hs, false, true, false, 10) == HOSTLINK_LEAVE_CLOSED);
	// Linux / macOS: DTR set on open, dropped on close
	hostlink_session_start(&hs, true);
	CHECK(hostlink_session_check(&hs, true, true, false, 10) == HOSTLINK_STAY);
	CHECK(hostlink_session_check(&hs, false, true, false, 10) == HOSTLINK_LEAVE_CLOSED);
	// The user's exit, and a port that went away
	hostlink_session_start(&hs, false);
	CHECK(hostlink_session_check(&hs, false, true, true, 10) == HOSTLINK_LEAVE_USER);
	hostlink_session_start(&hs, false);
	CHECK(hostlink_session_check(&hs, false, false, false, 10) == HOSTLINK_LEAVE_GONE);
}

static void test_end(void)
{
	reset_world();
	configure(2);
	CHECK(hotspot_running());
	mmdvm_end();
	CHECK(!hotspot_running() && m.stops == 1 && r.leaves == 1);
	// The modem refuses to start: the radio gets it back
	reset_world();
	m.start_ok = false;
	configure(2);
	CHECK(!hotspot_running() && r.enters == 1 && r.leaves == 1);
}

int main(void)
{
	test_framer();
	test_version_status();
	test_slow_enter();
	test_config();
	test_extension();
	test_net_call();
	test_short_call();
	test_back_to_back();
	test_full();
	test_net_lost();
	test_abort();
	test_uplink();
	test_data();
	test_uplink_emb();
	test_uplink_wrapped_gap();
	test_uplink_rssi();
	test_retune_lost();
	test_mode_idle();
	test_pocsag();
	test_cwid();
	test_ext_space();
	test_hostlink_dtr();
	test_end();
	if (failures == 0) {
		printf("mmdvmTest: all passed\n");
	}
	return failures != 0;
}
