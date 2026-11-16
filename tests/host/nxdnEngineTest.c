// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The NXDN engine (core/nxdn/nxdn_engine.c) and the air interface session under it: two engines on two made-up
// providers whose transmit queues feed each other's receivers a chunk at a time. A call from one is heard by the
// other with its fields and its voice in order; an encoder that stalls is covered with silence and never underruns
// the air; late entry; the RAN and group squelch; encrypted calls seen, not played; an abort; 9600 bit/s. Then a
// call through a modeled discriminator tap and the soft modem at 2400 symbols/s, with noise, to where it breaks.
// Voice frames are made-up 72-bit patterns.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxdn/nxdn_engine.h"
#include "air/air_syms.h"
#include "air/air_softmodem.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0xBADC0DE;
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

// The board's answer: both NXDN rates (2400 and 4800 symbols/s), nothing else
static uint32_t p_fit(const air_profile_t *pr)
{
	return (pr->sym_rate == 2400 || pr->sym_rate == 4800) && pr->nsyncs == 2 ? AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT : 0;
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
				    now##i, NULL, NULL, NULL, 72, DEPTH, 60, 100 };
PROVIDER(0)
PROVIDER(1)

// ── The two radios ──
static const uint8_t silence[NXDN_EHR_BYTES] = { 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A, 0xA5 };
#define VMAX	240
typedef struct {
	nxdn_engine_t e;
	uint8_t starts, updates, ends;
	nxdn_call_t call;
	uint8_t voice[VMAX][NXDN_EHR_BYTES];
	uint16_t nvoice;
	uint8_t enc[VMAX][NXDN_EHR_BYTES];
	uint16_t enc_made, enc_taken;
} radio_t;
static radio_t r[2];

static void h_start(const nxdn_call_t *c, void *ctx) { radio_t *x = ctx; x->starts++; x->call = *c; }
static void h_update(const nxdn_call_t *c, void *ctx) { radio_t *x = ctx; x->updates++; x->call = *c; }
static void h_end(void *ctx) { radio_t *x = ctx; x->ends++; }
static void h_voice(const uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	radio_t *x = ctx;

	if (x->nvoice < VMAX)
		memcpy(x->voice[x->nvoice++], ehr, NXDN_EHR_BYTES);
}
static bool h_tx_voice(uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	radio_t *x = ctx;

	if (x->enc_taken == x->enc_made)
		return false;
	memcpy(ehr, x->enc[x->enc_taken++], NXDN_EHR_BYTES);
	return true;
}
static const nxdn_engine_hooks_t hooks = { h_start, h_update, h_voice, h_end, h_tx_voice };

static void encode(radio_t *x, uint8_t n)
{
	while (n-- && x->enc_made < VMAX) {
		for (uint8_t k = 0; k < NXDN_EHR_BYTES; k++)
			x->enc[x->enc_made][k] = (uint8_t)rng();
		x->enc_made++;
	}
}

static void begin(uint8_t i, const nxdn_engine_cfg_t *cfg, const nxdn_engine_hooks_t *h)
{
	hal_air_t *air = i ? &air1 : &air0;

	memset(&r[i], 0, sizeof(r[i]));
	air->tx_chunk_syms = (uint16_t)(nxdn_engine_mode(cfg->rate9600)->profile.sym_rate * 3 / 100);
	CHECK(nxdn_engine_begin(&r[i].e, air, cfg, h, &r[i]), "radio %u begins", i);
}

static void steps(uint16_t n)
{
	while (n--) {
		step(0);
		step(1);
	}
}

// Radio 0 transmits: `n` chunk periods with the key down (the encoder makes 3 frames per 2 periods, none during
// stall_from..stall_to), then lets go and runs until the air is quiet. Returns the periods it took
static uint16_t over(uint16_t n_key, uint16_t stall_from, uint16_t stall_to)
{
	uint16_t n = 0;

	CHECK(nxdn_engine_tx_prepare(&r[0].e), "the preamble and header are queued");
	CHECK(nxdn_engine_tx_state(&r[0].e) == AIR_TX_PRIMING && !prov[0].on, "primed, not sending");
	prov[0].keyed = true;
	CHECK(nxdn_engine_tx_go(&r[0].e), "go");
	for (; n < 600; n++) {
		if (n == n_key)
			nxdn_engine_tx_stop(&r[0].e);
		if (n < n_key && !(n >= stall_from && n < stall_to))
			encode(&r[0], (uint8_t)(1 + (n & 1)));
		steps(1);
		if (nxdn_engine_tx_state(&r[0].e) == AIR_TX_DONE) {
			prov[0].keyed = false;
			nxdn_engine_tx_clear(&r[0].e);
			break;
		}
	}
	steps(20);
	return n;
}

// The frames heard against the frames made: the made ones in order (silence between them is filler), from the
// first heard. Returns the number out of order or wrong
static uint16_t check_order(const radio_t *rx, const radio_t *tx, uint16_t *first)
{
	uint16_t bad = 0, k = 0, i = 0;

	while (i < rx->nvoice && !memcmp(rx->voice[i], silence, NXDN_EHR_BYTES))
		i++;
	while (k < tx->enc_made && i < rx->nvoice && memcmp(rx->voice[i], tx->enc[k], NXDN_EHR_BYTES))
		k++;	// a late entry: the first ones are missing
	*first = k;
	for (; i < rx->nvoice; i++) {
		if (!memcmp(rx->voice[i], silence, NXDN_EHR_BYTES))
			continue;
		if (k >= tx->enc_made || memcmp(rx->voice[i], tx->enc[k++], NXDN_EHR_BYTES))
			bad++;
	}
	return bad;
}

static nxdn_engine_cfg_t base_cfg(bool rate9600)
{
	nxdn_engine_cfg_t cfg = { .rate9600 = rate9600, .rx_ran = 5, .tx_ran = 5, .unit_id = 1001,
				  .tx_call_type = NXDN_CALL_GROUP, .tx_dst = 77, .silence = silence };

	return cfg;
}

static void testCall(bool rate9600)
{
	nxdn_engine_cfg_t cfg = base_cfg(rate9600);
	nxdn_engine_view_t v;
	uint16_t n, first, bad;
	const char *name = rate9600 ? "9600" : "4800";

	CHECK(nxdn_engine_fit(&air0, rate9600) == (AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT) && nxdn_engine_fit(NULL, false) == 0, "fit");
	begin(0, &cfg, &hooks);
	cfg.unit_id = 2002;
	begin(1, &cfg, &hooks);
	steps(10);
	CHECK(r[0].starts == 0 && r[1].starts == 0, "%s: noise starts no call", name);

	n = over(100, 0, 0);	// 3 s
	// The end: the frame in hand finished (80 ms, or 40 at 9600) and a release frame, plus the queue's lead. Filler:
	// a frame waits for its four voice frames only while the queue holds more than its lead, so the first ones and
	// the last (a frame finished after the key) may be silence
	CHECK(n > 100 && n < 112, "%s: the over ended %u periods after 100", name, n);
	CHECK(prov[0].underruns == 0 && prov[0].keyed_violations == 0 && r[0].e.session.stats.tx_underruns == 0,
	      "%s: underruns %u, keyed outside permission %u", name, prov[0].underruns, prov[0].keyed_violations);
	nxdn_engine_view(&r[0].e, &v);
	CHECK(v.tx == AIR_TX_IDLE && v.tx_calls == 1 && v.tx_filler <= 8, "%s: sender's view: tx %u calls %u filler %u", name, v.tx,
	      v.tx_calls, v.tx_filler);
	CHECK(r[1].starts == 1 && r[1].ends == 1 && !r[1].call.late && r[1].call.vcall.src == 1001 && r[1].call.vcall.dst == 77 &&
	      r[1].call.ran == 5, "%s: heard: starts %u ends %u src %u", name, r[1].starts, r[1].ends, r[1].call.vcall.src);
	CHECK((r[1].call.vcall.voice_opt & 7) == (rate9600 ? NXDN_VOICE_9600_EHR : NXDN_VOICE_4800_EHR), "%s: voice option", name);
	bad = check_order(&r[1], &r[0], &first);
	CHECK(bad == 0 && first == 0 && r[0].enc_taken == r[0].enc_made && r[1].nvoice % 4 == 0 && r[1].nvoice >= r[0].enc_made &&
	      r[1].nvoice < r[0].enc_made + 4 + v.tx_filler, "%s: frames made %u heard %u, %u wrong", name, r[0].enc_made,
	      r[1].nvoice, bad);
	nxdn_engine_view(&r[1].e, &v);
	CHECK(!v.rx_active && v.rx_calls == 1 && v.rx.vcall.src == 1001, "%s: receiver's view after the call", name);
	CHECK(r[0].starts == 0, "%s: the sender did not hear itself", name);
	printf("  %s bit/s: %u voice frames sent, %u heard, over ended %u periods after the key\n", name, r[0].enc_made, r[1].nvoice,
	       n - 100);

	// The encoder stalls for 300 ms mid-over: silence fills in, the air never runs dry
	r[1].starts = r[1].ends = 0;
	r[1].nvoice = 0;
	r[0].enc_made = r[0].enc_taken = 0;
	over(60, 20, 30);
	nxdn_engine_view(&r[0].e, &v);
	CHECK(prov[0].underruns == 0 && v.tx_filler >= 10 && v.tx_filler <= 30, "%s: stall: underruns %u filler %u", name,
	      prov[0].underruns, v.tx_filler);
	CHECK(r[1].starts == 1 && r[1].ends == 1 && check_order(&r[1], &r[0], &first) == 0, "%s: stall: starts %u ends %u", name,
	      r[1].starts, r[1].ends);

	// Late entry: radio 1 opens its session 1 s into the over. Its voice starts at a superframe's end, in order (the
	// queue's lead of about 90 ms puts the frame on air at 990 ms a few frames back)
	nxdn_engine_end(&r[1].e);
	r[0].enc_made = r[0].enc_taken = 0;
	CHECK(nxdn_engine_tx_prepare(&r[0].e), "prime");
	prov[0].keyed = true;
	CHECK(nxdn_engine_tx_go(&r[0].e), "go");
	for (n = 0; n < 300; n++) {
		if (n == 33)
			begin(1, &cfg, &hooks);
		if (n == 100)
			nxdn_engine_tx_stop(&r[0].e);
		if (n < 100)
			encode(&r[0], (uint8_t)(1 + (n & 1)));
		steps(1);
		if (nxdn_engine_tx_state(&r[0].e) == AIR_TX_DONE) {
			prov[0].keyed = false;
			nxdn_engine_tx_clear(&r[0].e);
			break;
		}
	}
	steps(20);
	bad = check_order(&r[1], &r[0], &first);
	printf("  %s late entry at 990 ms: first frame heard %u of %u (%u ms of voice), %u wrong\n", name, first, r[0].enc_made,
	       first * 20, bad);
	CHECK(r[1].starts == 1 && r[1].call.late && r[1].ends == 1 && bad == 0 && first >= 40 && first <= 49 + 40,
	      "%s: late entry: starts %u late %u first %u", name, r[1].starts, r[1].call.late, first);
	nxdn_engine_end(&r[0].e);
	nxdn_engine_end(&r[1].e);
}

// An encrypted call (as a radio with a key sends it: VCALL with the cipher, VCALL_IV in place of the VCALL in the
// header's second FACCH1) delivered to radio 1 as symbols
static void send_encrypted(void)
{
	nxdn_vcall_t v = { NXDN_MSG_VCALL, 0, NXDN_CALL_GROUP, NXDN_VOICE_4800_EHR, 1001, 77, NXDN_CIPHER_AES, 9 };
	static uint8_t syms[NXDN_PREAMBLE_SYMS / 4 + 12 * NXDN_FRAME_BYTES + CHUNK_MAX / 4];
	uint16_t at = 0;
	nxdn_call_tx_t t;
	bool ended = false;

	memset(syms, 0x5F, sizeof syms);
	nxdn_call_tx_begin(&t, &v, 5, NXDN_DIR_INBOUND, false);
	while (!nxdn_call_tx_done(&t)) {
		uint16_t first, n = nxdn_call_tx_ready(&t, &first);

		if (!n) {
			uint8_t ehr[NXDN_EHR_BYTES];

			if (at > 8 * NXDN_FRAME_SYMS && !ended) {
				ended = true;
				nxdn_call_tx_end(&t, silence);
				continue;
			}
			for (uint8_t k = 0; k < NXDN_EHR_BYTES; k++)
				ehr[k] = (uint8_t)rng();
			nxdn_call_tx_voice(&t, ehr);
			continue;
		}
		air_syms_copy(syms, at, nxdn_call_tx_syms(&t), first, n);
		// The header's second FACCH1 carries the VCALL_IV (TS 1-B 4.1.5)
		if (t.state == 1 && first == 0) {
			uint8_t iv[NXDN_FACCH1_BYTES] = { NXDN_MSG_VCALL_IV, 1, 2, 3, 4, 5, 6, 7, 8 };
			uint8_t frame[NXDN_FRAME_BYTES];

			memcpy(frame, nxdn_call_tx_syms(&t), sizeof frame);
			nxdn_facch1_put(frame, 1, iv);
			air_syms_copy(syms, at, frame, 0, NXDN_FRAME_SYMS);
		}
		at = (uint16_t)(at + n);
		nxdn_call_tx_sent(&t, n);
	}
	for (uint16_t k = 0; k + 72 <= at + 144; k = (uint16_t)(k + 72)) {
		uint8_t c[18];

		air_syms_copy(c, 0, syms, k, 72);
		prov[1].now_us += 30000;
		deliver(&prov[1], c, 72);
	}
	steps(20);
}

static void testSquelch(void)
{
	nxdn_engine_cfg_t cfg = base_cfg(false);
	nxdn_engine_view_t v;

	// A group squelch on another group: the call is seen, not played
	begin(0, &cfg, &hooks);
	cfg.unit_id = 2002;
	cfg.rx_gid = 78;
	begin(1, &cfg, &hooks);
	CHECK(nxdn_engine_tx_prepare(&r[0].e), "prime");
	prov[0].keyed = true;
	CHECK(nxdn_engine_tx_go(&r[0].e), "go");
	for (uint8_t k = 0; k < 20; k++) {
		encode(&r[0], (uint8_t)(1 + (k & 1)));
		steps(1);
	}
	nxdn_engine_view(&r[1].e, &v);
	CHECK(v.rx_active && !v.rx_played && v.rx.vcall.dst == 77 && r[1].starts == 0 && r[1].nvoice == 0,
	      "another group: active %d played %d heard %u", v.rx_active, v.rx_played, r[1].nvoice);
	// The operator switches to that group mid-call: the next voice opens the squelch
	cfg.rx_gid = 77;
	nxdn_engine_set(&r[1].e, &cfg);
	for (uint8_t k = 0; k < 20; k++) {
		encode(&r[0], (uint8_t)(1 + (k & 1)));
		steps(1);
	}
	CHECK(r[1].starts == 1 && r[1].nvoice >= 8, "our group now: starts %u heard %u", r[1].starts, r[1].nvoice);
	// Abort: off the air at once, the far end sees the signal go
	nxdn_engine_tx_abort(&r[0].e);
	CHECK(nxdn_engine_tx_state(&r[0].e) == AIR_TX_DONE && !prov[0].on, "aborted");
	prov[0].keyed = false;
	nxdn_engine_tx_clear(&r[0].e);
	steps(30);
	nxdn_engine_view(&r[1].e, &v);
	CHECK(r[1].ends == 1 && !v.rx_active, "the far end's call ended after the abort: %u", r[1].ends);

	// An individual call to us passes the group squelch; one to another unit does not; a call to all does
	r[1].starts = r[1].ends = 0;
	cfg.unit_id = 1001;
	cfg.rx_gid = 0;
	cfg.tx_call_type = NXDN_CALL_INDIVIDUAL;
	cfg.tx_dst = 2002;
	nxdn_engine_set(&r[0].e, &cfg);
	r[1].e.cfg.rx_gid = 77;
	CHECK(over(20, 0, 0) > 20 && r[1].starts == 1 && r[1].call.vcall.call_type == NXDN_CALL_INDIVIDUAL &&
	      r[1].call.vcall.dst == 2002 && r[1].call.vcall.src == 1001, "individual call to us: starts %u", r[1].starts);
	r[1].starts = 0;
	cfg.tx_dst = 3003;
	nxdn_engine_set(&r[0].e, &cfg);
	over(20, 0, 0);
	CHECK(r[1].starts == 0, "individual call to another unit: starts %u", r[1].starts);
	cfg.tx_call_type = NXDN_CALL_GROUP;
	cfg.tx_dst = NXDN_ID_ALL;
	nxdn_engine_set(&r[0].e, &cfg);
	over(20, 0, 0);
	CHECK(r[1].starts == 1 && r[1].call.vcall.dst == NXDN_ID_ALL, "all call: starts %u", r[1].starts);
	r[1].starts = 0;
	// Another RAN: not even seen; RAN 0 on the receiver takes it
	cfg.tx_ran = 6;
	nxdn_engine_set(&r[0].e, &cfg);
	over(20, 0, 0);
	nxdn_engine_view(&r[1].e, &v);
	CHECK(r[1].starts == 0 && v.rx_calls == 4 && r[1].e.rx.rx.ran_rejects > 0, "another RAN: calls seen %u", v.rx_calls);
	r[1].e.cfg.rx_ran = NXDN_RAN_ANY;
	nxdn_engine_set(&r[1].e, &r[1].e.cfg);
	over(20, 0, 0);
	CHECK(r[1].starts == 1, "RAN 0 hears RAN 6: starts %u", r[1].starts);
	// Encrypted: seen, with its cipher, never played
	r[1].starts = 0;
	r[1].nvoice = 0;
	nxdn_engine_view(&r[1].e, &v);
	send_encrypted();
	{
		nxdn_engine_view_t w;

		nxdn_engine_view(&r[1].e, &w);
		CHECK(w.rx_calls == v.rx_calls + 1 && w.rx.vcall.cipher == NXDN_CIPHER_AES && w.rx.iv && r[1].starts == 0 &&
		      r[1].nvoice == 0 && r[1].e.rx.stats.voice_held >= 28, "encrypted: calls %u cipher %u iv %u voice %u held %u",
		      w.rx_calls, w.rx.vcall.cipher, w.rx.iv, r[1].nvoice, r[1].e.rx.stats.voice_held);
	}
	// A closed engine refuses
	nxdn_engine_end(&r[0].e);
	CHECK(!nxdn_engine_tx_prepare(&r[0].e), "closed: no transmit");
	nxdn_engine_end(&r[1].e);
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
	bool late;
} sm;
static uint8_t sm_sent[64][NXDN_EHR_BYTES];

static void sm_start(const nxdn_call_t *c, void *ctx) { (void)ctx; sm.starts++; sm.late = c->late; }
static void sm_update(const nxdn_call_t *c, void *ctx) { (void)c; (void)ctx; }
static void sm_end(void *ctx) { (void)ctx; sm.ends++; }
static void sm_voice(const uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	(void)ctx;
	// Which frame: the voice of a call joined late starts further on; compare with the best matching sent frame
	uint32_t best = 72;

	for (uint8_t k = 0; k < 64; k++) {
		uint32_t d = 0;

		for (uint8_t b = 0; b < NXDN_EHR_BYTES; b++)
			d += (uint32_t)__builtin_popcount(ehr[b] ^ sm_sent[k][b]);
		if (d < best)
			best = d;
	}
	sm.voice++;
	sm.voice_bits_bad += best;
}
static const nxdn_call_sink_t sm_sink = { sm_start, sm_update, sm_voice, sm_end };

static void out_chunk(const air_chunk_t *c, void *ctx)
{
	air_framer_rx(ctx, c);
}

static void testSoftModem(void)
{
	static const double sigmas[] = { 0, 100, 200, 300, 400, 500, 600, 700, 800 };
	static uint8_t fbuf[NXDN_FRAME_BYTES];
	static int8_t fsoft[NXDN_FRAME_SYMS];
	static nxdn_call_rx_t c;
	const air_profile_t *p = &nxdn_air_mode.profile;
	nxdn_vcall_t v = { NXDN_MSG_VCALL, 0, NXDN_CALL_GROUP, NXDN_VOICE_4800_EHR, 1001, 77, NXDN_CIPHER_NONE, 0 };
	nxdn_call_tx_t t;
	uint16_t nv = 0;
	bool ended = false;
	int last_whole = -1, last_found = -1;

	// The call: 1 s of noise symbols ahead (the tap carries noise there, below), preamble, 13 frames, release
	air_n = 0;
	for (uint8_t i = 0; i < 64; i++)
		for (uint8_t b = 0; b < NXDN_EHR_BYTES; b++)
			sm_sent[i][b] = (uint8_t)rng();
	for (uint16_t i = 0; i < 100; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	nxdn_call_tx_begin(&t, &v, 5, NXDN_DIR_INBOUND, false);
	while (!nxdn_call_tx_done(&t)) {
		uint16_t first, n = nxdn_call_tx_ready(&t, &first);

		if (nv == 48 && !ended) {
			ended = true;
			nxdn_call_tx_end(&t, sm_sent[63]);
			continue;
		}
		if (!n) {
			nxdn_call_tx_voice(&t, sm_sent[nv++]);
			continue;
		}
		for (uint16_t i = 0; i < n; i++)
			air[air_n++] = air_sym_get(nxdn_call_tx_syms(&t), (uint16_t)(first + i));
		nxdn_call_tx_sent(&t, n);
	}
	for (uint16_t i = 0; i < 100; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	modulate(p);
	printf("  soft modem, 2400 symbols/s, 8 kHz tap, 1050 Hz outer deviation = 2100 counts:\n");
	// sigma in counts at 2000 counts/kHz: a symbol level step is 700 Hz = 1400 counts. Eight calls at each level
	for (uint8_t si = 0; si < sizeof(sigmas) / sizeof(sigmas[0]); si++) {
		uint32_t whole = 0, found = 0, late = 0, voice = 0, bad = 0, lich = 0, sacch = 0, facch = 0, fec = 0;

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
			nxdn_call_rx_init(&c, 5, &sm_sink, NULL);
			air_framer_init(&fr, &nxdn_air_mode, &c.rx, fbuf, fsoft, NXDN_FRAME_SYMS);
			CHECK(air_softmodem_rx_open(&m, p, 8000, 2000, out_chunk, &fr), "open");
			for (uint32_t from = 0; from < n; from += 256)
				air_softmodem_rx(&m, all + from, (uint16_t)(n - from < 256 ? n - from : 256), 0x10000000u + from * 125u);
			whole += sm.starts == 1 && !sm.late && sm.voice >= 48 && sm.ends == 1;
			found += sm.starts >= 1;
			late += sm.starts >= 1 && sm.late;
			voice += sm.voice;
			bad += sm.voice_bits_bad;
			lich += c.stats.lich_fail;
			sacch += c.stats.sacch_fail;
			facch += c.stats.facch_fail;
			fec += c.stats.fec_bits;
			if (si < 2)	// (the VCH bits are raw channel bits: their own FEC is the vocoder's)
				CHECK(sm.starts == 1 && !sm.late && sm.ends == 1 && sm.voice >= 48 && sm.voice_bits_bad == 0,
				      "noise %.0f: starts %u voice %u bad bits %u", sigmas[si], sm.starts, sm.voice, sm.voice_bits_bad);
		}
		printf("    noise %3.0f counts: calls whole %u/%u, found %u (%u late), voice %3u%% of frames, VCH bit errors %5.2f %%, "
		       "fails LICH %u SACCH %u FACCH1 %u, %u bits corrected\n", sigmas[si], whole, TRIALS, found, late,
		       voice * 100 / (48 * TRIALS), voice ? 100.0 * bad / (72.0 * voice) : 0.0, lich, sacch, facch, fec);
		if (whole == TRIALS && last_whole == si - 1)
			last_whole = si;
		if (found == TRIALS && last_found == si - 1)
			last_found = si;
	}
	printf("    break points: every call whole up to %.0f counts, every call found up to %.0f counts\n",
	       last_whole >= 0 ? sigmas[last_whole] : -1.0, last_found >= 0 ? sigmas[last_found] : -1.0);
}

int main(void)
{
	testCall(false);
	testCall(true);
	testSquelch();
	testSoftModem();
	printf("nxdnEngineTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
