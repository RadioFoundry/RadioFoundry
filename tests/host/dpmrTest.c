// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR frames and calls (core/dpmr, ETSI TS 102 490): the block codes and whole frames against tools/dpmr_ref.py's
// vectors and against blocks a real radio sent (the vector file's air* lines), then whole calls built by the transmit
// side through the shared framer (core/air) into the receive side: voice in order, wake-up headers, late entry, a
// channel code that reads as FS2, coasting over a lost sync, symbol errors, a call cut off, the levels and offset the
// framer measures. IDs are arbitrary 24-bit numbers, never anyone's; voice frames are made-up 72-bit patterns.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dpmr/dpmr.h"
#include "air/air_syms.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define VECTORS		"../vectors/dpmr.txt"

static uint32_t rng_state = 0xD9A11;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static uint32_t unhex(const char *s, uint8_t *out, uint32_t cap)
{
	uint32_t n = 0;

	for (; s[0] && s[1] && n < cap; s += 2) {
		unsigned v;

		if (sscanf(s, "%2x", &v) != 1)
			break;
		out[n++] = (uint8_t)v;
	}
	return n;
}

// ── The codes ──
static void testHamming(void)
{
	for (uint32_t b = 0; b < 256u; b++) {
		uint16_t w = dpmr_ham_encode((uint8_t)b);
		uint8_t d;

		CHECK(dpmr_ham_decode(w, &d) == 0 && d == b, "hamming %02X clean", b);
		for (uint32_t e = 0; e < 12u; e++) {
			int r = dpmr_ham_decode((uint16_t)(w ^ (1u << e)), &d);

			CHECK(r == 1 && d == b, "hamming %02X bit %u: %d %02X", b, e, r, d);
		}
	}
}

static void testScrambleInterleave(void)
{
	uint8_t a[15], b[15], c[15], one[9] = { 0 }, out[9] = { 0 };

	for (uint32_t i = 0; i < sizeof a; i++)
		a[i] = (uint8_t)(i * 37u + 11u);
	memcpy(b, a, sizeof a);
	dpmr_scramble(b, 0, 120u);
	CHECK(memcmp(a, b, sizeof a) != 0, "scrambling changed nothing");
	dpmr_scramble(b, 0, 120u);
	CHECK(memcmp(a, b, sizeof a) == 0, "scrambling twice is not the identity");
	memset(b, 0, sizeof b);
	memset(c, 0, sizeof c);
	dpmr_interleave(a, b, 10u);
	dpmr_deinterleave(b, c, 10u);
	CHECK(memcmp(a, c, sizeof a) == 0, "deinterleave(interleave) is not the identity");
	// 7.5: an input numbered 1..72 comes out 1, 13, 25, 37, 49, 61, 2, ...
	dpmr_bit_put(one, 12, true);
	dpmr_interleave(one, out, 6u);
	CHECK(dpmr_bit(out, 1), "interleaving: input bit 13 is not output bit 2");
}

static void testChannelCodes(void)
{
	char t[16];

	CHECK(dpmr_cc_for_hz(446103125u) == 0x145u, "446.103125 is group A's first");
	CHECK(dpmr_cc_for_hz(446109375u) == 0x1A4u, "446.109375 is group A's second");
	CHECK(dpmr_cc_for_hz(446196875u) == 0x529u, "446.196875 is group A's last");
	CHECK(dpmr_cc_for_hz(433450000u) == 0x145u, "off the dPMR446 raster: group A's first");
	CHECK(dpmr_cc_index(0x1A4u) == 2u && dpmr_cc_index(0xEE5u) == 32u && dpmr_cc_index(0x123u) == 0u, "cc index");
	CHECK(dpmr_cc_group(true, 0) == 0xD11u && dpmr_cc_group(false, 15) == 0x529u, "cc group");
	dpmr_cc_text(0x1A4u, t, sizeof t);
	CHECK(!strcmp(t, "A2"), "cc text %s", t);
	dpmr_cc_text(0xEE5u, t, sizeof t);
	CHECK(!strcmp(t, "B16"), "cc text %s", t);
	dpmr_cc_text(0x00Fu, t, sizeof t);
	CHECK(!strcmp(t, "00F"), "cc text %s", t);
	dpmr_id_text(DPMR_ID_ALL, t, sizeof t);
	CHECK(!strcmp(t, "All"), "id text %s", t);
	dpmr_id_text(DPMR_ID_COMMON(12), t, sizeof t);
	CHECK(!strcmp(t, "CID 12"), "id text %s", t);
	dpmr_id_text(0x123456u, t, sizeof t);
	CHECK(!strcmp(t, "1193046"), "id text %s", t);
	dpmr_id_text(0x123456u, t, 4);
	CHECK(!strcmp(t, "119"), "id text cut %s", t);
}

// The frames a vector line names, made again with the public functions
static void frame_put(const char *what, uint16_t cc, uint8_t *frame, uint16_t *nsyms);

// Every line of tools/dpmr_ref.py's vectors, made again with the public functions
static void testVectors(void)
{
	FILE *f = fopen(VECTORS, "r");
	char key[16], tok[128], line[256];
	int lines = 0, air = 0, frames = 0;

	CHECK(f != NULL, "cannot open %s", VECTORS);
	if (f == NULL)
		return;
	while (fgets(line, sizeof line, f)) {
		unsigned a[8];
		uint8_t want[64], got[64], data[16];
		dpmr_block_result_t res;
		const char *rest;
		int k;

		if (line[0] == '#' || sscanf(line, "%15s%n", key, &k) != 1)
			continue;
		rest = line + k;
		lines++;
		if (!strncmp(key, "fs", 2)) {
			unsigned long long v, c = key[2] == '1' ? DPMR_FS1 : key[2] == '2' ? DPMR_FS2 : key[2] == '3' ? DPMR_FS3 : DPMR_FS4;

			CHECK(sscanf(rest, "%llx", &v) == 1 && v == c, "%s", key);
		} else if (!strcmp(key, "crc7") || !strcmp(key, "crc8")) {
			uint32_t n;
			unsigned c;

			CHECK(sscanf(rest, "%127s %x", tok, &a[0]) == 2, "%s", key);
			n = unhex(tok, data, sizeof data);
			c = key[3] == '7' ? dpmr_crc7(data, 8u * n) : dpmr_crc8(data, 8u * n);
			CHECK(c == a[0], "%s: vector %02X, C %02X", key, a[0], c);
		} else if (!strcmp(key, "ham")) {
			CHECK(sscanf(rest, "%x %x", &a[0], &a[1]) == 2 && dpmr_ham_encode((uint8_t)a[0]) == a[1], "ham %02X", a[0]);
		} else if (!strcmp(key, "hi") || !strcmp(key, "airhi")) {
			bool is_air = key[0] == 'a';
			dpmr_hi_t h, g;

			if (is_air)
				CHECK(sscanf(rest, "%127s %x %x %x %x %x %x %x", tok, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]) == 8, "airhi");
			else
				CHECK(sscanf(rest, "%x %x %x %x %x %x %x %127s", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], tok) == 8, "hi");
			h = (dpmr_hi_t){ (uint8_t)a[0], a[1], a[2], (uint8_t)a[3], (uint8_t)a[4], (uint8_t)a[5], (uint16_t)a[6] };
			unhex(tok, want, sizeof want);
			dpmr_hi_pack(&h, data);
			dpmr_hi_encode(data, got);
			CHECK(!memcmp(got, want, DPMR_HI_CODED_BITS / 8u), "%s %s: the C encoding differs", key, tok);
			dpmr_hi_decode(want, data, &res);
			dpmr_hi_unpack(data, &g);
			CHECK(res.crc_ok && g.called == h.called && g.own == h.own && g.ci == h.ci && g.format == h.format,
			      "%s %s: decoded %06X %06X ci %03X crc %d", key, tok, g.called, g.own, g.ci, res.crc_ok);
			air += is_air;
		} else if (!strcmp(key, "cch") || !strcmp(key, "aircch")) {
			bool is_air = key[0] == 'a';
			dpmr_cch_t c, g;

			if (is_air)
				CHECK(sscanf(rest, "%127s %x %x %x %x %x %x", tok, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 7, "aircch");
			else
				CHECK(sscanf(rest, "%x %x %x %x %x %x %127s", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], tok) == 7, "cch");
			c = (dpmr_cch_t){ (uint8_t)a[0], (uint16_t)a[1], (uint8_t)a[2], (uint8_t)a[3], (uint8_t)a[4], a[5] };
			unhex(tok, want, sizeof want);
			dpmr_cch_pack(&c, data);
			dpmr_cch_encode(data, got);
			CHECK(!memcmp(got, want, DPMR_CCH_CODED_BITS / 8u), "%s %s: the C encoding differs", key, tok);
			dpmr_cch_decode(want, data, &res);
			dpmr_cch_unpack(data, &g);
			CHECK(res.crc_ok && g.fn == c.fn && g.id12 == c.id12 && g.format == c.format, "%s %s: decoded fn %u id %03X",
			      key, tok, g.fn, g.id12);
			air += is_air;
		} else if (!strcmp(key, "end") || !strcmp(key, "airend")) {
			bool is_air = key[0] == 'a';
			dpmr_end_info_t e;

			if (is_air)
				CHECK(sscanf(rest, "%127s %x %x %x %x", tok, &a[0], &a[1], &a[2], &a[3]) == 5, "airend");
			else
				CHECK(sscanf(rest, "%x %x %x %x %127s", &a[0], &a[1], &a[2], &a[3], tok) == 5, "end");
			e = (dpmr_end_info_t){ (uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2], (uint8_t)a[3] };
			unhex(tok, want, sizeof want);
			dpmr_end_pack(&e, data);
			dpmr_end_encode(data, got);
			CHECK(!memcmp(got, want, DPMR_END_CODED_BITS / 8u), "%s %s: the C encoding differs", key, tok);
			dpmr_end_decode(want, data, &res);
			CHECK(res.crc_ok && dpmr_get(data, 4, 4u) == a[2], "%s %s: decode", key, tok);
			air += is_air;
		} else if (!strcmp(key, "cc")) {
			uint8_t bad = 9;

			CHECK(sscanf(rest, "%x %x", &a[0], &a[1]) == 2 && dpmr_cc_air((uint16_t)a[0]) == a[1], "cc %03X", a[0]);
			CHECK(dpmr_cc_from_air(a[1], &bad) == a[0] && bad == 0, "cc %06X back", a[1]);
		} else if (!strcmp(key, "frame")) {
			char what[8];
			uint16_t nsyms = 0;

			CHECK(sscanf(rest, "%7s %x %127s", what, &a[0], tok) == 3, "frame");
			memset(got, 0, sizeof got);
			frame_put(what, (uint16_t)a[0], got, &nsyms);
			CHECK(unhex(tok, want, sizeof want) == nsyms / 4u && !memcmp(got, want, nsyms / 4u),
			      "frame %s: the C frame differs", what);
			frames++;
		} else {
			CHECK(false, "unknown vector line %s", key);
			break;
		}
	}
	fclose(f);
	CHECK(lines >= 30 && air == 7 && frames == 6, "parsed %d vector lines, %d off the air, %d frames", lines, air, frames);
}

// ── Whole calls: the transmit side's symbols through the framer into the receive side ──
#define CALLED		0x123456u
#define OWN		0x0ABCDEu
#define CODE		0x392u		// group A, two symbols from FS2: the receiver must tell them apart

// The vector file's frames: the header of a call from OWN to CALLED, payload frames whose TCH k of frame fn is a
// made-up pattern, the end frame (tools/dpmr_ref.py tch())
static void vec_tch(uint8_t fn, uint8_t k, uint8_t out[DPMR_AMBE_BYTES])
{
	for (uint8_t i = 0; i < DPMR_AMBE_BYTES; i++)
		out[i] = (uint8_t)(fn * 64 + k * 16 + i * 29 + 7);
}

static void frame_put(const char *what, uint16_t cc, uint8_t *frame, uint16_t *nsyms)
{
	dpmr_hi_t h = { DPMR_HT_COMM_START, CALLED, OWN, DPMR_M_VOICE, 0, DPMR_F_P2P, 0 };

	if (!strcmp(what, "hdr")) {
		dpmr_header_put(frame, &h, cc);
		*nsyms = DPMR_FRAME_SYMS;
	} else if (!strcmp(what, "end")) {
		static const dpmr_end_info_t e = { 0, 0, 0, 0 };

		dpmr_end_put(frame, &e);
		*nsyms = DPMR_END_SYMS;
	} else if (!strncmp(what, "pay", 3)) {
		uint8_t fn = (uint8_t)(what[3] - '0'), ambe[DPMR_AMBE_BYTES];
		dpmr_cch_t c = { fn, dpmr_id_quarter(&h, fn), DPMR_M_VOICE, 0, DPMR_F_P2P, 0 };

		dpmr_payload_put(frame, &c, cc);
		for (uint8_t k = 0; k < DPMR_TCH_FRAMES; k++) {
			vec_tch(fn, k, ambe);
			dpmr_tch_put(frame, k, ambe);
		}
		*nsyms = DPMR_FRAME_SYMS;
	}
}

static uint8_t tx_air[60000];		// one dibit per byte
static uint32_t tx_n;

static struct {
	uint32_t starts, updates, ends, voice;
	uint8_t frames[160][DPMR_AMBE_BYTES];
	dpmr_call_t call;
} got;

static void s_start(const dpmr_call_t *c, void *ctx) { (void)ctx; got.starts++; got.call = *c; }
static void s_update(const dpmr_call_t *c, void *ctx) { (void)ctx; got.updates++; got.call = *c; }
static void s_voice(const uint8_t ambe[DPMR_AMBE_BYTES], void *ctx)
{
	(void)ctx;
	if (got.voice < 160)
		memcpy(got.frames[got.voice], ambe, DPMR_AMBE_BYTES);
	got.voice++;
}
static void s_end(const dpmr_call_t *c, void *ctx) { (void)ctx; got.ends++; got.call = *c; }
static const dpmr_call_sink_t sink = { s_start, s_update, s_voice, s_end };

static uint8_t voice[160][DPMR_AMBE_BYTES];

// A call of nvoice voice frames: its symbols to tx_air
static void make_call(uint32_t called, uint32_t own, uint16_t cc, uint8_t headers, uint16_t nvoice)
{
	static const uint8_t silence[DPMR_AMBE_BYTES] = { 0 };
	dpmr_hi_t h = { DPMR_HT_COMM_START, called, own, DPMR_M_VOICE, 0, DPMR_F_P2P, 0 };
	dpmr_call_tx_t t;
	uint16_t sent_voice = 0;
	bool ended = false;

	dpmr_call_tx_begin(&t, &h, cc, headers);
	while (!dpmr_call_tx_done(&t)) {
		uint16_t first, n = dpmr_call_tx_ready(&t, &first);

		// The operator lets go after the last voice frame (as the engine's tx_stop: before what is ready goes)
		if (sent_voice == nvoice && !ended) {
			ended = true;
			dpmr_call_tx_end(&t, silence);
			continue;
		}
		if (!n) {
			CHECK(dpmr_call_tx_voice(&t, voice[sent_voice++]), "the transmitter takes voice when nothing is ready");
			continue;
		}
		// Released a few symbols at a time, as a provider's chunks take them
		if (n > 7)
			n = 7;
		for (uint16_t i = 0; i < n; i++)
			tx_air[tx_n++] = air_sym_get(dpmr_call_tx_syms(&t), (uint16_t)(first + i));
		dpmr_call_tx_sent(&t, n);
	}
}

static void noise_syms(uint16_t n)
{
	while (n--)
		tx_air[tx_n++] = (uint8_t)(rng() & 3);
}

// tx_air[from..] into a receiver in 48-symbol chunks; soft values at a level and offset of our choosing (lvl 0: hard
// symbols only). The call layer's stats back
static dpmr_call_stats_t receive(uint32_t from, int lvl, int off)
{
	static uint8_t fbuf[DPMR_FRAME_BYTES];
	static dpmr_call_rx_t c;
	air_framer_t fr;

	memset(&got, 0, sizeof got);
	dpmr_call_rx_init(&c, &sink, NULL);
	air_framer_init(&fr, &dpmr_air_mode, &c.rx, fbuf, NULL, DPMR_FRAME_SYMS);
	for (uint32_t at = from; at < tx_n; at += 48) {
		uint8_t hard[12] = { 0 };
		int8_t sv[48];
		uint16_t n = (uint16_t)(tx_n - at < 48 ? tx_n - at : 48);
		air_chunk_t ch = { .t_us = at * 417u, .nsyms = n, .sync_at = -1, .hard = hard, .soft = lvl ? sv : NULL };

		for (uint16_t i = 0; i < n; i++) {
			int v = air_sym_level(tx_air[at + i]) * lvl / 3 + off + (int)(rng() % 9) - 4;

			air_sym_put(hard, i, tx_air[at + i]);
			sv[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
		}
		air_framer_rx(&fr, &ch);
	}
	return c.stats;
}

static bool voice_in_order(uint16_t first, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++)
		if (memcmp(got.frames[i], voice[first + i], DPMR_AMBE_BYTES))
			return false;
	return true;
}

// The first symbol of payload frame `frame` (0 = the first after the headers) of a call made at call_at
static uint32_t payload_at(uint32_t call_at, uint8_t headers, uint16_t frame)
{
	return call_at + headers * DPMR_FRAME_SYMS + frame * DPMR_FRAME_SYMS;
}

static void testCalls(void)
{
	dpmr_call_stats_t st;
	uint32_t call_at;

	for (uint16_t i = 0; i < 160; i++)
		for (uint8_t k = 0; k < DPMR_AMBE_BYTES; k++)
			voice[i][k] = (uint8_t)(i * 7 + k * 13 + 1);

	// One header, 22 voice frames: six payload frames (the last two silence), a whole superframe after the first
	tx_n = 0;
	noise_syms(500);
	call_at = tx_n;
	make_call(CALLED, OWN, CODE, 1, 22);
	CHECK(tx_n - call_at == DPMR_FRAME_SYMS + 8 * DPMR_FRAME_SYMS + DPMR_END_SYMS, "call length %u (whole superframes)",
	      tx_n - call_at);
	noise_syms(800);
	for (int pass = 0; pass < 3; pass++) {
		// Hard symbols; soft at the nominal level; soft at 70 % with an offset (the framer's levels)
		st = receive(0, pass == 0 ? 0 : pass == 1 ? AIR_SOFT_OUTER : AIR_SOFT_OUTER * 7 / 10, pass == 2 ? 20 : 0);
		CHECK(got.starts == 1 && got.ends == 1 && got.call.ended, "pass %d: starts %u ends %u", pass, got.starts, got.ends);
		CHECK(got.voice == 32 && voice_in_order(0, 22), "pass %d: voice %u in order %d", pass, got.voice,
		      voice_in_order(0, 22));
		CHECK(got.call.called == CALLED && got.call.own == OWN && got.call.cc == CODE &&
		      got.call.have == (DPMR_HAVE_CALLED | DPMR_HAVE_OWN | DPMR_HAVE_CC | DPMR_HAVE_HEADER),
		      "pass %d: called %06X own %06X cc %03X have %02X", pass, got.call.called, got.call.own, got.call.cc,
		      got.call.have);
		CHECK(st.headers == 1 && st.frames == 8 && st.hi_fail == 0 && st.cch_fail == 0 && st.ends == 1 &&
		      st.end_fail == 0 && st.ham_fixed == 0 && st.ham_bad == 0, "pass %d: stats", pass);
	}

	// Nine wake-up headers and the normal one, as a dPMR446 radio sends them: one call
	tx_n = 0;
	noise_syms(300);
	call_at = tx_n;
	make_call(DPMR_ID_COMMON(87), DPMR_ID_COMMON(87), 0x1A4, 10, 8);
	noise_syms(800);
	st = receive(0, AIR_SOFT_OUTER, 0);
	CHECK(got.starts == 1 && got.ends == 1 && st.headers == 10 && got.voice == 16 && voice_in_order(0, 8),
	      "wake-up headers: starts %u headers %u voice %u", got.starts, st.headers, got.voice);
	CHECK(got.call.called == DPMR_ID_COMMON(87) && (got.call.have & DPMR_HAVE_HEADER), "wake-up headers: IDs");

	// Late entry, from inside frame 1's CCH: the IDs come from the CCH quarters, the code from frame 2. With CODE
	// two symbols from FS2, the search meets frame 2's code first and must not lock a frame off
	tx_n = 0;
	noise_syms(300);
	call_at = tx_n;
	make_call(CALLED, OWN, CODE, 1, 40);
	noise_syms(800);
	st = receive(payload_at(call_at, 1, 0) + 20, AIR_SOFT_OUTER, 0);
	CHECK(got.starts == 1 && got.ends == 1 && !(got.call.have & DPMR_HAVE_HEADER), "late: starts %u ends %u",
	      got.starts, got.ends);
	// Frame 2's code read as FS2 is turned down; the pair of frames 3-4 is the first taken
	CHECK(got.voice == 48 - 8 && !memcmp(got.frames[0], voice[8], DPMR_AMBE_BYTES),
	      "late: voice %u, first frame %d", got.voice, memcmp(got.frames[0], voice[8], DPMR_AMBE_BYTES));
	CHECK(got.call.called == CALLED && got.call.own == OWN && got.call.cc == CODE &&
	      (got.call.have & (DPMR_HAVE_CALLED | DPMR_HAVE_OWN | DPMR_HAVE_CC)) == (DPMR_HAVE_CALLED | DPMR_HAVE_OWN | DPMR_HAVE_CC),
	      "late: called %06X own %06X cc %03X have %02X", got.call.called, got.call.own, got.call.cc, got.call.have);

	// A lost FS2 (frame 3's, five symbols turned) and a few symbol errors in the TCH: the pair is placed by
	// counting, its CCH still reads, voice keeps its order
	tx_n = 0;
	noise_syms(300);
	call_at = tx_n;
	make_call(CALLED, OWN, CODE, 1, 32);
	noise_syms(800);
	for (uint32_t i = 0; i < 5; i++)
		tx_air[payload_at(call_at, 1, 2) + 2 * i] ^= 2;
	tx_air[payload_at(call_at, 1, 5) + 100] ^= 2;
	st = receive(0, AIR_SOFT_OUTER, 0);
	// (TCH 1 of frame 6 is voice frame 21, the one with the error: the voice path's FEC is for it)
	CHECK(got.starts == 1 && got.ends == 1 && got.voice == 32 && voice_in_order(0, 21) &&
	      memcmp(got.frames[21], voice[21], DPMR_AMBE_BYTES) && !memcmp(got.frames[22], voice[22], DPMR_AMBE_BYTES),
	      "coasting: starts %u voice %u", got.starts, got.voice);
	CHECK(st.cch_fail == 0 && st.frames == 8, "coasting: frames %u cch fails %u", st.frames, st.cch_fail);

	// A CCH error the Hamming code corrects
	tx_n = 0;
	noise_syms(200);
	call_at = tx_n;
	make_call(CALLED, OWN, CODE, 1, 16);
	noise_syms(800);
	tx_air[payload_at(call_at, 1, 1) + DPMR_CCH_SYM + 3] ^= 1;
	st = receive(0, 0, 0);
	CHECK(got.starts == 1 && got.voice == 16 && st.ham_fixed == 1 && st.cch_fail == 0, "hamming: fixed %u fails %u",
	      st.ham_fixed, st.cch_fail);

	// Cut off mid-call: no end frame, the call ends when the syncs stop (lost, not ended)
	tx_n = 0;
	noise_syms(200);
	call_at = tx_n;
	make_call(CALLED, OWN, CODE, 1, 40);
	tx_n = payload_at(call_at, 1, 5) + 30;
	noise_syms(2000);
	st = receive(0, AIR_SOFT_OUTER, 0);
	CHECK(got.starts == 1 && got.ends == 1 && !got.call.ended && st.ends == 0, "cut: starts %u ends %u ended %d",
	      got.starts, got.ends, got.call.ended);

	// Noise alone: nothing
	tx_n = 0;
	noise_syms(30000);
	st = receive(0, AIR_SOFT_OUTER, 0);
	CHECK(got.starts == 0 && got.voice == 0, "noise: starts %u voice %u", got.starts, got.voice);
	st = receive(0, 0, 0);
	CHECK(got.starts == 0 && got.voice == 0, "noise, hard: starts %u voice %u", got.starts, got.voice);

	// Two calls back to back, the second from someone else
	tx_n = 0;
	noise_syms(200);
	make_call(CALLED, OWN, CODE, 2, 8);
	noise_syms(100);
	make_call(DPMR_ID_ALL, 0x000777u, CODE, 1, 8);
	noise_syms(800);
	st = receive(0, AIR_SOFT_OUTER, 0);
	CHECK(got.starts == 2 && got.ends == 2 && got.voice == 32 && got.call.called == DPMR_ID_ALL && got.call.own == 0x777u,
	      "two calls: starts %u ends %u voice %u", got.starts, got.ends, got.voice);
}

// What the framer's table says to a provider
static void testProfile(void)
{
	const air_profile_t *p = &dpmr_air_mode.profile;

	CHECK(p->sym_rate == 2400 && p->levels == 4 && p->dev_outer_hz == 1050 && p->shape == AIR_SHAPE_RRC &&
	      p->shape_param == 20, "physical layer");
	// A chip correlator takes the 24-symbol, all-outer FS1 first among the patterns it can hold
	for (uint8_t i = 0; i < p->nsyncs; i++) {
		const air_sync_t *s = &p->sync[i];
		uint64_t want = s->id == DPMR_SYNC_FS1 ? DPMR_FS1 : s->id == DPMR_SYNC_FS2 ? DPMR_FS2 : DPMR_FS3;
		uint64_t v = 0;

		for (uint8_t k = 0; k < s->nsyms; k++) {
			uint8_t d = air_sym_get(s->syms, k);

			v = v << 2 | d;
			CHECK(d == 1 || d == 3, "sync %u symbol %u is not outer", s->id, k);
		}
		CHECK(v == want, "sync %u: %llx", s->id, (unsigned long long)v);
	}
}

int main(void)
{
	testHamming();
	testScrambleInterleave();
	testChannelCodes();
	testVectors();
	testProfile();
	testCalls();
	printf("dpmrTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
