// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The second receiver's SoC as the main SoC's second DMR transmitter (core/rx2/rx2_dmr.c): both ends back to back
// over the real link (rx2_link.c), each with a fake modem
#include <stdio.h>
#include <string.h>
#include "rx2/rx2_link.h"
#include "rx2/rx2_dmr.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- A fake modem: what was called ---
typedef struct {
	int starts, begins, queued, ends, aborts, listens, embs;
	dmr_timing_t timing;
	int8_t ts;
	dmr_access_t access;
	dmr_tx_burst_t last;
	uint8_t emb[9];
	uint8_t space;
	bool refuse, tx;
	dmr_event_cb_t cb;
	void *ctx;
} fake_t;

static fake_t fma_, fmb_;
#define fa fma_
#define fb fmb_	// A's (main) and B's (receiver) modems

#define FAKE_OPS(X)												\
static bool X##_start(const dmr_config_t *c, dmr_event_cb_t cb, void *ctx) { (void)c; f##X##_.starts++; f##X##_.cb = cb; f##X##_.ctx = ctx; return true; } \
static void X##_stop(void) {}											\
static bool X##_tx_begin(dmr_timing_t t, int8_t ts, dmr_access_t a)						\
{ if (f##X##_.refuse) return false; f##X##_.begins++; f##X##_.timing = t; f##X##_.ts = ts; f##X##_.access = a; f##X##_.tx = true; return true; } \
static bool X##_tx_queue(const dmr_tx_burst_t *b) { f##X##_.queued++; f##X##_.last = *b; return true; }		\
static uint8_t X##_tx_queue_space(void) { return f##X##_.space; }							\
static void X##_tx_end(void) { f##X##_.ends++; }									\
static void X##_tx_abort(void) { f##X##_.aborts++; f##X##_.tx = false; }						\
static void X##_set_embedded_lc(const uint8_t lc[9]) { f##X##_.embs++; memcpy(f##X##_.emb, lc, 9); }			\
static bool X##_is_transmitting(void) { return f##X##_.tx; }							\
static void X##_tx_listen(void) { f##X##_.listens++; }								\
static const hal_dmr_modem_t X##_modem = { .start = X##_start, .stop = X##_stop, .tx_begin = X##_tx_begin,	\
	.tx_queue = X##_tx_queue, .tx_queue_space = X##_tx_queue_space, .tx_end = X##_tx_end,			\
	.tx_abort = X##_tx_abort, .set_embedded_lc = X##_set_embedded_lc, .is_transmitting = X##_is_transmitting,	\
	.tx_listen = X##_tx_listen };

FAKE_OPS(ma)
FAKE_OPS(mb)

// --- The wire ---
static uint8_t to_recv[8192], to_main[8192];
static size_t n_to_recv, n_to_main;
static rx2_main_t m;
static rx2_recv_t r;
static uint32_t now;

static void send_main(const uint8_t *p, uint16_t n)
{
	if (n_to_recv + n <= sizeof to_recv) {
		memcpy(&to_recv[n_to_recv], p, n);
		n_to_recv += n;
	}
}

static void send_recv(const uint8_t *p, uint16_t n)
{
	if (n_to_main + n <= sizeof to_main) {
		memcpy(&to_main[n_to_main], p, n);
		n_to_main += n;
	}
}

// --- The receiver's board ---
static int b_splits, b_split_offs, b_arms;
static int8_t b_cc;
static uint32_t b_hz;
static uint8_t b_power;
static bool b_split(bool on, int8_t cc)
{
	if (on) {
		b_splits++;
		b_cc = cc;
	} else {
		b_split_offs++;
	}
	return true;
}
static bool b_arm_refuse;	// the board can't transmit there (its band, its calibration)
static uint32_t a_listen_hz;	// the main receiver's frequency the board gives (0: not known)
static bool a_listen(uint32_t *hz, uint8_t *power)
{
	*hz = a_listen_hz;
	*power = 0;
	return a_listen_hz != 0;
}
static bool b_arm(uint32_t hz, uint8_t power)
{
	b_arms++;
	b_hz = hz;
	b_power = power;
	return hz == 0 || !b_arm_refuse;
}
static void b_ev_send(const uint8_t *p, uint8_t len) { rx2_recv_dmr_ev(&r, p, len); }
static void b_dmr(const uint8_t *p, uint8_t len) { rx2_dmr_recv(p, len); }
static bool b_tune(const rx2_tune_t *t) { (void)t; return true; }
static void b_stop(void) {}
static void b_audio(bool on, uint8_t v) { (void)on; (void)v; }
static void b_status(rx2_status_t *st) { memset(st, 0, sizeof(*st)); }
static const rx2_ops_t b_ops = { .tune = b_tune, .stop = b_stop, .audio = b_audio, .status = b_status, .dmr = b_dmr };

// --- The main SoC's board ---
static int a_splits;
static bool a_split_on;
static bool a_send(const uint8_t *p, uint8_t len) { return rx2_main_dmr(&m, p, len); }
static void a_split(bool on) { a_splits++; a_split_on = on; }
static void a_ev(const uint8_t *p, uint8_t len) { rx2_dmr_main_event(p, len, now); }

// The core's event callback (the SFR runner's)
static int ev_started, ev_done, ev_rejected;
static void on_event(const dmr_event_t *ev, void *ctx)
{
	(void)ctx;
	if (ev->kind == DMR_EV_TX_SLOT_STARTED) {
		ev_started++;
	} else if (ev->kind == DMR_EV_TX_DONE) {
		ev_done++;
	} else if (ev->kind == DMR_EV_TX_REJECTED) {
		ev_rejected++;
	}
}

// Deliver both ways and poll both ends, for ms in 10 ms steps
static void run(uint32_t ms)
{
	for (uint32_t end = now + ms; now < end; now += 10) {
		size_t n = n_to_recv;
		uint8_t buf[sizeof to_recv];
		memcpy(buf, to_recv, n);
		n_to_recv = 0;
		for (size_t i = 0; i < n; i++) {
			rx2_recv_rx(&r, buf[i], now);
		}
		rx2_dmr_recv_poll(now);
		n = n_to_main;
		memcpy(buf, to_main, n);
		n_to_main = 0;
		for (size_t i = 0; i < n; i++) {
			rx2_main_rx(&m, buf[i], now);
		}
		rx2_main_poll(&m, now);
		rx2_recv_poll(&r, now);
		rx2_dmr_main_poll(m.up, now);
	}
}

static const hal_dmr_modem_t *setup(uint8_t flags)
{
	memset(&fa, 0, sizeof(fa));
	memset(&fb, 0, sizeof(fb));
	fb.space = 4;
	n_to_recv = n_to_main = 0;
	b_splits = b_split_offs = b_arms = a_splits = 0;
	b_arm_refuse = false;
	a_listen_hz = 0;
	ev_started = ev_done = ev_rejected = 0;
	rx2_main_init(&m, send_main);
	m.dmr_ev = a_ev;
	rx2_recv_init(&r, send_recv, &b_ops, flags, "t");
	rx2_dmr_recv_cfg_t rc = { &mb_modem, b_split, b_arm, b_ev_send };
	rx2_dmr_recv_init(&rc);
	rx2_dmr_main_cfg_t mc = { &ma_modem, a_send, a_split, a_listen };
	static hal_dmr_modem_t split_modem;
	rx2_dmr_main_init(&mc, &split_modem);
	const hal_dmr_modem_t *mod = &split_modem;
	rx2_main_command(&m, RX2_HELLO_REQ);
	run(100);
	dmr_config_t cfg = { .cc = 2 };
	CHECK(mod->start(&cfg, on_event, NULL));
	CHECK(fa.starts == 1);
	return mod;
}

static dmr_tx_burst_t voice(uint8_t idx)
{
	dmr_tx_burst_t t;
	memset(&t, 0, sizeof(t));
	t.kind = DMR_BURST_VOICE;
	t.voice_index = idx;
	t.sync_override = 3;
	t.len = DMR_VOICE_BYTES;
	for (int i = 0; i < DMR_VOICE_BYTES; i++) {
		t.payload[i] = (uint8_t)(i * 9 + idx);
	}
	return t;
}

// A whole repeat: split, armed, begun, bursts, end
static void test_repeat(void)
{
	const hal_dmr_modem_t *mod = setup(RX2_HELLO_DMR_TX);
	CHECK(m.up);
	CHECK(mod->tx_split(true));
	CHECK(rx2_dmr_main_split() && a_splits == 1 && a_split_on);
	CHECK(rx2_dmr_main_arm(461462500u, 1));
	run(30);
	CHECK(b_splits == 1 && b_cc == 2);
	CHECK(b_arms == 1 && b_hz == 461462500u && b_power == 1);
	CHECK(rx2_dmr_main_state()->ready);

	// Begun: B's chip, aligned to what it hears, A's own transmitter untouched
	CHECK(mod->tx_begin(DMR_TIMING_ALIGNED, 2, DMR_ACCESS_IMPOLITE));
	mod->tx_listen();
	dmr_tx_burst_t v = voice(1);
	CHECK(mod->tx_queue(&v));
	CHECK(mod->tx_queue_space() < 4);	// the burst on the link counts against the last reported space
	uint8_t lc[9] = { 0, 0, 0, 0, 0, 202, 0x2F, 0x52, 0x1C };
	mod->set_embedded_lc(lc);
	CHECK(mod->is_transmitting());
	run(20);
	CHECK(fa.begins == 0 && fa.queued == 0 && fa.listens == 0);
	CHECK(fb.begins == 1 && fb.timing == DMR_TIMING_ALIGNED && fb.ts == 2 && fb.access == DMR_ACCESS_IMPOLITE);
	CHECK(fb.listens == 1);
	CHECK(fb.queued == 1 && memcmp(&fb.last, &v, sizeof(v)) == 0);
	CHECK(fb.embs == 1 && memcmp(fb.emb, lc, 9) == 0);
	CHECK(fa.embs == 1);	// the local modem keeps it too (a fallback starts with it)
	CHECK(mod->tx_queue_space() == 4);	// B reported its space with the burst taken

	// B's events reach the core's callback
	dmr_event_t e = { .kind = DMR_EV_TX_SLOT_STARTED, .ts = 2 };
	fb.cb = NULL;
	rx2_dmr_recv_event(&e);
	e.kind = DMR_EV_BURST_RX;	// not forwarded: A hears for itself
	rx2_dmr_recv_event(&e);
	run(20);
	CHECK(ev_started == 1);
	mod->tx_end();
	run(20);
	CHECK(fb.ends == 1 && ev_done == 0 && mod->is_transmitting());
	e.kind = DMR_EV_TX_DONE;
	rx2_dmr_recv_event(&e);
	run(20);
	CHECK(ev_done == 1 && !mod->is_transmitting());
	// The view's counts (hal_dmr_modem_t.tx_split_stats)
	dmr_split_stats_t sp;
	mod->tx_split_stats(&sp);
	CHECK(sp.ready && sp.remote && sp.bursts == 1 && sp.events >= 2 && sp.fallbacks == 0 && sp.ends_forced == 0);

	// Off: B back to its own settings, A's board back to its own transmitter
	mod->tx_split(false);
	run(30);
	CHECK(b_split_offs >= 1 && !rx2_dmr_main_split() && !a_split_on);
	CHECK(mod->tx_begin(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE) && fa.begins == 1);
}

// B goes quiet mid-transmission: the core gets an end instead of waiting for ever
static void test_quiet(void)
{
	const hal_dmr_modem_t *mod = setup(RX2_HELLO_DMR_TX);
	CHECK(mod->tx_split(true));
	rx2_dmr_main_arm(461462500u, 1);
	run(30);
	CHECK(mod->tx_begin(DMR_TIMING_ALIGNED, 2, DMR_ACCESS_IMPOLITE));
	// B's link task stops: only the main side runs
	for (uint32_t end = now + RX2_DMR_QUIET_MS + 50u; now < end; now += 10) {
		rx2_dmr_main_poll(true, now);
	}
	CHECK(ev_rejected == 1 && !mod->is_transmitting());
	CHECK(rx2_dmr_main_state()->ends_forced == 1);
}

// No transmitter offered: the split is refused and the main SoC transmits itself
static void test_not_offered(void)
{
	const hal_dmr_modem_t *mod = setup(0);
	CHECK(m.up);
	CHECK(!mod->tx_split(true));
	CHECK(!rx2_dmr_main_split() && a_splits == 0);
	CHECK(mod->tx_begin(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE) && fa.begins == 1);
}

// Split but never armed (B can't): tx_begin waits, then the main SoC takes over
static void test_never_ready(void)
{
	const hal_dmr_modem_t *mod = setup(RX2_HELLO_DMR_TX);
	CHECK(mod->tx_split(true));
	run(30);
	CHECK(!rx2_dmr_main_state()->ready);
	CHECK(!mod->tx_begin(DMR_TIMING_ALIGNED, 2, DMR_ACCESS_IMPOLITE));
	run(RX2_DMR_READY_MS);
	CHECK(mod->tx_begin(DMR_TIMING_ALIGNED, 2, DMR_ACCESS_IMPOLITE));
	CHECK(fa.begins == 1 && fb.begins == 0);
	CHECK(!rx2_dmr_main_split() && !a_split_on && rx2_dmr_main_state()->fallbacks == 1);
	run(30);
	CHECK(!rx2_dmr_recv_state()->split && b_split_offs == 1);	// B told: back to receiving
	dmr_split_stats_t sp;
	mod->tx_split_stats(&sp);
	CHECK(!sp.ready && !sp.remote && sp.fallbacks == 1);
}

// B's board refuses the arm: counted, never ready, and the main SoC transmits after the wait
static void test_arm_refused(void)
{
	const hal_dmr_modem_t *mod = setup(RX2_HELLO_DMR_TX);
	CHECK(mod->tx_split(true));
	b_arm_refuse = true;
	rx2_dmr_main_arm(461462500u, 1);
	run(30);
	CHECK(b_arms == 1 && !rx2_dmr_recv_state()->armed && rx2_dmr_recv_state()->arm_fails == 1);
	CHECK(!rx2_dmr_main_state()->ready);
	run(RX2_DMR_READY_MS);
	CHECK(mod->tx_begin(DMR_TIMING_ALIGNED, 2, DMR_ACCESS_IMPOLITE));
	CHECK(fa.begins == 1 && fb.begins == 0 && rx2_dmr_main_state()->fallbacks == 1);
	run(30);
	CHECK(!rx2_dmr_recv_state()->split && b_split_offs == 1);
}

// B refuses the begin: the core hears TX_REJECTED
static void test_rejected(void)
{
	const hal_dmr_modem_t *mod = setup(RX2_HELLO_DMR_TX);
	CHECK(mod->tx_split(true));
	rx2_dmr_main_arm(461462500u, 1);
	run(30);
	fb.refuse = true;
	CHECK(mod->tx_begin(DMR_TIMING_ALIGNED, 2, DMR_ACCESS_IMPOLITE));
	run(30);
	CHECK(ev_rejected == 1 && !mod->is_transmitting());
}

// The main SoC restarts (HELLO_REQ): B drops the role
static void test_restart(void)
{
	const hal_dmr_modem_t *mod = setup(RX2_HELLO_DMR_TX);
	CHECK(mod->tx_split(true));
	rx2_dmr_main_arm(461462500u, 1);
	run(30);
	CHECK(mod->tx_begin(DMR_TIMING_ALIGNED, 2, DMR_ACCESS_IMPOLITE));
	run(20);
	rx2_main_command(&m, RX2_HELLO_REQ);
	run(20);
	CHECK(fb.aborts == 1 && b_hz == 0 && b_split_offs == 1);
	CHECK(!rx2_dmr_recv_state()->split && !rx2_dmr_recv_state()->armed);
}

// The split arms B where A receives at once (its modem has the slot timing before the first repeat), the key-up's arm
// at the same place keeps it, and the end of a transmission leaves it listening until the split ends
static void test_listen(void)
{
	const hal_dmr_modem_t *mod = setup(RX2_HELLO_DMR_TX);
	a_listen_hz = 461462500u;
	CHECK(mod->tx_split(true));
	run(30);
	CHECK(b_arms == 1 && b_hz == 461462500u && rx2_dmr_main_state()->ready);
	CHECK(rx2_dmr_main_arm(461462500u, 0));	// the key-up: already there
	run(30);
	CHECK(b_arms == 1);
	CHECK(rx2_dmr_main_arm(0, 0));		// the transmission's end: B keeps listening
	run(30);
	CHECK(b_arms == 1 && rx2_dmr_recv_state()->armed && rx2_dmr_main_state()->ready);
	CHECK(rx2_dmr_main_arm(461462500u, 1));	// another power level: armed again
	run(30);
	CHECK(b_arms == 2 && b_power == 1);
	mod->tx_split(false);
	run(30);
	CHECK(!rx2_dmr_recv_state()->armed && !rx2_dmr_recv_state()->split);
}

// The core changes the color code after the start (a hotspot's config): B filters on it and is armed again where it was
static void test_cc_change(void)
{
	const hal_dmr_modem_t *mod = setup(RX2_HELLO_DMR_TX);
	a_listen_hz = 461462500u;
	CHECK(mod->tx_split(true));
	run(30);
	CHECK(b_cc == 2 && b_arms == 1 && rx2_dmr_main_state()->ready);
	mod->set_cc_filter(1);	// B's arms: the disarm with the old code, then the arm
	run(30);
	CHECK(b_cc == 1 && rx2_dmr_recv_state()->cc == 1 && b_arms == 3 && b_hz == 461462500u);
	CHECK(rx2_dmr_recv_state()->armed && rx2_dmr_main_state()->ready);
	mod->set_cc_filter(1);	// the same: nothing sent
	run(30);
	CHECK(b_arms == 3);
}

int main(void)
{
	test_cc_change();
	test_listen();
	test_repeat();
	test_quiet();
	test_not_offered();
	test_never_ready();
	test_arm_refused();
	test_rejected();
	test_restart();
	if (failures == 0) {
		printf("rx2DmrTest: all passed\n");
	}
	return failures != 0;
}
