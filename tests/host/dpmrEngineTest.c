// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The dPMR engine (core/dpmr/dpmr_engine.c) and the air interface session under it, as NXDN's engine is tested
// (nxdnEngineTest.c): two engines on two made-up providers whose transmit queues feed each other's receivers a chunk
// at a time. A call from one is heard by the other with its IDs and its voice in order, whole superframes, an end
// frame; an encoder that stalls is covered with silence and never underruns the air; late entry; the "own calls only"
// filter; an abort. Then a call through a modeled discriminator tap and the soft modem at 2400 symbols/s, with noise,
// to where it breaks. IDs are arbitrary 24-bit numbers, never anyone's; voice frames are made-up 72-bit patterns.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dpmr/dpmr_engine.h"
#include "air/air_syms.h"
#include "air/air_softmodem.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0xD9A1E;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

// ── Two providers, each other's far end. A chunk is 30 ms of symbols at the session's rate ──
#define CHUNK_MAX	144
#define DEPTH		4
typedef struct {
	air_event_cb_t cb;
	void *ctx;
	bool open, on, begun_ev;
	uint16_t chunk;
	air_tx_chunk_t q[DEPTH];
	uint8_t qn;
	uint32_t underruns, sent_syms, now_us;
	bool keyed;
	uint32_t keyed_violations;
} prov_t;
static prov_t prov[2];

static void ev(prov_t *p, uint8_t kind, const air_chunk_t *c)
{
	air_event_t e = { .kind = kind, .t_us = p->now_us, .chunk = c };

	if (p->cb)
		p->cb(&e, p->ctx);
}

static void deliver(prov_t *to, const uint8_t *syms, uint16_t n)
{
	int8_t soft[CHUNK_MAX];
	air_chunk_t c = { .t_us = to->now_us, .nsyms = n, .sync_at = -1, .hard = syms, .soft = soft };

	for (uint16_t i = 0; i < n; i++)
		soft[i] = (int8_t)(32 * air_sym_level(air_sym_get(syms, i)));
	if (to->open && !to->on)
		ev(to, AIR_EV_RX, &c);
}

static void step(uint8_t i)
{
	prov_t *p = &prov[i], *far = &prov[i ^ 1];

	p->now_us += 30000;
	if (!p->on) {
		if (!far->on) {
			uint8_t noise[CHUNK_MAX / 4];

			for (uint8_t k = 0; k < sizeof(noise); k++)
				noise[k] = (uint8_t)rng();
			deliver(p, noise, p->chunk);
		}
		return;
	}
	if (!p->keyed)
		p->keyed_violations++;
	if (!p->begun_ev) {
		p->begun_ev = true;
		ev(p, AIR_EV_TX_STARTED, NULL);
	}
	if (!p->qn) {
		uint8_t idle[CHUNK_MAX / 4];

		memset(idle, 0x5F, sizeof(idle));
		p->underruns++;
		deliver(far, idle, p->chunk);
		ev(p, AIR_EV_TX_UNDERRUN, NULL);
		return;
	}
	air_tx_chunk_t c = p->q[0];

	memmove(p->q, p->q + 1, (size_t)(--p->qn) * sizeof(c));
	p->sent_syms += c.nsyms;
	deliver(far, c.syms, c.nsyms);
	if (c.flags & AIR_TX_LAST) {
		p->on = false;
		ev(p, AIR_EV_TX_DONE, NULL);
	} else {
		ev(p, AIR_EV_TX_SPACE, NULL);
	}
}

// The board's answer: dPMR's profile (2400 symbols/s, its three patterns), nothing else
static uint32_t p_fit(const air_profile_t *pr)
{
	return pr->sym_rate == 2400 && pr->nsyncs == 3 ? AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT : 0;
}
static bool p_open(uint8_t i, const air_profile_t *pr, air_event_cb_t cb, void *ctx)
{
	memset(&prov[i], 0, sizeof(prov[i]));
	prov[i].cb = cb;
	prov[i].ctx = ctx;
	prov[i].open = true;
	prov[i].chunk = (uint16_t)(pr->sym_rate * 3 / 100);
	return true;
}
static void p_close(uint8_t i) { prov[i].open = prov[i].on = false; prov[i].cb = NULL; }
static bool p_begin(uint8_t i) { if (!prov[i].qn) return false; prov[i].on = true; prov[i].begun_ev = false; return true; }
static bool p_queue(uint8_t i, const air_tx_chunk_t *c)
{
	if (prov[i].qn == DEPTH)
		return false;
	prov[i].q[prov[i].qn++] = *c;
	return true;
}
static void p_end(uint8_t i) { if (prov[i].qn) prov[i].q[prov[i].qn - 1].flags |= AIR_TX_LAST; }
static void p_abort(uint8_t i) { prov[i].qn = 0; prov[i].on = false; ev(&prov[i], AIR_EV_TX_DONE, NULL); }

#define PROVIDER(i) \
	static bool open##i(const air_profile_t *pr, air_event_cb_t cb, void *ctx) { return p_open(i, pr, cb, ctx); } \
	static void close##i(void) { p_close(i); } \
	static void restart##i(void) { } \
	static bool begin##i(void) { return p_begin(i); } \
	static bool queue##i(const air_tx_chunk_t *c) { return p_queue(i, c); } \
	static uint8_t space##i(void) { return (uint8_t)(DEPTH - prov[i].qn); } \
	static void end##i(void) { p_end(i); } \
	static void abort##i(void) { p_abort(i); } \
	static uint32_t now##i(void) { return prov[i].now_us; } \
	static hal_air_t air##i = { p_fit, open##i, close##i, restart##i, begin##i, queue##i, space##i, end##i, abort##i, \
				    now##i, NULL, NULL, NULL, NULL, 72, DEPTH, 60, 100 };
PROVIDER(0)
PROVIDER(1)
// ── The two radios ──
static const uint8_t silence[DPMR_AMBE_BYTES] = { 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A, 0xA5 };
#define VMAX	240
typedef struct {
	dpmr_engine_t e;
	uint8_t starts, updates, ends;
	dpmr_call_t call;
	uint8_t voice[VMAX][DPMR_AMBE_BYTES];
	uint16_t nvoice;
	uint8_t enc[VMAX][DPMR_AMBE_BYTES];
	uint16_t enc_made, enc_taken;
} radio_t;
static radio_t r[2];

static void h_start(const dpmr_call_t *c, void *ctx) { radio_t *x = ctx; x->starts++; x->call = *c; }
static void h_update(const dpmr_call_t *c, void *ctx) { radio_t *x = ctx; x->updates++; x->call = *c; }
static void h_end(void *ctx) { radio_t *x = ctx; x->ends++; }
static void h_voice(const uint8_t ambe[DPMR_AMBE_BYTES], void *ctx)
{
	radio_t *x = ctx;

	if (x->nvoice < VMAX)
		memcpy(x->voice[x->nvoice++], ambe, DPMR_AMBE_BYTES);
}
static bool h_tx_voice(uint8_t ambe[DPMR_AMBE_BYTES], void *ctx)
{
	radio_t *x = ctx;

	if (x->enc_taken == x->enc_made)
		return false;
	memcpy(ambe, x->enc[x->enc_taken++], DPMR_AMBE_BYTES);
	return true;
}
static const dpmr_engine_hooks_t hooks = { h_start, h_update, h_voice, h_end, h_tx_voice };

static void encode(radio_t *x, uint8_t n)
{
	while (n-- && x->enc_made < VMAX) {
		for (uint8_t k = 0; k < DPMR_AMBE_BYTES; k++)
			x->enc[x->enc_made][k] = (uint8_t)rng();
		x->enc_made++;
	}
}

static void begin(uint8_t i, const dpmr_engine_cfg_t *cfg)
{
	hal_air_t *air = i ? &air1 : &air0;

	memset(&r[i], 0, sizeof(r[i]));
	air->tx_chunk_syms = (uint16_t)(dpmr_air_mode.profile.sym_rate * 3 / 100);
	CHECK(dpmr_engine_begin(&r[i].e, air, cfg, &hooks, &r[i]), "radio %u begins", i);
}

static void steps(uint16_t n)
{
	while (n--) {
		step(0);
		step(1);
	}
}

static bool done(void)
{
	if (dpmr_engine_tx_state(&r[0].e) != AIR_TX_DONE)
		return false;
	prov[0].keyed = false;
	dpmr_engine_tx_clear(&r[0].e);
	return true;
}

// Radio 0 transmits: `n` chunk periods with the key down (the encoder makes 3 frames per 2 periods, none during
// stall_from..stall_to), then lets go and runs until the air is quiet. Returns the periods it took
static uint16_t over(uint16_t n_key, uint16_t stall_from, uint16_t stall_to)
{
	uint16_t n = 0;

	CHECK(dpmr_engine_tx_prepare(&r[0].e), "the headers are queued");
	CHECK(dpmr_engine_tx_state(&r[0].e) == AIR_TX_PRIMING && !prov[0].on, "primed, not sending");
	prov[0].keyed = true;
	CHECK(dpmr_engine_tx_go(&r[0].e), "go");
	for (; n < 600; n++) {
		if (n == n_key)
			dpmr_engine_tx_stop(&r[0].e);
		if (n < n_key && !(n >= stall_from && n < stall_to))
			encode(&r[0], (uint8_t)(1 + (n & 1)));
		steps(1);
		if (done())
			break;
	}
	steps(20);
	return n;
}

// The frames heard against the frames made: the made ones in order (silence between them is filler), from the
// first heard. Returns the number out of order or wrong
static uint16_t check_order(const radio_t *rx, const radio_t *tx, uint16_t *first)
{
	uint16_t bad = 0, k = 0, i = 0;

	while (i < rx->nvoice && !memcmp(rx->voice[i], tx->e.cfg.silence, DPMR_AMBE_BYTES))
		i++;
	while (k < tx->enc_made && i < rx->nvoice && memcmp(rx->voice[i], tx->enc[k], DPMR_AMBE_BYTES))
		k++;	// a late entry: the first ones are missing
	*first = k;
	for (; i < rx->nvoice; i++) {
		if (!memcmp(rx->voice[i], tx->e.cfg.silence, DPMR_AMBE_BYTES))
			continue;
		if (k >= tx->enc_made || memcmp(rx->voice[i], tx->enc[k++], DPMR_AMBE_BYTES))
			bad++;
	}
	return bad;
}

#define CALLED	DPMR_ID_COMMON(42)
#define OWN_A	0x00A001u
#define OWN_B	0x00B002u
#define CODE	0x1D4u		// group A, two symbols from FS2

static dpmr_engine_cfg_t base_cfg(uint32_t own)
{
	dpmr_engine_cfg_t cfg = { .called = CALLED, .own = own, .cc = CODE, .rx_mine = false, .headers = 2,
				  .silence = silence };

	return cfg;
}

static void testCall(void)
{
	dpmr_engine_cfg_t cfg = base_cfg(OWN_A);
	dpmr_engine_view_t v;
	uint16_t n, first, bad;

	CHECK(dpmr_engine_fit(&air0) == (AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT) && dpmr_engine_fit(NULL) == 0, "fit");
	begin(0, &cfg);
	cfg.own = OWN_B;
	begin(1, &cfg);
	steps(10);
	CHECK(r[0].starts == 0 && r[1].starts == 0, "noise starts no call");

	n = over(100, 0, 0);	// 3 s
	// The end: the superframe in hand finished (up to 320 ms) and the end frame, plus the queue's lead
	CHECK(n > 100 && n < 116, "the over ended %u periods after 100", n);
	CHECK(prov[0].underruns == 0 && prov[0].keyed_violations == 0 && r[0].e.session.stats.tx_underruns == 0,
	      "underruns %u, keyed outside permission %u", prov[0].underruns, prov[0].keyed_violations);
	dpmr_engine_view(&r[0].e, &v);
	CHECK(v.tx == AIR_TX_IDLE && v.tx_calls == 1 && v.tx_filler <= 8, "sender's view: tx %u calls %u filler %u", v.tx,
	      v.tx_calls, v.tx_filler);
	CHECK(r[1].starts == 1 && r[1].ends == 1 && r[1].call.called == CALLED && r[1].call.own == OWN_A &&
	      r[1].call.cc == CODE && (r[1].call.have & DPMR_HAVE_HEADER), "heard: starts %u ends %u called %06X own %06X",
	      r[1].starts, r[1].ends, r[1].call.called, r[1].call.own);
	bad = check_order(&r[1], &r[0], &first);
	// A call is whole superframes: 16 voice frames each
	CHECK(bad == 0 && first == 0 && r[0].enc_taken == r[0].enc_made && r[1].nvoice % 16 == 0 &&
	      r[1].nvoice >= r[0].enc_made && r[1].nvoice < r[0].enc_made + 16 + v.tx_filler,
	      "frames made %u heard %u, %u wrong", r[0].enc_made, r[1].nvoice, bad);
	dpmr_engine_view(&r[1].e, &v);
	CHECK(!v.rx_active && v.rx_calls == 1 && v.rx.own == OWN_A && v.rx.ended, "receiver's view after the call");
	CHECK(r[1].e.rx.stats.ends == 1 && r[1].e.rx.stats.headers == 2 && r[1].e.rx.stats.cch_fail == 0,
	      "receiver's counts: ends %u headers %u CCH fails %u", r[1].e.rx.stats.ends, r[1].e.rx.stats.headers,
	      r[1].e.rx.stats.cch_fail);
	CHECK(r[0].starts == 0, "the sender did not hear itself");
	printf("  %u voice frames sent, %u heard, over ended %u periods after the key\n", r[0].enc_made, r[1].nvoice,
	       n - 100);

	// The encoder stalls for 300 ms mid-over: silence fills in, the air never runs dry
	r[1].starts = r[1].ends = 0;
	r[1].nvoice = 0;
	r[0].enc_made = r[0].enc_taken = 0;
	over(60, 20, 30);
	dpmr_engine_view(&r[0].e, &v);
	CHECK(prov[0].underruns == 0 && v.tx_filler >= 10 && v.tx_filler <= 40, "stall: underruns %u filler %u",
	      prov[0].underruns, v.tx_filler);
	CHECK(r[1].starts == 1 && r[1].ends == 1 && check_order(&r[1], &r[0], &first) == 0, "stall: starts %u ends %u",
	      r[1].starts, r[1].ends);

	// PTT let go before any voice: one superframe of silence, then the end frame (4.2.3: a call is whole superframes)
	r[1].starts = r[1].ends = 0;
	r[1].nvoice = 0;
	r[0].enc_made = r[0].enc_taken = 0;
	over(0, 0, 0);
	dpmr_engine_view(&r[1].e, &v);
	CHECK(r[1].starts == 1 && r[1].ends == 1 && r[1].nvoice == 16 && v.rx.ended, "key and let go: voice %u",
	      r[1].nvoice);

	// Late entry: radio 1 opens its session 1 s into the over. Its IDs come from the CCH, its voice in order
	dpmr_engine_end(&r[1].e);
	r[0].enc_made = r[0].enc_taken = 0;
	CHECK(dpmr_engine_tx_prepare(&r[0].e), "prime");
	prov[0].keyed = true;
	CHECK(dpmr_engine_tx_go(&r[0].e), "go");
	for (n = 0; n < 300; n++) {
		if (n == 33)
			begin(1, &cfg);
		if (n == 100)
			dpmr_engine_tx_stop(&r[0].e);
		if (n < 100)
			encode(&r[0], (uint8_t)(1 + (n & 1)));
		steps(1);
		if (done())
			break;
	}
	steps(20);
	bad = check_order(&r[1], &r[0], &first);
	printf("  late entry at 990 ms: first frame heard %u of %u (%u ms of voice), %u wrong\n", first, r[0].enc_made,
	       first * 20, bad);
	CHECK(r[1].starts == 1 && !(r[1].call.have & DPMR_HAVE_HEADER) && r[1].ends == 1 && bad == 0 && first >= 30 &&
	      first <= 49 + 40, "late entry: starts %u have %02X first %u", r[1].starts, r[1].call.have, first);
	CHECK(r[1].call.called == CALLED && r[1].call.own == OWN_A && r[1].call.cc == CODE, "late entry: IDs");
	dpmr_engine_end(&r[0].e);
	dpmr_engine_end(&r[1].e);
}

static void testFilter(void)
{
	dpmr_engine_cfg_t cfg = base_cfg(OWN_A);
	dpmr_engine_view_t v;

	// A receiver that plays its own calls only, on another common ID: the call is seen, not played
	begin(0, &cfg);
	cfg.own = OWN_B;
	cfg.called = DPMR_ID_COMMON(43);
	cfg.rx_mine = true;
	begin(1, &cfg);
	CHECK(dpmr_engine_tx_prepare(&r[0].e), "prime");
	prov[0].keyed = true;
	CHECK(dpmr_engine_tx_go(&r[0].e), "go");
	for (uint8_t k = 0; k < 20; k++) {
		encode(&r[0], (uint8_t)(1 + (k & 1)));
		steps(1);
	}
	dpmr_engine_view(&r[1].e, &v);
	CHECK(v.rx_active && !v.rx_played && v.rx.called == CALLED && r[1].starts == 0 && r[1].nvoice == 0,
	      "another ID: active %d played %d heard %u", v.rx_active, v.rx_played, r[1].nvoice);
	// The operator switches to that ID mid-call: the next voice plays
	cfg.called = CALLED;
	dpmr_engine_set(&r[1].e, &cfg);
	for (uint8_t k = 0; k < 20; k++) {
		encode(&r[0], (uint8_t)(1 + (k & 1)));
		steps(1);
	}
	CHECK(r[1].starts == 1 && r[1].nvoice >= 8, "our ID now: starts %u heard %u", r[1].starts, r[1].nvoice);
	// Abort: off the air at once, the far end sees the signal go
	dpmr_engine_tx_abort(&r[0].e);
	CHECK(dpmr_engine_tx_state(&r[0].e) == AIR_TX_DONE && !prov[0].on, "aborted");
	prov[0].keyed = false;
	dpmr_engine_tx_clear(&r[0].e);
	steps(30);
	dpmr_engine_view(&r[1].e, &v);
	CHECK(r[1].ends == 1 && !v.rx_active && !v.rx.ended, "the far end's call was lost after the abort: %u", r[1].ends);

	// A call to us, and a call to all, pass the filter; one to another individual does not
	r[1].starts = r[1].ends = 0;
	r[1].e.cfg.called = DPMR_ID_COMMON(43);
	cfg = base_cfg(OWN_A);
	cfg.called = OWN_B;
	dpmr_engine_set(&r[0].e, &cfg);
	CHECK(over(20, 0, 0) > 20 && r[1].starts == 1 && r[1].call.called == OWN_B, "a call to us: starts %u",
	      r[1].starts);
	r[1].starts = 0;
	cfg.called = 0x00C003u;
	dpmr_engine_set(&r[0].e, &cfg);
	over(20, 0, 0);
	CHECK(r[1].starts == 0, "a call to someone else: starts %u", r[1].starts);
	cfg.called = DPMR_ID_ALL;
	dpmr_engine_set(&r[0].e, &cfg);
	over(20, 0, 0);
	CHECK(r[1].starts == 1 && r[1].call.called == DPMR_ID_ALL, "all call: starts %u", r[1].starts);
	// Transmit refused: inhibited, or a closed engine
	cfg.tx_inhibit = true;
	dpmr_engine_set(&r[0].e, &cfg);
	CHECK(!dpmr_engine_tx_prepare(&r[0].e), "inhibited: no transmit");
	dpmr_engine_end(&r[0].e);
	cfg.tx_inhibit = false;
	r[0].e.cfg = cfg;
	CHECK(!dpmr_engine_tx_prepare(&r[0].e), "closed: no transmit");
	dpmr_engine_end(&r[1].e);
}

// ── A call through a modeled discriminator tap and the soft modem (as airSoftmodemTest's channel model) ──
#define FS		240000
#define SPAN		8
#define LEAD		2000
#define LPF_HALF	120
#define SYMS_MAX	6000
#define TRIALS		8
#define WAVE_MAX	(SYMS_MAX * 100 + 2 * LEAD)
static double wave[WAVE_MAX];
static uint32_t wave_n;
static int16_t tap[WAVE_MAX / 10];
static uint32_t tap_n;
static uint8_t air[SYMS_MAX];
static uint32_t air_n;

static double gauss(void)
{
	double u = (rng() % 1000000 + 1) / 1000001.0, w = (rng() % 1000000) / 1000000.0;

	return sqrt(-2 * log(u)) * cos(2 * M_PI * w);
}

// The transmit pulse: root raised cosine (3.4's H(f); its sinc shaper P(f) is left out), DC gain 1
static void modulate(const air_profile_t *p)
{
	static double h[2 * SPAN * 100 + 1];
	int osr = FS / p->sym_rate, n = 2 * SPAN * osr + 1;
	double a = p->shape_param / 100.0, sum = 0;

	for (int i = 0; i < n; i++) {
		double x = (double)(i - SPAN * osr) / osr, v;

		if (fabs(x) < 1e-9)
			v = 1 - a + 4 * a / M_PI;
		else if (fabs(fabs(4 * a * x) - 1) < 1e-6)
			v = a / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * cos(M_PI / (4 * a)));
		else
			v = (sin(M_PI * x * (1 - a)) + 4 * a * x * cos(M_PI * x * (1 + a))) / (M_PI * x * (1 - (4 * a * x) * (4 * a * x)));
		h[i] = v;
		sum += v;
	}
	for (int i = 0; i < n; i++)
		h[i] *= osr / sum;
	wave_n = LEAD + air_n * (uint32_t)osr + LEAD;
	memset(wave, 0, wave_n * sizeof(double));
	for (uint32_t k = 0; k < air_n; k++) {
		long at = LEAD + (long)k * osr - SPAN * osr;
		double lv = air_sym_level(air[k]) * (p->dev_outer_hz / 3.0);

		for (int i = 0; i < n; i++)
			if (at + i >= 0 && at + i < (long)wave_n)
				wave[at + i] += lv * h[i];
	}
}

// wave[] through the tap: a low-pass at hi_hz, sampled at rate, counts_per_khz, an offset, noise of sigma counts
static void sample_tap(uint32_t rate, double hi_hz, double counts_per_khz, double off, double sigma)
{
	static double lpf[2 * LPF_HALF + 1];
	uint32_t dec = FS / rate;
	double sum = 0;

	for (int i = -LPF_HALF; i <= LPF_HALF; i++) {
		double x = 2 * M_PI * hi_hz / FS * i, w = 0.54 + 0.46 * cos(M_PI * i / LPF_HALF);

		lpf[i + LPF_HALF] = (i ? sin(x) / x : 1) * w;
		sum += lpf[i + LPF_HALF];
	}
	tap_n = 0;
	for (uint32_t j = 7 + LPF_HALF; j + LPF_HALF < wave_n; j += dec) {
		double v = 0;

		for (int i = -LPF_HALF; i <= LPF_HALF; i++)
			v += wave[j + (uint32_t)i] * lpf[i + LPF_HALF];
		v = v / sum / 1000 * counts_per_khz + off + sigma * gauss();
		tap[tap_n++] = (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}
static struct {
	uint32_t starts, ends, voice, voice_bits_bad;
	bool header;
} sm;
static uint8_t sm_sent[64][DPMR_AMBE_BYTES];

static void sm_start(const dpmr_call_t *c, void *ctx) { (void)ctx; sm.starts++; sm.header = (c->have & DPMR_HAVE_HEADER) != 0; }
static void sm_update(const dpmr_call_t *c, void *ctx) { (void)c; (void)ctx; }
static void sm_end(const dpmr_call_t *c, void *ctx) { (void)c; (void)ctx; sm.ends++; }
static void sm_voice(const uint8_t ambe[DPMR_AMBE_BYTES], void *ctx)
{
	// Which frame: compare with the best matching sent frame (the TCH bits are raw channel bits: their own FEC is
	// the vocoder's)
	uint32_t best = 72;

	(void)ctx;
	for (uint8_t k = 0; k < 64; k++) {
		uint32_t d = 0;

		for (uint8_t b = 0; b < DPMR_AMBE_BYTES; b++)
			d += (uint32_t)__builtin_popcount(ambe[b] ^ sm_sent[k][b]);
		if (d < best)
			best = d;
	}
	sm.voice++;
	sm.voice_bits_bad += best;
}
static const dpmr_call_sink_t sm_sink = { sm_start, sm_update, sm_voice, sm_end };

static void out_chunk(const air_chunk_t *c, void *ctx)
{
	air_framer_rx(ctx, c);
}

static void testSoftModem(void)
{
	static const double sigmas[] = { 0, 100, 200, 300, 400, 500, 600, 700, 800 };
	static uint8_t fbuf[DPMR_FRAME_BYTES];
	static dpmr_call_rx_t c;
	const air_profile_t *p = &dpmr_air_mode.profile;
	dpmr_hi_t hi = { DPMR_HT_COMM_START, CALLED, OWN_A, DPMR_M_VOICE, 0, DPMR_F_P2P, 0 };
	dpmr_call_tx_t t;
	uint16_t nv = 0;
	bool ended = false;
	int last_whole = -1, last_found = -1;

	// The call: idle symbols ahead (the tap carries noise there, below), two headers, three superframes, an end
	air_n = 0;
	for (uint8_t i = 0; i < 64; i++)
		for (uint8_t b = 0; b < DPMR_AMBE_BYTES; b++)
			sm_sent[i][b] = (uint8_t)rng();
	for (uint16_t i = 0; i < 100; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	dpmr_call_tx_begin(&t, &hi, CODE, 2);
	while (!dpmr_call_tx_done(&t)) {
		uint16_t first, n = dpmr_call_tx_ready(&t, &first);

		if (nv == 48 && !ended) {
			ended = true;
			dpmr_call_tx_end(&t, sm_sent[63]);
			continue;
		}
		if (!n) {
			dpmr_call_tx_voice(&t, sm_sent[nv++]);
			continue;
		}
		for (uint16_t i = 0; i < n; i++)
			air[air_n++] = air_sym_get(dpmr_call_tx_syms(&t), (uint16_t)(first + i));
		dpmr_call_tx_sent(&t, n);
	}
	for (uint16_t i = 0; i < 100; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	modulate(p);
	printf("  soft modem, 2400 symbols/s, 8 kHz tap, 1050 Hz outer deviation = 2100 counts:\n");
	// sigma in counts at 2000 counts/kHz: a symbol level step is 700 Hz = 1400 counts. Eight calls at each level
	for (uint8_t si = 0; si < sizeof(sigmas) / sizeof(sigmas[0]); si++) {
		uint32_t whole = 0, found = 0, late = 0, voice = 0, bad = 0, hi_fail = 0, cch_fail = 0, ham = 0;

		for (uint8_t trial = 0; trial < TRIALS; trial++) {
			static int16_t all[WAVE_MAX / 30 + 20000];
			air_softmodem_t m;
			air_framer_t fr;
			uint32_t n = 0;

			sample_tap(8000, 1800, 2000, 300, sigmas[si]);
			for (uint32_t j = 0; j < 8000; j++)	// 1 s without carrier: discriminator noise
				all[n++] = (int16_t)lround(300 + 5000 * gauss());
			for (uint32_t j = 0; j < tap_n; j++)
				all[n++] = tap[j];
			for (uint32_t j = 0; j < 4000; j++)
				all[n++] = (int16_t)lround(300 + 5000 * gauss());
			memset(&sm, 0, sizeof sm);
			dpmr_call_rx_init(&c, &sm_sink, NULL);
			air_framer_init(&fr, &dpmr_air_mode, &c.rx, fbuf, NULL, DPMR_FRAME_SYMS);
			CHECK(air_softmodem_rx_open(&m, p, 8000, 2000, out_chunk, &fr), "open");
			for (uint32_t from = 0; from < n; from += 256)
				air_softmodem_rx(&m, all + from, (uint16_t)(n - from < 256 ? n - from : 256), 0x10000000u + from * 125u);
			whole += sm.starts == 1 && sm.header && sm.voice >= 48 && sm.ends == 1;
			found += sm.starts >= 1;
			late += sm.starts >= 1 && !sm.header;
			voice += sm.voice;
			bad += sm.voice_bits_bad;
			hi_fail += c.stats.hi_fail;
			cch_fail += c.stats.cch_fail;
			ham += c.stats.ham_fixed;
			if (si < 2)
				CHECK(sm.starts == 1 && sm.header && sm.ends == 1 && sm.voice >= 48 && sm.voice_bits_bad == 0,
				      "noise %.0f: starts %u voice %u bad bits %u", sigmas[si], sm.starts, sm.voice, sm.voice_bits_bad);
		}
		printf("    noise %3.0f counts: calls whole %u/%u, found %u (%u late), voice %3u%% of frames, TCH bit errors %5.2f %%, "
		       "fails HI %u CCH %u, %u Hamming words corrected\n", sigmas[si], whole, TRIALS, found, late,
		       voice * 100 / (48 * TRIALS), voice ? 100.0 * bad / (72.0 * voice) : 0.0, hi_fail, cch_fail, ham);
		if (whole == TRIALS && last_whole == si - 1)
			last_whole = si;
		if (found == TRIALS && last_found == si - 1)
			last_found = si;
	}
	printf("    break points: every call whole up to %.0f counts, every call found up to %.0f counts\n",
	       last_whole >= 0 ? sigmas[last_whole] : -1.0, last_found >= 0 ? sigmas[last_found] : -1.0);
	// NXDN at 4800 bit/s, the same physical layer, holds every call whole to 400 counts here (nxdnEngineTest.c):
	// dPMR's block codes are weaker than NXDN's convolutional code, so its own floor is what is checked
	CHECK(last_whole >= 2 && last_found >= 3, "break points: whole %d found %d", last_whole, last_found);
}

int main(void)
{
	testCall();
	testFilter();
	testSoftModem();
	printf("dpmrEngineTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
