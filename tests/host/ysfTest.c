// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// YSF (core/ysf): the frame's channels against reference frames from another implementation of the
// format, the FEC under bit errors and soft values, the transmit builder through the framer into the call layer, and
// two engines on two made-up providers whose transmit queues feed each other's receivers: a call heard with its
// callsigns and voice in order, late entry, the DG-ID squelch, an encoder that stalls, the end of an over.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ysf/ysf_engine.h"
#include "ysf/ysf_id.h"
#include "dmr/ambe/ambeFec.h"
#include "air/air_syms.h"
#include "fec/crc16.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0x5EED1234;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void hex(const char *s, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned v;

		sscanf(s + 2 * i, "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

// ── Reference frames ──
// A communications frame's FICH (FI 1, CS 2, FN 3, FT 6, DT 2) and one with every field away from zero
static const char *fich_ref[2] = {
	"102D40AD99FEB2F3933D842011ECD20443F16F33CB1F81CD2C",
	"31D082B423FFB6082E3A839FA3C8203A57B57D393070B0EFF7",
};
static const ysf_fich_t fich_in[2] = {
	{ .fi = 1, .cs = 2, .fn = 3, .ft = 6, .dt = 2 },
	{ .fi = 1, .cs = 2, .cm = 3, .bn = 1, .bt = 2, .fn = 7, .ft = 7, .dev = true, .mr = 2, .voip = true, .dt = 3,
	  .sql = true, .dgid = 99 },
};
// A header's payload: CSD1 "ALL       SP9ABC    ", CSD2 all spaces
static const char *head_ref =
	"F7235C101FA07F3EC0DF2363C017607F1C88B7185A82969A1C149DB9A85ABA46921014BEA9A00B7FBE451C8EC675A8F77FB81110F2"
	"C613730070250C4992B83DEF0E84251E4994641373063ABBD58AD18B1373F60DFBD5890193";
// A DN payload: DCH "SP9ABC    " and five voice frames' 49 bits
static const char *dn_ref =
	"F73C376E944CCD265663B16BB0B50C6EA4778A23B606B7A2239CF9B7B3D003C0C5F66A890F8C7574875998374758097B93C1D5F72F"
	"55FFB917E0493FDC356114C53EC7D04C541FC95BD101D93B0CC5FB8A9C08E21B49592279BF";
static const char *dn_bits =
	"1011010101011010010010011010110001111001101111001010111101000100010111111111001011111011001100100010100110"
	"0001100001000011110100111001000000000001110110100110001000001001101010011100101101111011101100100011101001"
	"110000111010001010001100000011111";

static void testReference(void)
{
	static const uint8_t pn[20] = { 0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F,
					0xF8, 0x3D, 0xF1, 0x73, 0x20, 0x94, 0xED, 0x1E, 0x7C, 0xD8 };
	uint8_t frame[YSF_FRAME_BYTES], ref[90], c1[YSF_CSD_BYTES], c2[YSF_CSD_BYTES], out[YSF_CSD_BYTES];
	uint8_t bits[YSF_VCH_PER_FRAME][AMBE_PARAM_BITS];
	ysf_fec_work_t w;
	ysf_fich_t f;
	bool pn_ok = true;

	for (uint16_t i = 0; i < 160; i++)
		pn_ok &= ysf_pn(i) == ((pn[i / 8] >> (7 - i % 8)) & 1);
	CHECK(pn_ok, "the whitening sequence");
	CHECK(fec_crc_ccitt((const uint8_t *)"123456789", 9) == (uint16_t)~0x31C3, "the CRC-16 check value");

	for (uint8_t k = 0; k < 2; k++) {
		memset(frame, 0, sizeof frame);
		ysf_frame_begin(frame);
		ysf_fich_put(frame, &fich_in[k]);
		hex(fich_ref[k], ref, 25);
		CHECK(!memcmp(frame + 5, ref, 25) && ysf_sync_errs(frame) == 0, "FICH %u as the reference", k);
		CHECK(ysf_fich_get(frame, NULL, &w, &f) == 0 && !memcmp(&f, &fich_in[k], sizeof f), "FICH %u read back", k);
	}

	memset(c1, ' ', sizeof c1);
	memcpy(c1, "ALL", 3);
	memcpy(c1 + 10, "SP9ABC", 6);
	memset(c2, ' ', sizeof c2);
	ysf_csd_put(frame, c1, c2);
	hex(head_ref, ref, 90);
	CHECK(!memcmp(frame + 30, ref, 90), "header payload as the reference");
	CHECK(ysf_csd_get(frame, NULL, 0, &w, out) == 0 && !memcmp(out, c1, 20), "CSD1 read back");
	CHECK(ysf_csd_get(frame, NULL, 1, &w, out) == 0 && !memcmp(out, c2, 20), "CSD2 read back");

	for (uint8_t n = 0; n < YSF_VCH_PER_FRAME; n++)
		for (uint8_t i = 0; i < AMBE_PARAM_BITS; i++)
			bits[n][i] = (uint8_t)(dn_bits[n * AMBE_PARAM_BITS + i] - '0');
	ysf_dch_put(frame, (const uint8_t *)"SP9ABC    ");
	for (uint8_t n = 0; n < YSF_VCH_PER_FRAME; n++)
		ysf_vch_put(frame, n, bits[n]);
	hex(dn_ref, ref, 90);
	CHECK(!memcmp(frame + 30, ref, 90), "DN payload as the reference");
	CHECK(ysf_dch_get(frame, NULL, &w, out) == 0 && !memcmp(out, "SP9ABC    ", 10), "DCH read back");
	for (uint8_t n = 0; n < YSF_VCH_PER_FRAME; n++) {
		uint8_t b[AMBE_PARAM_BITS];

		CHECK(ysf_vch_get(frame, NULL, n, b) == 0 && !memcmp(b, bits[n], sizeof b), "VCH %u read back", n);
	}
}

// ── FEC: hard bit errors, then soft values with noise ──
static void flip(uint8_t *frame, uint16_t from, uint16_t to, uint8_t n)
{
	while (n--) {
		uint16_t b = (uint16_t)(from + rng() % (to - from));

		frame[b >> 3] ^= (uint8_t)(0x80 >> (b & 7));
	}
}

static void testFec(void)
{
	uint8_t frame[YSF_FRAME_BYTES], dch[YSF_DCH_BYTES], out[YSF_DCH_BYTES], bits[AMBE_PARAM_BITS], b[AMBE_PARAM_BITS];
	int8_t soft[YSF_FRAME_SYMS];
	ysf_fec_work_t w;
	ysf_fich_t f, g;
	unsigned fich_ok = 0, dch_ok = 0, vch_ok = 0, soft_fich = 0, hard_fich = 0;

	for (uint16_t it = 0; it < 300; it++) {
		f = (ysf_fich_t){ .fi = 1, .cs = 2, .fn = (uint8_t)(it % 7), .ft = 6, .dt = 2, .dgid = (uint8_t)(it % 100) };
		for (uint8_t i = 0; i < YSF_DCH_BYTES; i++)
			dch[i] = (uint8_t)rng();
		for (uint8_t i = 0; i < AMBE_PARAM_BITS; i++)
			bits[i] = (uint8_t)(rng() & 1);
		memset(frame, 0, sizeof frame);
		ysf_frame_begin(frame);
		ysf_fich_put(frame, &f);
		ysf_dch_put(frame, dch);
		for (uint8_t n = 0; n < YSF_VCH_PER_FRAME; n++)
			ysf_vch_put(frame, n, bits);
		// Six errors in the FICH's 200 bits, six in the frame's payload: the codes take them
		flip(frame, 40, 240, 6);
		flip(frame, 240, 960, 6);
		fich_ok += ysf_fich_get(frame, NULL, &w, &g) >= 0 && !memcmp(&f, &g, sizeof f);
		dch_ok += ysf_dch_get(frame, NULL, &w, out) >= 0 && !memcmp(out, dch, sizeof out);
		vch_ok += ysf_vch_get(frame, NULL, 0, b) >= 0 && !memcmp(b, bits, 27);

		// Soft values: the levels with noise of about half the spacing between them
		memset(frame, 0, sizeof frame);
		ysf_frame_begin(frame);
		ysf_fich_put(frame, &f);
		for (uint16_t s = 0; s < YSF_FRAME_SYMS; s++) {
			int v = 32 * air_sym_level(air_sym_get(frame, s)) + (int)(rng() % 49) - 24 + (int)(rng() % 49) - 24;
			uint8_t d = v >= 64 ? 1 : v >= 0 ? 0 : v > -64 ? 2 : 3;

			soft[s] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
			air_sym_put(frame, s, d);
		}
		soft_fich += ysf_fich_get(frame, soft, &w, &g) >= 0 && !memcmp(&f, &g, sizeof f);
		hard_fich += ysf_fich_get(frame, NULL, &w, &g) >= 0 && !memcmp(&f, &g, sizeof f);
	}
	CHECK(fich_ok == 300 && dch_ok == 300, "hard errors: FICH %u, DCH %u of 300", fich_ok, dch_ok);
	CHECK(vch_ok >= 290, "hard errors: VCH repeated bits %u of 300", vch_ok);
	CHECK(soft_fich >= hard_fich && soft_fich >= 290, "noise: FICH soft %u, hard %u of 300", soft_fich, hard_fich);
}

// ── The transmit builder through the framer into the call layer, symbol by symbol ──
#define VMAX	200
typedef struct {
	uint8_t starts, updates, ends;
	ysf_call_t call;
	uint8_t voice[VMAX][YSF_VOICE_BYTES];
	uint16_t nvoice;
} sink_t;

static void s_start(const ysf_call_t *c, void *ctx) { sink_t *x = ctx; x->starts++; x->call = *c; }
static void s_update(const ysf_call_t *c, void *ctx) { sink_t *x = ctx; x->updates++; x->call = *c; }
static void s_end(void *ctx) { sink_t *x = ctx; x->ends++; }
static void s_voice(const uint8_t v[YSF_VOICE_BYTES], void *ctx)
{
	sink_t *x = ctx;

	if (x->nvoice < VMAX)
		memcpy(x->voice[x->nvoice++], v, YSF_VOICE_BYTES);
}
static ysf_wx_t wx_got;
static uint8_t wx_n;
static void s_wx(const ysf_wx_t *w, void *ctx) { (void)ctx; wx_got = *w; wx_n++; }
static const ysf_call_sink_t call_sink = { s_start, s_update, s_voice, s_end, s_wx };

static void voice_frame(uint8_t v[YSF_VOICE_BYTES])
{
	uint8_t bits[AMBE_PARAM_BITS];

	for (uint8_t i = 0; i < AMBE_PARAM_BITS; i++)
		bits[i] = (uint8_t)(rng() & 1);
	ambeFecEncode(bits, v);
}

static void testCallThroughFramer(void)
{
	static ysf_call_rx_t rx;
	static ysf_call_tx_t tx;
	static sink_t s;
	static air_framer_t fr;
	static uint8_t fbuf[YSF_FRAME_BYTES], made[40][YSF_VOICE_BYTES], silence[YSF_VOICE_BYTES];
	ysf_tx_id_t id = { .callsign = "n0call", .radio_id = "K7Q2Z", .dgid = 12 };
	uint16_t nmade = 0, total = 0, first;
	uint8_t chunk[36];

	memset(&s, 0, sizeof s);
	voice_frame(silence);
	ysf_call_rx_init(&rx, &call_sink, &s);
	air_framer_init(&fr, &ysf_air_mode, &rx.rx, fbuf, NULL, YSF_FRAME_SYMS);
	ysf_call_tx_begin(&tx, &id);
	while (!ysf_call_tx_done(&tx) && total < 20000) {
		uint16_t n = ysf_call_tx_ready(&tx, &first);
		air_chunk_t c = { .sync_at = -1 };

		if (!n) {
			if (nmade < 40) {
				voice_frame(made[nmade]);
				CHECK(ysf_call_tx_voice(&tx, made[nmade]), "voice taken");
				nmade++;
			} else {
				ysf_call_tx_end(&tx, silence);
			}
			continue;
		}
		if (n > 144)
			n = 144;
		memset(chunk, 0, sizeof chunk);
		air_syms_copy(chunk, 0, ysf_call_tx_syms(&tx), first, n);
		c.nsyms = n;
		c.hard = chunk;
		air_framer_rx(&fr, &c);
		ysf_call_tx_sent(&tx, n);
		total = (uint16_t)(total + n);
	}
	CHECK(ysf_call_tx_done(&tx), "the builder finished");
	// 100 ms of preamble, the header, eight communications frames (40 voice frames), the terminator
	CHECK(total == 11 * YSF_FRAME_SYMS, "symbols sent: %u", total);
	CHECK(s.starts == 1 && s.ends == 1 && !s.call.late, "one call, from its header: starts %u ends %u", s.starts, s.ends);
	CHECK(!strcmp(s.call.src, "N0CALL") && !strcmp(s.call.dst, "*****K7Q2Z") && !strcmp(s.call.src_id, "K7Q2Z") &&
	      s.call.dgid == 12 && s.call.dt == YSF_DT_VD2, "fields: '%s' '%s' '%s' DG-ID %u", s.call.src, s.call.dst,
	      s.call.src_id, s.call.dgid);
	CHECK(s.nvoice == 40, "voice frames heard: %u", s.nvoice);
	for (uint16_t i = 0; i < s.nvoice && i < nmade; i++)
		if (memcmp(s.voice[i], made[i], YSF_VOICE_BYTES)) {
			CHECK(0, "voice frame %u differs", i);
			break;
		}
	CHECK(rx.stats.fich_fail == 0 && rx.stats.dch_fail == 0 && rx.stats.csd_fail == 0, "no channel failed");

	// The DCH a DN transmission carries by FN
	uint8_t d[YSF_DCH_BYTES];

	ysf_dch_content(&id, 0, d);
	CHECK(!memcmp(d, "*****K7Q2Z", 10), "FN 0: destination");
	ysf_dch_content(&id, 1, d);
	CHECK(!memcmp(d, "N0CALL    ", 10), "FN 1: source");
	ysf_dch_content(&id, 5, d);
	CHECK(!memcmp(d, "     K7Q2Z", 10), "FN 5: remarks with the radio ID");
}

// ── Two engines on two providers, each other's far end. A chunk is 30 ms of symbols ──
#define CHUNK_MAX	144
#define DEPTH		4
typedef struct {
	air_event_cb_t cb;
	void *ctx;
	bool open, on, begun_ev, keyed;
	air_tx_chunk_t q[DEPTH];
	uint8_t qn;
	uint32_t underruns, now_us, keyed_violations;
	bool deaf;			// nothing reaches its receiver (late entry: switched on mid-call)
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
	if (to->open && !to->on && !to->deaf)
		ev(to, AIR_EV_RX, &c);
}

static void step(uint8_t i)
{
	prov_t *p = &prov[i], *far = &prov[i ^ 1];

	p->now_us += 30000;
	if (!p->on) {
		if (!far->on) {
			uint8_t noise[CHUNK_MAX / 4];

			for (uint8_t k = 0; k < sizeof noise; k++)
				noise[k] = (uint8_t)rng();
			deliver(p, noise, CHUNK_MAX);
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

		memset(idle, 0x77, sizeof idle);
		p->underruns++;
		deliver(far, idle, CHUNK_MAX);
		ev(p, AIR_EV_TX_UNDERRUN, NULL);
		return;
	}
	air_tx_chunk_t c = p->q[0];

	memmove(p->q, p->q + 1, (size_t)(--p->qn) * sizeof c);
	deliver(far, c.syms, c.nsyms);
	if (c.flags & AIR_TX_LAST) {
		p->on = false;
		ev(p, AIR_EV_TX_DONE, NULL);
	} else {
		ev(p, AIR_EV_TX_SPACE, NULL);
	}
}

static uint32_t p_fit(const air_profile_t *pr)
{
	return pr->sym_rate == 4800 && pr->dev_outer_hz == 2700 ? AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT : 0;
}
static bool p_open(uint8_t i, const air_profile_t *pr, air_event_cb_t cb, void *ctx)
{
	(void)pr;
	memset(&prov[i], 0, sizeof prov[i]);
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
	static hal_air_t air##i = { p_fit, open##i, close##i, restart##i, begin##i, queue##i, space##i, end##i, abort##i, \
				    now##i, NULL, NULL, NULL, NULL, CHUNK_MAX, DEPTH, 60, 100 };
PROVIDER(0)
PROVIDER(1)

typedef struct {
	ysf_engine_t e;
	sink_t s;
	uint8_t enc[VMAX][YSF_VOICE_BYTES];
	uint16_t enc_made, enc_taken;
} radio_t;
static radio_t r[2];
static uint8_t silence[YSF_VOICE_BYTES];

static void h_start(const ysf_call_t *c, void *ctx) { s_start(c, &((radio_t *)ctx)->s); }
static void h_update(const ysf_call_t *c, void *ctx) { s_update(c, &((radio_t *)ctx)->s); }
static void h_end(void *ctx) { s_end(&((radio_t *)ctx)->s); }
static void h_voice(const uint8_t v[YSF_VOICE_BYTES], void *ctx) { s_voice(v, &((radio_t *)ctx)->s); }
static bool h_tx_voice(uint8_t v[YSF_VOICE_BYTES], void *ctx)
{
	radio_t *x = ctx;

	if (x->enc_taken == x->enc_made)
		return false;
	memcpy(v, x->enc[x->enc_taken++], YSF_VOICE_BYTES);
	return true;
}
static const ysf_engine_hooks_t hooks = { h_start, h_update, h_voice, h_end, h_tx_voice, NULL };

static ysf_engine_cfg_t cfg_for(uint8_t i, uint8_t rx_dgid, uint8_t tx_dgid)
{
	ysf_engine_cfg_t c = { .rx_dgid = rx_dgid, .silence = silence };

	snprintf(c.tx.callsign, sizeof c.tx.callsign, i ? "N1ABC" : "N0CALL");
	snprintf(c.tx.radio_id, sizeof c.tx.radio_id, i ? "AB123" : "K7Q2Z");
	c.tx.dgid = tx_dgid;
	return c;
}

static void begin(uint8_t i, const ysf_engine_cfg_t *cfg)
{
	memset(&r[i], 0, sizeof r[i]);
	CHECK(ysf_engine_begin(&r[i].e, i ? &air1 : &air0, cfg, &hooks, &r[i]), "radio %u begins", i);
}

static void steps(uint16_t n)
{
	while (n--) {
		step(0);
		step(1);
	}
}

// Radio 0 transmits: n chunk periods with the key down (the encoder makes 3 frames per 2 periods, none in
// stall_from..stall_to), then lets go and runs until the air is quiet. Returns the periods it took
static uint16_t over(uint16_t n_key, uint16_t stall_from, uint16_t stall_to)
{
	uint16_t n = 0;

	CHECK(ysf_engine_tx_prepare(&r[0].e), "the preamble and header are queued");
	CHECK(ysf_engine_tx_state(&r[0].e) == AIR_TX_PRIMING && !prov[0].on, "primed, not sending");
	prov[0].keyed = true;
	CHECK(ysf_engine_tx_go(&r[0].e), "go");
	for (; n < 800; n++) {
		if (n == n_key)
			ysf_engine_tx_stop(&r[0].e);
		if (n < n_key && !(n >= stall_from && n < stall_to)) {
			for (uint8_t k = (uint8_t)(1 + (n & 1)); k && r[0].enc_made < VMAX; k--)
				voice_frame(r[0].enc[r[0].enc_made++]);
		}
		steps(1);
		if (ysf_engine_tx_state(&r[0].e) == AIR_TX_DONE) {
			prov[0].keyed = false;
			ysf_engine_tx_clear(&r[0].e);
			break;
		}
	}
	steps(20);
	return n;
}

// The frames heard against the frames made, in order from the first heard (silence between them is filler).
// Returns the number out of order or wrong
static uint16_t check_order(const radio_t *rx, const radio_t *tx, uint16_t *first)
{
	uint16_t bad = 0, k = 0, i = 0;

	while (i < rx->s.nvoice && !memcmp(rx->s.voice[i], silence, YSF_VOICE_BYTES))
		i++;
	while (k < tx->enc_made && i < rx->s.nvoice && memcmp(rx->s.voice[i], tx->enc[k], YSF_VOICE_BYTES))
		k++;
	*first = k;
	for (; i < rx->s.nvoice; i++) {
		if (!memcmp(rx->s.voice[i], silence, YSF_VOICE_BYTES))
			continue;
		if (k >= tx->enc_made || memcmp(rx->s.voice[i], tx->enc[k++], YSF_VOICE_BYTES))
			bad++;
	}
	return bad;
}

static void testEngines(void)
{
	ysf_engine_cfg_t c0 = cfg_for(0, 0, 0), c1 = cfg_for(1, 0, 0);
	ysf_engine_view_t v;
	uint16_t n, first;

	voice_frame(silence);
	CHECK(ysf_engine_fit(&air0) == (AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT) && ysf_engine_fit(NULL) == 0, "fit");

	// A call: callsigns, voice in order, no underrun, the over ends soon after the key comes up
	begin(0, &c0);
	begin(1, &c1);
	steps(30);
	CHECK(r[0].s.starts == 0 && r[1].s.starts == 0, "noise starts no call");
	n = over(100, 1000, 1000);
	CHECK(n > 100 && n < 115, "the over ended %u periods after 100", n);
	CHECK(prov[0].underruns == 0 && prov[0].keyed_violations == 0 && r[0].e.av.session.stats.tx_underruns == 0,
	      "no underrun: %u", prov[0].underruns);
	ysf_engine_view(&r[0].e, &v);
	CHECK(v.tx == AIR_TX_IDLE && v.tx_calls == 1 && v.tx_filler <= 10, "sender: tx %u calls %u filler %u", v.tx,
	      v.tx_calls, v.tx_filler);
	CHECK(r[1].s.starts == 1 && r[1].s.ends == 1 && !r[1].s.call.late && !strcmp(r[1].s.call.src, "N0CALL") &&
	      !strcmp(r[1].s.call.src_id, "K7Q2Z"), "receiver: starts %u ends %u src '%s'", r[1].s.starts, r[1].s.ends,
	      r[1].s.call.src);
	CHECK(check_order(&r[1], &r[0], &first) == 0 && first == 0 && r[1].s.nvoice % 5 == 0 && r[1].s.nvoice >= r[0].enc_made,
	      "voice in order: heard %u made %u first %u", r[1].s.nvoice, r[0].enc_made, first);
	ysf_engine_view(&r[1].e, &v);
	CHECK(!v.rx_active && v.rx_calls == 1 && !strcmp(v.rx.src, "N0CALL"), "receiver's view after the call");
	CHECK(r[0].s.starts == 0, "the sender did not hear itself");

	// The encoder stalls: silence covers the gap, the air never runs dry
	begin(0, &c0);
	begin(1, &c1);
	over(100, 30, 50);
	ysf_engine_view(&r[0].e, &v);
	CHECK(prov[0].underruns == 0 && v.tx_filler >= 10, "stall: underruns %u filler %u", prov[0].underruns, v.tx_filler);
	CHECK(r[1].s.starts == 1 && check_order(&r[1], &r[0], &first) == 0, "stall: voice still in order");

	// Late entry: the receiver comes on after the header; the fields come from the DCH
	begin(0, &c0);
	begin(1, &c1);
	prov[1].deaf = true;
	CHECK(ysf_engine_tx_prepare(&r[0].e), "prime");
	prov[0].keyed = true;
	CHECK(ysf_engine_tx_go(&r[0].e), "go");
	for (n = 0; n < 400; n++) {
		if (n == 20)
			prov[1].deaf = false;
		if (n == 120)
			ysf_engine_tx_stop(&r[0].e);
		if (n < 120)
			for (uint8_t k = (uint8_t)(1 + (n & 1)); k && r[0].enc_made < VMAX; k--)
				voice_frame(r[0].enc[r[0].enc_made++]);
		steps(1);
		if (ysf_engine_tx_state(&r[0].e) == AIR_TX_DONE) {
			prov[0].keyed = false;
			ysf_engine_tx_clear(&r[0].e);
			break;
		}
	}
	steps(20);
	CHECK(r[1].s.starts == 1 && r[1].s.call.late && r[1].s.ends == 1 && check_order(&r[1], &r[0], &first) == 0 &&
	      first > 0, "late entry: starts %u late %u first %u", r[1].s.starts, r[1].s.call.late, first);
	CHECK(!strcmp(r[1].s.call.src, "N0CALL") && !strcmp(r[1].s.call.src_id, "K7Q2Z"), "late entry: fields from the DCH");

	// DG-ID: another one is seen, not played; ours, and 0 at the receiver, are
	c0 = cfg_for(0, 0, 33);
	c1 = cfg_for(1, 44, 0);
	begin(0, &c0);
	begin(1, &c1);
	over(60, 1000, 1000);
	ysf_engine_view(&r[1].e, &v);
	CHECK(r[1].s.starts == 0 && r[1].s.nvoice == 0 && v.rx_calls == 1 && v.rx.dgid == 33, "DG-ID 33 to 44: seen, not played");
	c1.rx_dgid = 33;
	ysf_engine_set(&r[1].e, &c1);
	r[0].enc_made = r[0].enc_taken = 0;
	over(60, 1000, 1000);
	CHECK(r[1].s.starts == 1 && r[1].s.nvoice > 0, "DG-ID 33 to 33: played");

	// An abort: the air goes quiet without a terminator; the receiver gives up on the lost signal
	c0 = cfg_for(0, 0, 0);
	c1 = cfg_for(1, 0, 0);
	begin(0, &c0);
	begin(1, &c1);
	CHECK(ysf_engine_tx_prepare(&r[0].e), "prime");
	prov[0].keyed = true;
	CHECK(ysf_engine_tx_go(&r[0].e), "go");
	for (n = 0; n < 40; n++) {
		for (uint8_t k = (uint8_t)(1 + (n & 1)); k && r[0].enc_made < VMAX; k--)
			voice_frame(r[0].enc[r[0].enc_made++]);
		steps(1);
	}
	ysf_engine_tx_abort(&r[0].e);
	CHECK(ysf_engine_tx_state(&r[0].e) == AIR_TX_DONE, "abort: done at once");
	prov[0].keyed = false;
	ysf_engine_tx_clear(&r[0].e);
	steps(40);
	CHECK(r[1].s.starts == 1 && r[1].s.ends == 1, "abort: the call ended on the lost signal");

	// No callsign: PTT refused
	c0.tx_inhibit = true;
	begin(0, &c0);
	CHECK(!ysf_engine_tx_prepare(&r[0].e), "inhibited: refused");
	ysf_engine_end(&r[0].e);
	ysf_engine_end(&r[1].e);
}

// ── The radio ID: the same every start, different between radios, every value as likely ──
static int cmp_rid(const void *a, const void *b)
{
	return memcmp(a, b, YSF_RID_LEN);
}

static void testRid(void)
{
	static char ids[100000][YSF_RID_LEN + 1];
	uint32_t uid[3] = { 0x12345678, 0x9ABCDEF0, 0x0F1E2D3C }, near[3];
	char a[YSF_RID_LEN + 1], b[YSF_RID_LEN + 1];
	unsigned same = 0, bad_chars = 0, diff_bits = 0;

	ysf_rid_from_uid(uid, a);
	ysf_rid_from_uid(uid, b);
	CHECK(!strcmp(a, b) && strlen(a) == YSF_RID_LEN, "the same ID gives the same RID: %s", a);
	// One bit apart, anywhere in the 96: never the same RID
	for (uint8_t bit = 0; bit < 96; bit++) {
		memcpy(near, uid, sizeof near);
		near[bit / 32] ^= 1u << (bit % 32);
		ysf_rid_from_uid(near, b);
		same += !strcmp(a, b);
		for (uint8_t i = 0; i < YSF_RID_LEN; i++)
			diff_bits += a[i] != b[i];
	}
	CHECK(same == 0 && diff_bits > 96 * 4, "one bit apart: %u alike, %u of %u characters differ", same, diff_bits, 96 * 5);
	// 100000 radios: the pairs that share a RID are about n^2 / (2 x 36^5) = 83
	for (uint32_t i = 0; i < 100000; i++) {
		uint32_t u[3] = { rng(), rng(), rng() };

		ysf_rid_from_uid(u, ids[i]);
		for (uint8_t k = 0; k < YSF_RID_LEN; k++)
			bad_chars += !((ids[i][k] >= '0' && ids[i][k] <= '9') || (ids[i][k] >= 'A' && ids[i][k] <= 'Z'));
	}
	qsort(ids, 100000, sizeof ids[0], cmp_rid);
	same = 0;
	for (uint32_t i = 1; i < 100000; i++)
		same += !memcmp(ids[i - 1], ids[i], YSF_RID_LEN);
	CHECK(bad_chars == 0 && same > 40 && same < 140, "100000 radios: %u shared RIDs (83 expected)", same);
	uint32_t zero[3] = { 0, 0, 0 }, ones[3] = { ~0u, ~0u, ~0u };
	CHECK(!ysf_uid_usable(zero) && !ysf_uid_usable(ones) && ysf_uid_usable(uid), "unusable IDs");
	ysf_rid_fallback(2602100, "N0CALL", a);
	ysf_rid_fallback(2602101, "N0CALL", b);
	CHECK(strlen(a) == YSF_RID_LEN && strcmp(a, b), "fallback: %s %s", a, b);
}

// ── Rooms (ysf.md 9): commands as a node reads them, replies as a node sends them, through the framer ──
static void put_text(uint8_t *d, const char *s, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		d[i] = *s ? (uint8_t)*s++ : ' ';
}

static uint16_t reply_end(uint8_t *d, uint16_t n)
{
	uint8_t s = 0;

	d[n++] = 0x03;
	for (uint16_t i = 0; i < n; i++)
		s = (uint8_t)(s + d[i]);
	d[n++] = s;
	return n;
}

// A node's data transmission of d through the framer into a call layer: header, blocks of FN 0..FT, terminator
static void send_data(ysf_call_rx_t *rx, air_framer_t *fr, const uint8_t *d, uint16_t len, int lose_fn)
{
	static uint8_t frame[YSF_FRAME_BYTES];
	ysf_fich_t f = { .cs = 2, .cm = 1, .dt = YSF_DT_DATA };
	uint8_t c1[20], c2[20];
	uint16_t at = 0;
	(void)rx;

	for (int part = 0; part < 3; part++) {
		uint8_t blk = 0;

		do {
			uint8_t ft = 1;
			unsigned room = part == 1 ? (blk == 0 ? 260u : 259u) : 0;

			if (part == 1) {
				unsigned left = len > at ? len - at : 0, first = blk == 0 ? 20u : 19u;

				while (ft < 7 && first + (ft - 1u) * 40u < left)
					ft++;
				(void)room;
			}
			for (uint8_t fn = 0; fn <= (part == 1 ? ft : 0); fn++) {
				memset(frame, 0, sizeof frame);
				f.fi = part == 0 ? YSF_FI_HEADER : part == 1 ? YSF_FI_COMMS : YSF_FI_TERM;
				f.fn = fn;
				f.ft = part == 1 ? ft : 0;
				f.bn = blk & 3;
				f.bt = 0;
				ysf_frame_begin(frame);
				ysf_fich_put(frame, &f);
				memset(c1, ' ', 20);
				memset(c2, ' ', 20);
				if (part == 0)
					memcpy(c1, "ALL       N0RPT     ", 20);	// the header's CSD1: to all, from the node
				if (part == 1 && fn == 1) {
					uint8_t k = 0;

					if (blk)
						c2[k++] = 0x00;
					for (; k < 20 && at < len; k++)
						c2[k] = d[at++];
				} else if (part == 1 && fn >= 2) {
					for (uint8_t k = 0; k < 20 && at < len; k++)
						c1[k] = d[at++];
					for (uint8_t k = 0; k < 20 && at < len; k++)
						c2[k] = d[at++];
				}
				ysf_csd_put(frame, c1, c2);
				if (part == 1 && fn == lose_fn && blk == 0)
					memset(frame + 5, 0, 25);	// its FICH lost
				for (uint16_t s0 = 0; s0 < YSF_FRAME_SYMS; s0 += 120) {
					air_chunk_t c = { .sync_at = -1, .nsyms = 120 };
					uint8_t chunk[30];

					air_syms_copy(chunk, 0, frame, s0, 120);
					c.hard = chunk;
					air_framer_rx(fr, &c);
				}
			}
			blk++;
		} while (part == 1 && at < len);
	}
}

static void testRooms(void)
{
	static ysf_call_rx_t rx;
	static sink_t s;
	static air_framer_t fr;
	static uint8_t fbuf[YSF_FRAME_BYTES], d[1100];
	uint8_t cmd[YSF_WX_CMD_MAX];
	uint8_t n;
	uint16_t len;

	// Commands, byte for byte
	// As a commercial portable sends them (an off-air recording; its sequence byte counts, ours stays 0)
	n = ysf_wx_command(YSF_WX_DX, 0, NULL, cmd);
	CHECK(n == 17 && !memcmp(cmd, "\x00\x5D\x71\x5F\x27          \x03", 16) && cmd[16] == (uint8_t)(0x9B - 0x04), "DX");
	n = ysf_wx_command(YSF_WX_CONNECT, 86787, NULL, cmd);
	CHECK(n == 17 && !memcmp(cmd + 1, "\x5D\x23\x5F\x27" "86787     \x03", 15) && cmd[16] == (uint8_t)(0xC3 - 0x06),
	      "CONNECT 86787");
	n = ysf_wx_command(YSF_WX_DISCONNECT, 0, NULL, cmd);
	CHECK(n == 17 && !memcmp(cmd + 1, "\x5D\x2A\x5F\x27          \x03", 15) && cmd[16] == (uint8_t)(0x59 - 0x09),
	      "DISCONNECT");
	n = ysf_wx_command(YSF_WX_ALL, 21, NULL, cmd);
	CHECK(n == 12 && !memcmp(cmd + 4, "\x27" "01021\x03", 7), "ALL from 21");
	n = ysf_wx_command(YSF_WX_SEARCH, 0, "pol", cmd);
	CHECK(n == 28 && !memcmp(cmd + 1, "\x5D\x66\x5F\x27" "11   POL             \x03", 26) &&
	      cmd[27] == (uint8_t)(0xA0 - 0x07), "SEARCH");
	n = ysf_wx_command(YSF_WX_SEARCH, 21, "xx", cmd);
	CHECK(n == 28 && !memcmp(cmd + 5, "11021XX              \x03", 22), "SEARCH from 21");
	CHECK(ysf_wx_command(9, 0, NULL, cmd) == 0, "no such command");

	memset(&s, 0, sizeof s);
	ysf_call_rx_init(&rx, &call_sink, &s);
	air_framer_init(&fr, &ysf_air_mode, &rx.rx, fbuf, NULL, YSF_FRAME_SYMS);

	// The node's status: connected to a room
	memset(d, ' ', sizeof d);
	d[0] = 0x00;
	memcpy(d + 1, "\x5D\x51\x5F\x26", 4);
	put_text(d + 5, "54321", 5);
	put_text(d + 10, "N0RPT", 10);
	put_text(d + 20, "NODE-RPT", 14);
	put_text(d + 34, "15", 2);
	put_text(d + 36, "27299", 5);
	put_text(d + 41, "TEST-ROOM", 16);
	len = reply_end(d, 57);
	wx_n = 0;
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_n == 1 && wx_got.kind == YSF_WX_DX && wx_got.connected && !strcmp(wx_got.node_name, "NODE-RPT") &&
	      !strcmp(wx_got.room_id, "27299") && !strcmp(wx_got.room_name, "TEST-ROOM") && !strcmp(wx_got.node_call, "N0RPT"),
	      "DX reply: n %u kind %u '%s' '%s'", wx_n, wx_got.kind, wx_got.node_name, wx_got.room_name);
	// ... and shows as a call without voice, from the node's callsign, ended by its terminator
	CHECK(s.starts == 1 && s.ends == 1 && s.call.dt == YSF_DT_DATA && !strcmp(s.call.src, "N0RPT") && s.nvoice == 0,
	      "a data transmission as a call: starts %u ends %u dt %u src '%s'", s.starts, s.ends, s.call.dt, s.call.src);

	// A room list of 20 rooms, in five blocks (BN counts on past its two bits)
	memset(d, ' ', sizeof d);
	d[0] = 0x00;
	memcpy(d + 1, "\x5D\x46\x5F\x26", 4);
	put_text(d + 5, "21", 2);	// a list ('0' at [5]: a search's result)
	put_text(d + 7, "54321", 5);
	put_text(d + 12, "N0RPT", 10);
	put_text(d + 22, "020", 3);
	put_text(d + 25, "137", 3);
	d[28] = 0x0D;
	for (unsigned j = 0; j < 20; j++) {
		uint8_t *e = d + 29 + j * 50;
		char id[6], name[17];

		snprintf(id, sizeof id, "%05u", 10000 + j);
		snprintf(name, sizeof name, "ROOM-%u", j);
		e[0] = '5';
		put_text(e + 1, id, 5);
		put_text(e + 6, name, 16);
		e[22] = '0';
		e[23] = (uint8_t)('0' + j / 10);
		e[24] = (uint8_t)('0' + j % 10);
		e[49] = 0x0D;
	}
	len = reply_end(d, 29 + 20 * 50);
	wx_n = 0;
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_n == 1 && wx_got.kind == YSF_WX_ALL && !wx_got.search && wx_got.rooms == 20 && wx_got.rooms_total == 137,
	      "list: n %u kind %u rooms %u total %u", wx_n, wx_got.kind, wx_got.rooms, wx_got.rooms_total);
	CHECK(!strcmp(wx_got.room[0].id, "10000") && !strcmp(wx_got.room[19].name, "ROOM-19") && wx_got.room[19].users == 19,
	      "list: rooms read");
	// A frame lost: the reply is dropped, not taken half
	wx_n = 0;
	send_data(&rx, &fr, d, len, 3);
	CHECK(wx_n == 0, "a lost frame drops the reply");
	// A status reply after the list: the list stays in the parser for the screens (the reply only sets its own fields)
	memset(d, ' ', sizeof d);
	d[0] = 0x00;
	memcpy(d + 1, "\x5D\x51\x5F\x26", 4);
	put_text(d + 5, "54321", 5);
	put_text(d + 10, "N0RPT", 10);
	put_text(d + 20, "NODE-RPT", 14);
	put_text(d + 34, "12", 2);
	len = reply_end(d, 57);
	wx_n = 0;
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_n == 1 && wx_got.kind == YSF_WX_DX && !wx_got.connected && wx_got.room_name[0] == '\0' &&
	      !strcmp(wx_got.room[0].id, "10000") && !strcmp(wx_got.room[19].name, "ROOM-19"),
	      "status after a list: the rooms stay, no room now");
	// A node's status with its name and room, then a list from another node: that name and room are not the new one's
	memset(d, ' ', sizeof d);
	d[0] = 0x00;
	memcpy(d + 1, "\x5D\x51\x5F\x26", 4);
	put_text(d + 5, "54321", 5);
	put_text(d + 10, "N0RPT", 10);
	put_text(d + 20, "NODE-RPT", 14);
	put_text(d + 34, "15", 2);
	put_text(d + 36, "27299", 5);
	put_text(d + 41, "TEST-ROOM", 16);
	len = reply_end(d, 57);
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_got.connected && !strcmp(wx_got.node_name, "NODE-RPT"), "status from N0RPT");
	memset(d, ' ', sizeof d);
	d[0] = 0x00;
	memcpy(d + 1, "\x5D\x46\x5F\x26", 4);
	d[5] = '1';
	put_text(d + 7, "22222", 5);
	put_text(d + 12, "N9XYZ", 10);
	put_text(d + 22, "001", 3);
	put_text(d + 25, "001", 3);
	d[29] = '5';
	put_text(d + 30, "30001", 5);
	put_text(d + 35, "OTHER-ROOM", 16);
	put_text(d + 51, "002", 3);
	d[78] = '\r';
	len = reply_end(d, 29 + 50);
	wx_n = 0;
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_n == 1 && !strcmp(wx_got.node_call, "N9XYZ") && wx_got.node_name[0] == '\0' && !wx_got.connected &&
	      wx_got.room_name[0] == '\0' && wx_got.rooms == 1 && !strcmp(wx_got.room[0].name, "OTHER-ROOM"),
	      "a list from another node: '%s' name '%s' connected %d", wx_got.node_call, wx_got.node_name, wx_got.connected);
	// A search's result laid out as the list, the page padded to 20 with blank entries: one match is one room
	memset(d, ' ', sizeof d);
	d[0] = 0x00;
	memcpy(d + 1, "\x5D\x46\x5F\x26", 4);
	d[5] = '0';
	put_text(d + 7, "22222", 5);
	put_text(d + 12, "N9XYZ", 10);
	put_text(d + 22, "020", 3);
	put_text(d + 25, "001", 3);
	d[28] = 0x0D;
	for (unsigned j = 0; j < 20; j++) {
		d[29 + j * 50] = '5';
		d[29 + j * 50 + 49] = 0x0D;
	}
	put_text(d + 30, "90002", 5);
	put_text(d + 35, "LAB-ROOM", 16);
	len = reply_end(d, 29 + 20 * 50);
	wx_n = 0;
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_n == 1 && wx_got.search && wx_got.rooms == 1 && wx_got.rooms_total == 1 &&
	      !strcmp(wx_got.room[0].id, "90002"), "a padded search result: search %d rooms %u total %u", wx_got.search,
	      wx_got.rooms, wx_got.rooms_total);

	// A hotspot gateway's status (its layout read off the radio, names changed): "13" linked to a reflector
	memset(d, ' ', sizeof d);
	d[0] = 0x18;
	memcpy(d + 1, "\x5D\x51\x5F\x26", 4);
	put_text(d + 5, "29615", 5);
	put_text(d + 10, "N0CALL-Y", 10);
	put_text(d + 20, "N0CALL_HS", 14);
	put_text(d + 34, "13", 2);
	put_text(d + 36, "26001", 5);
	put_text(d + 41, "XX-REFLECTOR", 16);
	put_text(d + 57, "000", 3);
	len = reply_end(d, 127);
	wx_n = 0;
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_n == 1 && wx_got.kind == YSF_WX_DX && wx_got.connected && !strcmp(wx_got.room_id, "26001") &&
	      !strcmp(wx_got.room_name, "XX-REFLECTOR"), "gateway status '13': connected %d room '%s' '%s'",
	      wx_got.connected, wx_got.room_id, wx_got.room_name);

	// A commercial repeater's node (the layout of an off-air recording, names changed): status 0x52 with 18 bytes
	// before the node's ID, in a room
	memset(d, ' ', sizeof d);
	d[0] = 0xF0;
	memcpy(d + 1, "\x5D\x52\x5F\x26", 4);
	memcpy(d + 5, "ABCDE12]ul \x1c" "0W", 14);
	put_text(d + 23, "54321", 5);
	put_text(d + 28, "N0RPT-RPT", 10);
	put_text(d + 38, "Town", 14);
	put_text(d + 52, "15", 2);
	put_text(d + 54, "27784", 5);
	put_text(d + 59, "COUNTRY-ROOM", 16);
	put_text(d + 75, "011", 3);
	len = reply_end(d, 125);
	wx_n = 0;
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_n == 1 && wx_got.kind == YSF_WX_DX && wx_got.connected && !strcmp(wx_got.node_id, "54321") &&
	      !strcmp(wx_got.node_call, "N0RPT-RPT") && !strcmp(wx_got.node_name, "Town") &&
	      !strcmp(wx_got.room_id, "27784") && !strcmp(wx_got.room_name, "COUNTRY-ROOM"),
	      "repeater status: n %u kind %u connected %d '%s' '%s' '%s'", wx_n, wx_got.kind, wx_got.connected,
	      wx_got.node_call, wx_got.room_id, wx_got.room_name);
	// ... its disconnect's answer, 0x42 with "12": no room
	d[2] = 0x42;
	put_text(d + 52, "12", 2);
	memset(d + 54, ' ', 24);
	len = reply_end(d, 125);
	wx_n = 0;
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_n == 1 && wx_got.kind == YSF_WX_DISCONNECT && !wx_got.connected && wx_got.room_id[0] == '\0',
	      "repeater disconnect: kind %u connected %d", wx_got.kind, wx_got.connected);
	// ... and connect, 0x42 with "15"
	put_text(d + 52, "15", 2);
	put_text(d + 54, "86787", 5);
	put_text(d + 59, "CITY-ROOM", 16);
	len = reply_end(d, 125);
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_got.kind == YSF_WX_CONNECT && wx_got.connected && !strcmp(wx_got.room_name, "CITY-ROOM"),
	      "repeater connect: kind %u '%s'", wx_got.kind, wx_got.room_name);
	// Its search's result: '1' at [22], three on the page at [23..24], three in all
	memset(d, ' ', sizeof d);
	d[0] = 0xF3;
	memcpy(d + 1, "\x5D\x46\x5F\x26", 4);
	put_text(d + 5, "04", 2);
	put_text(d + 7, "54321", 5);
	put_text(d + 12, "N0RPT-RPT", 10);
	put_text(d + 22, "103003", 6);
	d[28] = 0x0D;
	for (unsigned j = 0; j < 3; j++) {
		uint8_t *e = d + 29 + j * 50;
		char id[6];

		snprintf(id, sizeof id, "%05u", 30000 + j);
		e[0] = '5';
		put_text(e + 1, id, 5);
		put_text(e + 6, "ROOM", 16);
		put_text(e + 22, "002", 3);
		e[49] = 0x0D;
	}
	len = reply_end(d, 29 + 3 * 50);
	wx_n = 0;
	send_data(&rx, &fr, d, len, -1);
	CHECK(wx_n == 1 && wx_got.rooms == 3 && wx_got.rooms_total == 3 && !strcmp(wx_got.room[2].id, "30002"),
	      "repeater search: rooms %u of %u", wx_got.rooms, wx_got.rooms_total);
}

int main(void)
{
	testRid();
	testReference();
	testFec();
	testCallThroughFramer();
	testEngines();
	testRooms();
	printf("# ysfTest %s (%d checks, %d failures)\n", failures ? "FAILED" : "PASSED", checks, failures);
	return failures != 0;
}
