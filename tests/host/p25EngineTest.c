// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The P25 engine (core/p25/p25_engine.c) and the air interface session under it (core/air/air_session.c): two
// engines on two made-up providers whose transmit queues feed each other's receivers a chunk at a time, as a modem
// chip's records do. A call from one is heard by the other with its fields and its voice in order; an encoder that
// stalls is covered with silence and never underruns the air; the selective squelch; an abort.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p25/p25_engine.h"
#include "air/air_syms.h"

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

// ── Two providers, each other's far end ──
#define CHUNK	144
#define DEPTH	4
typedef struct {
	air_event_cb_t cb;
	void *ctx;
	bool open, on, begun_ev;
	air_tx_chunk_t q[DEPTH];
	uint8_t qn;
	uint32_t underruns, sent_syms, now_us;
	bool keyed;		// the test's radio: the transmitter is switched on
	uint32_t keyed_violations;
} prov_t;
static prov_t prov[2];

static void ev(prov_t *p, uint8_t kind, const air_chunk_t *c)
{
	air_event_t e = { .kind = kind, .t_us = p->now_us, .chunk = c };

	if (p->cb)
		p->cb(&e, p->ctx);
}

// n dibits of syms arrive at provider `to`
static void deliver(prov_t *to, const uint8_t *syms, uint16_t n)
{
	int8_t soft[CHUNK];
	air_chunk_t c = { .t_us = to->now_us, .nsyms = n, .sync_at = -1, .hard = syms, .soft = soft };

	for (uint16_t i = 0; i < n; i++)
		soft[i] = (int8_t)(32 * air_sym_level(air_sym_get(syms, i)));
	if (to->open && !to->on)
		ev(to, AIR_EV_RX, &c);
}

// One chunk period (30 ms) at provider i
static void step(uint8_t i)
{
	prov_t *p = &prov[i], *far = &prov[i ^ 1];

	p->now_us += 30000;
	if (!p->on) {
		if (!far->on) {		// nobody sends: the receiver hands on noise
			uint8_t noise[CHUNK / 4];

			for (uint8_t k = 0; k < sizeof(noise); k++)
				noise[k] = (uint8_t)rng();
			deliver(p, noise, CHUNK);
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
		uint8_t idle[CHUNK / 4];

		memset(idle, 0x5F, sizeof(idle));
		p->underruns++;
		deliver(far, idle, CHUNK);
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

static uint32_t p_fit(const air_profile_t *pr) { return pr->sym_rate == 4800 ? AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT : 0; }
static bool p_open(uint8_t i, const air_profile_t *pr, air_event_cb_t cb, void *ctx)
{
	memset(&prov[i], 0, sizeof(prov[i]));
	prov[i].cb = cb;
	prov[i].ctx = ctx;
	prov[i].open = true;
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
	static const hal_air_t air##i = { p_fit, open##i, close##i, restart##i, begin##i, queue##i, space##i, end##i, abort##i, \
					  now##i, NULL, NULL, NULL, CHUNK, DEPTH, 60, 100 };
PROVIDER(0)
PROVIDER(1)

// ── The two radios ──
static const uint8_t silence[P25_IMBE_BYTES] = { 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A, 0xA5 };
typedef struct {
	p25_engine_t e;
	// What it heard
	uint8_t starts, updates, ends;
	p25_call_t call;
	uint8_t voice[200][P25_IMBE_BYTES];
	uint16_t nvoice, voice_errs;	// frames; of them, with bits the codes corrected
	// Its encoder: frames made, frames the engine took
	uint8_t enc[200][P25_IMBE_BYTES];
	uint16_t enc_made, enc_taken;
} radio_t;
static radio_t r[2];

static void h_start(const p25_call_t *c, void *ctx) { radio_t *x = ctx; x->starts++; x->call = *c; }
static void h_update(const p25_call_t *c, void *ctx) { radio_t *x = ctx; x->updates++; x->call = *c; }
static void h_end(void *ctx) { radio_t *x = ctx; x->ends++; }
static void h_voice(const uint8_t imbe[P25_IMBE_BYTES], uint8_t errs, void *ctx)
{
	radio_t *x = ctx;

	if (errs)
		x->voice_errs++;
	if (x->nvoice < 200)
		memcpy(x->voice[x->nvoice++], imbe, P25_IMBE_BYTES);
}
static bool h_tx_voice(uint8_t imbe[P25_IMBE_BYTES], void *ctx)
{
	radio_t *x = ctx;

	if (x->enc_taken == x->enc_made)
		return false;
	memcpy(imbe, x->enc[x->enc_taken++], P25_IMBE_BYTES);
	return true;
}
static const p25_engine_hooks_t hooks = { h_start, h_update, h_voice, h_end, h_tx_voice };
static const p25_engine_hooks_t hooks_no_enc = { h_start, h_update, h_voice, h_end, NULL };

static void encode(radio_t *x, uint8_t n)
{
	while (n-- && x->enc_made < 200) {
		for (uint8_t k = 0; k < P25_IMBE_BYTES; k++)
			x->enc[x->enc_made][k] = (uint8_t)rng();
		x->enc_made++;
	}
}

static void begin(uint8_t i, const p25_engine_cfg_t *cfg, const p25_engine_hooks_t *h)
{
	memset(&r[i], 0, sizeof(r[i]));
	CHECK(p25_engine_begin(&r[i].e, i ? &air1 : &air0, cfg, h, &r[i]), "radio %u begins", i);
}

// Radio 0 transmits: `steps` chunk periods with the key down (the encoder makes 3 frames per 2 periods, none
// during the periods stall_from..stall_to), then lets go and runs until the air is quiet. Returns the periods it took
static uint16_t over(uint16_t steps, uint16_t stall_from, uint16_t stall_to)
{
	uint16_t n = 0;

	CHECK(p25_engine_tx_prepare(&r[0].e), "the header is queued");
	CHECK(p25_engine_tx_state(&r[0].e) == AIR_TX_PRIMING && !prov[0].on, "primed, not sending");
	prov[0].keyed = true;		// hal_radio_t.tx()
	CHECK(p25_engine_tx_go(&r[0].e), "go");
	for (; n < 400; n++) {
		if (n == steps)
			p25_engine_tx_stop(&r[0].e);
		if (n < steps && !(n >= stall_from && n < stall_to))
			encode(&r[0], (uint8_t)(1 + (n & 1)));
		step(0);
		step(1);
		if (p25_engine_tx_state(&r[0].e) == AIR_TX_DONE) {
			prov[0].keyed = false;	// hal_radio_t.tx_off()
			p25_engine_tx_clear(&r[0].e);
			break;
		}
	}
	for (uint8_t k = 0; k < 20; k++) {	// the far end hears the channel go quiet
		step(0);
		step(1);
	}
	return n;
}

static void testCall(void)
{
	p25_engine_cfg_t cfg = { .rx_nac = 0x293, .tx_nac = 0x293, .unit_id = 1001, .silence = silence,
				 .tx_lc = { .lcf = P25_LCF_GROUP, .tgid = 77 } };
	p25_engine_view_t v;
	uint16_t n, bad = 0;

	CHECK(p25_engine_fit(&air0) == (AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT) && p25_engine_fit(NULL) == 0, "fit");
	begin(0, &cfg, &hooks);
	cfg.unit_id = 2002;
	begin(1, &cfg, &hooks);
	for (uint8_t k = 0; k < 10; k++) {	// noise both ways: nothing is heard
		step(0);
		step(1);
	}
	CHECK(r[0].starts == 0 && r[1].starts == 0, "noise starts no call");

	n = over(100, 0, 0);	// 3 s
	CHECK(n > 100 && n < 112, "the over ended %u periods after 100", n);
	CHECK(prov[0].underruns == 0 && prov[0].keyed_violations == 0 && r[0].e.session.stats.tx_underruns == 0, "underruns %u, keyed outside permission %u", prov[0].underruns, prov[0].keyed_violations);
	p25_engine_view(&r[0].e, &v);
	CHECK(v.tx == AIR_TX_IDLE && v.tx_calls == 1 && v.tx_filler <= 2, "sender's view: tx %u calls %u filler %u", v.tx, v.tx_calls, v.tx_filler);
	CHECK(r[1].starts == 1 && r[1].ends == 1 && r[1].updates >= 1, "heard: starts %u ends %u updates %u", r[1].starts, r[1].ends, r[1].updates);
	CHECK(r[1].call.have_lc && r[1].call.lc.tgid == 77 && r[1].call.lc.src == 1001 && r[1].call.nac == 0x293 && !r[1].call.late, "heard fields: tg %u src %u", r[1].call.lc.tgid, r[1].call.lc.src);
	// Every encoded frame arrived, in order; the tail is silence up to the LDU's end, and whole LDUs came
	CHECK(r[0].enc_taken == r[0].enc_made && r[1].nvoice % 9 == 0 && r[1].nvoice >= r[0].enc_made && r[1].nvoice < r[0].enc_made + 9 + v.tx_filler, "frames: made %u taken %u heard %u", r[0].enc_made, r[0].enc_taken, r[1].nvoice);
	for (uint16_t i = 0, k = 0; i < r[1].nvoice; i++) {
		if (!memcmp(r[1].voice[i], silence, P25_IMBE_BYTES))
			continue;	// filler
		if (k >= r[0].enc_made || memcmp(r[1].voice[i], r[0].enc[k++], P25_IMBE_BYTES))
			bad++;
	}
	CHECK(bad == 0 && r[1].voice_errs == 0, "%u frames out of order or wrong, %u with errors", bad, r[1].voice_errs);
	// The air ran one chunk period behind the encoder plus the lead, not an LDU behind: the first frame was
	// taken within three periods of the start
	p25_engine_view(&r[1].e, &v);
	CHECK(!v.rx_active && v.rx_calls == 1 && v.rx.lc.src == 1001, "receiver's view after the call");
	CHECK(r[0].starts == 0, "the sender did not hear itself");

	// The encoder stalls for 300 ms mid-over: silence fills in, the air never runs dry
	r[1].starts = r[1].ends = 0;
	r[1].nvoice = 0;
	r[0].enc_made = r[0].enc_taken = 0;
	over(60, 20, 30);
	p25_engine_view(&r[0].e, &v);
	CHECK(prov[0].underruns == 0 && v.tx_filler >= 10 && v.tx_filler <= 20, "stall: underruns %u filler %u", prov[0].underruns, v.tx_filler);
	CHECK(r[1].starts == 1 && r[1].ends == 1 && r[0].enc_taken == r[0].enc_made, "stall: starts %u ends %u", r[1].starts, r[1].ends);

	// The other way, with no encoder at all: a call of silence frames
	p25_engine_end(&r[1].e);
	cfg.unit_id = 2002;
	begin(1, &cfg, &hooks_no_enc);
	r[0].starts = r[0].ends = 0;
	r[0].nvoice = 0;
	CHECK(p25_engine_tx_prepare(&r[1].e), "radio 1 primes");
	prov[1].keyed = true;
	CHECK(p25_engine_tx_go(&r[1].e), "radio 1 goes");
	for (n = 0; n < 200; n++) {
		if (n == 40)
			p25_engine_tx_stop(&r[1].e);
		step(1);
		step(0);
		if (p25_engine_tx_state(&r[1].e) == AIR_TX_DONE)
			break;
	}
	prov[1].keyed = false;
	p25_engine_tx_clear(&r[1].e);
	for (uint8_t k = 0; k < 20; k++) {
		step(1);
		step(0);
	}
	CHECK(n > 40 && n < 52 && prov[1].underruns == 0, "no encoder: ended at %u, underruns %u", n, prov[1].underruns);
	CHECK(r[0].starts == 1 && r[0].ends == 1 && r[0].call.lc.src == 2002 && r[0].nvoice >= 54 && !memcmp(r[0].voice[5], silence, P25_IMBE_BYTES), "no encoder: heard %u frames", r[0].nvoice);
	p25_engine_end(&r[0].e);
	p25_engine_end(&r[1].e);
}

static void testSquelch(void)
{
	p25_engine_cfg_t cfg = { .rx_nac = 0x293, .tx_nac = 0x293, .unit_id = 1001, .silence = silence,
				 .tx_lc = { .lcf = P25_LCF_GROUP, .tgid = 77 } };
	p25_engine_view_t v;

	// Selective squelch on another talk group: the call is seen, not played
	begin(0, &cfg, &hooks);
	cfg.unit_id = 2002;
	cfg.rx_tgid = 78;
	begin(1, &cfg, &hooks);
	CHECK(p25_engine_tx_prepare(&r[0].e), "prime");
	prov[0].keyed = true;
	CHECK(p25_engine_tx_go(&r[0].e), "go");
	for (uint8_t k = 0; k < 20; k++) {
		encode(&r[0], (uint8_t)(1 + (k & 1)));
		step(0);
		step(1);
	}
	p25_engine_view(&r[1].e, &v);
	CHECK(v.rx_active && !v.rx_played && v.rx.lc.tgid == 77 && r[1].starts == 0 && r[1].nvoice == 0, "another group: active %d played %d heard %u", v.rx_active, v.rx_played, r[1].nvoice);
	// The operator switches to that group mid-call: the next LDU1 opens the squelch
	cfg.rx_tgid = 77;
	p25_engine_set(&r[1].e, &cfg);
	for (uint8_t k = 0; k < 20; k++) {
		encode(&r[0], (uint8_t)(1 + (k & 1)));
		step(0);
		step(1);
	}
	CHECK(r[1].starts == 1 && r[1].nvoice >= 9, "our group now: starts %u heard %u", r[1].starts, r[1].nvoice);
	// Abort: off the air at once, the far end sees the signal go
	p25_engine_tx_abort(&r[0].e);
	CHECK(p25_engine_tx_state(&r[0].e) == AIR_TX_DONE && !prov[0].on, "aborted");
	prov[0].keyed = false;
	p25_engine_tx_clear(&r[0].e);
	for (uint8_t k = 0; k < 30; k++) {
		step(0);
		step(1);
	}
	// (the LDU that was cut is handed on with noise for its rest, and the receiver coasts one or two more on the
	// call's order before it gives up: those frames come with errors, for the decoder to hide)
	CHECK(r[1].ends == 1 && r[1].voice_errs > 0 && r[1].voice_errs <= 27, "the far end's call ended: %u, frames with errors %u", r[1].ends, r[1].voice_errs);

	// A unit to unit call to us passes the selective squelch; one to another unit does not
	r[1].starts = r[1].ends = 0;
	cfg.unit_id = 1001;
	cfg.rx_tgid = 0;
	cfg.tx_lc = (p25_lc_t){ .lcf = P25_LCF_UNIT, .dst = 2002 };
	p25_engine_set(&r[0].e, &cfg);
	CHECK(over(20, 0, 0) > 20 && r[1].starts == 1 && r[1].call.lc.lcf == P25_LCF_UNIT && r[1].call.lc.dst == 2002 && r[1].call.lc.src == 1001, "unit call to us: starts %u", r[1].starts);
	r[1].starts = 0;
	cfg.tx_lc.dst = 3003;
	p25_engine_set(&r[0].e, &cfg);
	over(20, 0, 0);
	CHECK(r[1].starts == 0, "unit call to another unit: starts %u", r[1].starts);
	// A group call to everyone passes it too
	cfg.tx_lc = (p25_lc_t){ .lcf = P25_LCF_GROUP, .tgid = P25_TGID_EVERYONE };
	p25_engine_set(&r[0].e, &cfg);
	over(20, 0, 0);
	CHECK(r[1].starts == 1 && r[1].call.lc.tgid == P25_TGID_EVERYONE, "everyone: starts %u", r[1].starts);
	r[1].starts = 0;
	// Another system's NAC: not even seen
	cfg.tx_nac = 0x555;
	p25_engine_set(&r[0].e, &cfg);
	over(20, 0, 0);
	p25_engine_view(&r[1].e, &v);
	CHECK(r[1].starts == 0 && v.rx_calls == 4, "another NAC: calls seen %u", v.rx_calls);
	// A closed engine refuses
	p25_engine_end(&r[0].e);
	CHECK(!p25_engine_tx_prepare(&r[0].e), "closed: no transmit");
	p25_engine_end(&r[1].e);
}

int main(void)
{
	testCall();
	testSquelch();
	printf("p25EngineTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
