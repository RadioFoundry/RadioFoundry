// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host tests for D-STAR DV framing (core/dstar/dstar_fec.c, dstar_frame.c; docs/spec/dstar-modem.md 3): the header
 * CRC, coding, interleaving and scrambling against the vectors of tools/dstar_ref.py (an independent reference written
 * from the JARL standard, tests/vectors/dstar.txt), the soft Viterbi decoder against bit errors and erasures, the
 * slow data both ways, and whole transmissions from the transmitter's bit stream into the receive deframer: header,
 * voice frames, message, the end pattern, late entry from the slow data, inverted polarity, a lost signal, noise.
 *
 *   make -C tests/host run-dstar
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dstar/dstar_fec.h"
#include "dstar/dstar_frame.h"
#include "fuzz.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#ifndef VECTORS
#define VECTORS "../vectors/dstar.txt"
#endif

static size_t unhex(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;
	if (strcmp(s, "-") == 0) {
		return 0;
	}
	while (s[0] && s[1] && n < max) {
		unsigned v;
		if (sscanf(s, "%2x", &v) != 1) {
			break;
		}
		out[n++] = (uint8_t)v;
		s += 2;
	}
	return n;
}

// Air bits to packed soft values of a given strength
static void to_soft(const uint8_t *air, uint32_t nbits, int8_t strength, uint8_t *soft)
{
	for (uint32_t i = 0; i < nbits; i++) {
		dstar_soft_put(soft, i, dstar_bit(air, i) ? strength : (int8_t)-strength);
	}
}

// --- the reference's vectors ---

static void test_vectors(void)
{
	FILE *f = fopen(VECTORS, "r");
	CHECK(f != NULL, "no %s", VECTORS);
	if (!f) {
		return;
	}
	char line[1024];
	int n_crc = 0, n_hdr = 0, n_data = 0, n_msg = 0, n_slowhdr = 0, n_pn = 0, n_sync = 0;
	while (fgets(line, sizeof(line), f)) {
		char kind[16], a[512], b[512];
		if (line[0] == '#' || sscanf(line, "%15s %511s %511[^\n]", kind, a, b) < 2) {
			continue;
		}
		uint8_t x[256], y[256];
		if (strcmp(kind, "crc") == 0) {
			size_t n = unhex(a, x, sizeof(x));
			unsigned want;
			sscanf(b, "%x", &want);
			CHECK(dstar_crc(x, n) == want, "crc %s: %04X, want %04X", a, dstar_crc(x, n), want);
			n_crc++;
		} else if (strcmp(kind, "pn") == 0) {
			size_t n = unhex(a, x, sizeof(x));
			for (uint32_t i = 0; i < 127u && i / 8u < n; i++) {
				CHECK(dstar_pn_bit(i) == dstar_bit(x, i), "pn bit %u", i);
				CHECK(dstar_pn_bit(i + 127u) == dstar_bit(x, i), "pn period at %u", i);
			}
			n_pn++;
		} else if (strcmp(kind, "header") == 0) {
			CHECK(unhex(a, x, sizeof(x)) == DSTAR_HEADER_BYTES, "header length");
			CHECK(unhex(b, y, sizeof(y)) == DSTAR_HEADER_AIR_BYTES, "air length");
			CHECK(dstar_header_crc_ok(x), "vector header CRC");
			uint8_t air[DSTAR_HEADER_AIR_BYTES];
			dstar_header_encode(x, air);
			CHECK(memcmp(air, y, DSTAR_HEADER_AIR_BYTES) == 0, "header encode differs from the reference: %s", a);
			uint8_t soft[DSTAR_HEADER_SOFT_BYTES], hdr[DSTAR_HEADER_BYTES];
			to_soft(y, DSTAR_HEADER_BITS, 5, soft);
			dstar_header_result_t r = dstar_header_decode(soft, hdr);
			CHECK(r.crc_ok && r.corrected == 0 && memcmp(hdr, x, DSTAR_HEADER_BYTES) == 0, "decode clean vector");
			// Header fields round trip
			dstar_header_t h;
			CHECK(dstar_header_unpack(x, &h), "unpack");
			uint8_t again[DSTAR_HEADER_BYTES];
			dstar_header_pack(&h, again);
			CHECK(memcmp(again, x, DSTAR_HEADER_BYTES) == 0, "pack");
			n_hdr++;
		} else if (strcmp(kind, "data") == 0) {
			CHECK(unhex(a, x, sizeof(x)) == 3 && unhex(b, y, sizeof(y)) == 3, "data lengths");
			dstar_data_scramble(x);
			CHECK(memcmp(x, y, 3) == 0, "data scramble %s", a);
			dstar_data_scramble(x);
			n_data++;
		} else if (strcmp(kind, "slowmsg") == 0) {
			char msg[DSTAR_MSG_LEN + 1] = { 0 };
			memcpy(msg, a, strnlen(a, DSTAR_MSG_LEN));
			for (char *p = msg; *p; p++) {
				if (*p == '_') {
					*p = ' ';
				}
			}
			uint8_t hdr[DSTAR_HEADER_BYTES] = { 0 };
			dstar_slow_tx_t tx;
			dstar_slow_tx_init(&tx, hdr, msg);
			// Superframe 0 carries the message: blocks 0-3 at frames 1-8
			const char *p = b;
			for (int blk = 0; blk < 4; blk++) {
				CHECK(unhex(p, y, 6) == 6, "block");
				p += 13;
				uint8_t got[6];
				dstar_slow_tx_frame(&tx, 0, (uint8_t)(1 + 2 * blk), got);
				dstar_slow_tx_frame(&tx, 0, (uint8_t)(2 + 2 * blk), &got[3]);
				CHECK(memcmp(got, y, 6) == 0, "message block %d", blk);
			}
			n_msg++;
		} else if (strcmp(kind, "slowhdr") == 0) {
			CHECK(unhex(a, x, sizeof(x)) == DSTAR_HEADER_BYTES, "slowhdr header");
			dstar_slow_tx_t tx;
			dstar_slow_tx_init(&tx, x, NULL);
			const char *p = b;
			for (int blk = 0; blk < 9; blk++) {
				CHECK(unhex(p, y, 6) == 6, "block");
				p += 13;
				uint8_t got[6];
				dstar_slow_tx_frame(&tx, 1, (uint8_t)(1 + 2 * blk), got);
				dstar_slow_tx_frame(&tx, 1, (uint8_t)(2 + 2 * blk), &got[3]);
				CHECK(memcmp(got, y, 6) == 0, "header block %d", blk);
			}
			n_slowhdr++;
		} else if (strcmp(kind, "sync") == 0) {
			char rs[64], es[64];
			CHECK(sscanf(b, "%63s %63s", rs, es) == 2, "sync fields");
			uint8_t r[8], e[8];
			CHECK(unhex(a, x, 8) == 2 && memcmp(x, dstar_frame_sync, 2) == 0, "frame sync pattern");
			CHECK(unhex(rs, r, 8) == 3 && memcmp(r, dstar_resync, 3) == 0, "resync pattern");
			CHECK(unhex(es, e, 8) == 6 && memcmp(e, dstar_end, 6) == 0, "end pattern");
			n_sync++;
		}
	}
	fclose(f);
	CHECK(n_crc >= 3 && n_pn == 1 && n_hdr >= 4 && n_data >= 4 && n_msg == 1 && n_slowhdr == 1 && n_sync == 1,
	      "vector counts %d %d %d %d %d %d %d", n_crc, n_pn, n_hdr, n_data, n_msg, n_slowhdr, n_sync);
}

// --- header decoder against errors ---

static void test_header_errors(void)
{
	dstar_header_t h;
	dstar_header_set(&h, DSTAR_FLAG1_REPEATER, "XX1XX  G", "XX1XX  B", "CQCQCQ", "N0CALL", "TEST");
	uint8_t hdr[DSTAR_HEADER_BYTES], air[DSTAR_HEADER_AIR_BYTES];
	dstar_header_pack(&h, hdr);
	dstar_header_encode(hdr, air);
	fuzz_t f;
	fuzz_init(&f, 0xD57A);

	// Hard errors spread at random: the interleaver turns them into isolated coded errors, which the K=3 code
	// corrects a few at a time; 12 errors in 660 (1.8 %) decode nearly always
	int ok12 = 0, tries = 200;
	for (int t = 0; t < tries; t++) {
		uint8_t soft[DSTAR_HEADER_SOFT_BYTES], out[DSTAR_HEADER_BYTES];
		to_soft(air, DSTAR_HEADER_BITS, 4, soft);
		for (int e = 0; e < 12; e++) {
			uint32_t i = fuzz_below(&f, DSTAR_HEADER_BITS);
			dstar_soft_put(soft, i, (int8_t)-dstar_soft_get(soft, i));
		}
		dstar_header_result_t r = dstar_header_decode(soft, out);
		if (r.crc_ok && memcmp(out, hdr, sizeof(hdr)) == 0) {
			ok12++;
		}
	}
	CHECK(ok12 >= tries * 95 / 100, "12 random errors: %d of %d decoded", ok12, tries);

	// A burst of 24 consecutive air bits (5 ms: a fade) lands one per coded row: corrected
	{
		uint8_t soft[DSTAR_HEADER_SOFT_BYTES], out[DSTAR_HEADER_BYTES];
		to_soft(air, DSTAR_HEADER_BITS, 4, soft);
		for (uint32_t i = 200; i < 224; i++) {
			dstar_soft_put(soft, i, (int8_t)-dstar_soft_get(soft, i));
		}
		dstar_header_result_t r = dstar_header_decode(soft, out);
		CHECK(r.crc_ok && r.corrected == 24, "burst of 24: crc %d corrected %u", r.crc_ok, r.corrected);
	}
	// Erasures (soft 0) cost nothing but margin: a quarter of the bits, picked at random, erased still decodes. (A
	// regular pattern such as every 4th air bit lines up with the interleaver's rows and erases both coded bits of the
	// first six steps, more than a code with two bits of memory can bridge: no decoder can)
	{
		uint8_t soft[DSTAR_HEADER_SOFT_BYTES], out[DSTAR_HEADER_BYTES];
		to_soft(air, DSTAR_HEADER_BITS, 4, soft);
		for (uint32_t k = 0; k < DSTAR_HEADER_BITS / 4u; k++) {
			dstar_soft_put(soft, fuzz_below(&f, DSTAR_HEADER_BITS), 0);
		}
		CHECK(dstar_header_decode(soft, out).crc_ok, "a quarter erased");
	}
	// Weak wrong bits lose to strong right ones: 60 errors at soft 1 among bits at 6
	{
		uint8_t soft[DSTAR_HEADER_SOFT_BYTES], out[DSTAR_HEADER_BYTES];
		to_soft(air, DSTAR_HEADER_BITS, 6, soft);
		for (uint32_t i = 0; i < DSTAR_HEADER_BITS; i += 11) {
			dstar_soft_put(soft, i, dstar_bit(air, i) ? -1 : 1);
		}
		CHECK(dstar_header_decode(soft, out).crc_ok, "60 weak errors");
	}
	// Garbage fails the CRC rather than inventing a header
	{
		uint8_t soft[DSTAR_HEADER_SOFT_BYTES], out[DSTAR_HEADER_BYTES];
		int passed = 0;
		for (int t = 0; t < 200; t++) {
			for (uint32_t i = 0; i < DSTAR_HEADER_BITS; i++) {
				dstar_soft_put(soft, i, (int8_t)((int)fuzz_below(&f, 15) - 7));
			}
			passed += dstar_header_decode(soft, out).crc_ok;
		}
		CHECK(passed == 0, "noise passed the CRC %d times", passed);
	}
	// A corrupted CRC byte is caught
	hdr[40] ^= 0x01;
	CHECK(!dstar_header_crc_ok(hdr), "bad CRC accepted");
}

// --- slow data receive ---

static void test_slow_rx(void)
{
	dstar_header_t h;
	dstar_header_set(&h, 0, "DIRECT", "DIRECT", "CQCQCQ", "N0CALL", "ID51");
	uint8_t hdr[DSTAR_HEADER_BYTES];
	dstar_header_pack(&h, hdr);
	dstar_slow_tx_t tx;
	dstar_slow_tx_init(&tx, hdr, "Testing 1 2 3");
	dstar_slow_rx_t rx;
	dstar_slow_rx_init(&rx);
	int msgs = 0, hdrs = 0;
	for (uint32_t sf = 0; sf < 4; sf++) {
		for (uint8_t fr = 0; fr < DSTAR_SUPERFRAME; fr++) {
			uint8_t d[3] = { 0x55, 0x2D, 0x16 };
			if (fr) {
				dstar_slow_tx_frame(&tx, sf, fr, d);
			}
			dstar_slow_event_t ev = dstar_slow_rx_frame(&rx, fr, d, true);
			if (ev == DSTAR_SLOW_MESSAGE) {
				msgs++;
				CHECK(memcmp(rx.msg, "Testing 1 2 3       ", DSTAR_MSG_LEN) == 0, "message %.20s", rx.msg);
			} else if (ev == DSTAR_SLOW_HEADER) {
				hdrs++;
				CHECK(memcmp(rx.hdr, hdr, DSTAR_HEADER_BYTES) == 0, "slow header");
			}
		}
	}
	CHECK(msgs == 1, "message delivered %d times (once expected)", msgs);
	CHECK(hdrs == 2, "header from slow data %d times (superframes 1 and 3)", hdrs);

	// A lost data frame in the header's run spoils that superframe's copy only
	dstar_slow_rx_init(&rx);
	hdrs = 0;
	for (uint32_t sf = 1; sf < 4; sf += 2) {
		for (uint8_t fr = 0; fr < DSTAR_SUPERFRAME; fr++) {
			uint8_t d[3] = { 0 };
			if (fr) {
				dstar_slow_tx_frame(&tx, sf, fr, d);
			}
			bool ok = !(sf == 1 && fr == 7);
			hdrs += dstar_slow_rx_frame(&rx, fr, d, ok) == DSTAR_SLOW_HEADER;
		}
	}
	CHECK(hdrs == 1, "header with a lost frame: %d", hdrs);

	// No message: every superframe repeats the header
	dstar_slow_tx_init(&tx, hdr, NULL);
	dstar_slow_rx_init(&rx);
	hdrs = 0;
	for (uint32_t sf = 0; sf < 3; sf++) {
		for (uint8_t fr = 0; fr < DSTAR_SUPERFRAME; fr++) {
			uint8_t d[3] = { 0 };
			if (fr) {
				dstar_slow_tx_frame(&tx, sf, fr, d);
			}
			hdrs += dstar_slow_rx_frame(&rx, fr, d, true) == DSTAR_SLOW_HEADER;
		}
	}
	CHECK(hdrs == 3, "header every superframe without a message: %d", hdrs);
}

// --- whole transmissions through the deframer ---

typedef struct {
	int headers, slow_headers, voices, messages, ends, losts;
	dstar_end_t why;
	dstar_header_t h;
	uint8_t first_frame;
	uint32_t voice_bad;		// voice frames whose content isn't the counter pattern sent
	uint32_t next_voice;
} rx_log_t;

static void on_header(void *ctx, const dstar_header_t *h, bool from_slow)
{
	rx_log_t *l = ctx;
	if (from_slow) {
		l->slow_headers++;
	} else {
		l->headers++;
	}
	l->h = *h;
}

// The transmitter's voice: frame k carries k in byte 0 and its complement in byte 8
typedef struct {
	uint32_t sent, limit;
} voice_src_t;

static bool pull_voice(void *ctx, uint8_t v[DSTAR_VOICE_BYTES])
{
	voice_src_t *s = ctx;
	if (s->sent >= s->limit) {
		return false;
	}
	memset(v, 0xA5, DSTAR_VOICE_BYTES);
	v[0] = (uint8_t)s->sent;
	v[8] = (uint8_t)~s->sent;
	s->sent++;
	return true;
}

static void on_voice(void *ctx, const uint8_t v[DSTAR_VOICE_BYTES], uint8_t frame)
{
	rx_log_t *l = ctx;
	if (l->voices == 0) {
		l->first_frame = frame;
		l->next_voice = v[0];
	}
	if (v[0] != (uint8_t)l->next_voice || v[8] != (uint8_t)~l->next_voice || v[4] != 0xA5 ||
	    frame != l->next_voice % DSTAR_SUPERFRAME) {
		l->voice_bad++;
	}
	l->next_voice++;
	l->voices++;
}

static void on_message(void *ctx, const char msg[DSTAR_MSG_LEN])
{
	rx_log_t *l = ctx;
	l->messages++;
	CHECK(memcmp(msg, "CQ from the bench   ", DSTAR_MSG_LEN) == 0, "message %.20s", msg);
}

static void on_end(void *ctx, dstar_end_t why)
{
	rx_log_t *l = ctx;
	if (why == DSTAR_END_PATTERN) {
		l->ends++;
	} else {
		l->losts++;
	}
}

static const dstar_rx_cb_t cbs = { on_header, on_voice, on_message, on_end, NULL };

// Run a transmission of nvoice frames into a deframer: skip the first skip bits, invert if asked, flip the bits in
// [flip_from, flip_to), stop sending after cut bits (0 = all), then trailing noise bits
static void run(rx_log_t *l, uint32_t nvoice, uint32_t skip, bool invert, uint32_t flip_from, uint32_t flip_to,
		uint32_t cut, uint32_t noise, dstar_rx_t *rx)
{
	memset(l, 0, sizeof(*l));
	dstar_rx_cb_t cb = cbs;
	cb.ctx = l;
	dstar_rx_init(rx, &cb);
	dstar_header_t h;
	dstar_header_set(&h, DSTAR_FLAG1_REPEATER, "XX1XX  G", "XX1XX  B", "CQCQCQ", "N0CALL", "TEST");
	voice_src_t src = { 0, nvoice };
	dstar_tx_t tx;
	dstar_tx_init(&tx, &h, "CQ from the bench", 128, pull_voice, &src);
	fuzz_t f;
	fuzz_init(&f, 0xB175);
	// Noise first, as a receiver hears before the carrier
	for (int i = 0; i < 500; i++) {
		dstar_rx_bit(rx, (fuzz_next(&f) & 1u) ? 40 : -40, 64);
	}
	uint32_t i = 0;
	int b;
	while ((b = dstar_tx_bit(&tx)) >= 0) {
		if (cut && i >= cut) {
			break;
		}
		if (i >= flip_from && i < flip_to) {
			b ^= 1;
		}
		if (i++ < skip) {
			continue;
		}
		int16_t s = b ? 64 : -64;
		dstar_rx_bit(rx, invert ? (int16_t)-s : s, 64);
	}
	for (uint32_t k = 0; k < noise; k++) {
		dstar_rx_bit(rx, (fuzz_next(&f) & 1u) ? 40 : -40, 64);
	}
}

static void test_stream(void)
{
	static dstar_rx_t rx;
	rx_log_t l;
	const uint32_t hdr_end = 128 + DSTAR_FRAME_SYNC_BITS + DSTAR_HEADER_BITS;

	// Clean: header, 100 voice frames in order, the message once, the end pattern
	run(&l, 100, 0, false, 0, 0, 0, 2000, &rx);
	CHECK(l.headers == 1 && l.voices == 100 && l.voice_bad == 0 && l.messages == 1 && l.ends == 1 && l.losts == 0,
	      "clean: headers %d voices %d bad %u messages %d ends %d losts %d", l.headers, l.voices, l.voice_bad,
	      l.messages, l.ends, l.losts);
	CHECK(memcmp(l.h.my, "N0CALL  ", 8) == 0 && memcmp(l.h.rpt1, "XX1XX  B", 8) == 0 && l.h.flag[0] == 0x40,
	      "header fields");
	CHECK(rx.slow_headers >= 2 && rx.syncs >= 4 && rx.sync_misses == 0, "slow headers %u syncs %u misses %u",
	      rx.slow_headers, rx.syncs, rx.sync_misses);
	printf("# dstar rx state %zu bytes, tx state %zu bytes\n", sizeof(dstar_rx_t), sizeof(dstar_tx_t));

	// Inverted: the frame sync is found inverted and everything else follows
	run(&l, 60, 0, true, 0, 0, 0, 500, &rx);
	CHECK(l.headers == 1 && l.voices == 60 && l.voice_bad == 0 && l.ends == 1, "inverted: %d %d %u %d", l.headers,
	      l.voices, l.voice_bad, l.ends);

	// Late entry: join 30 frames in (after the header). The first resync pattern after joining (frame 42) isn't
	// trusted alone, so voice starts at the second one (frame 63, itself a frame 0) and the header comes from the
	// slow data
	run(&l, 120, hdr_end + 30 * DSTAR_FRAME_BITS + 5, false, 0, 0, 0, 500, &rx);
	CHECK(l.headers == 0 && l.slow_headers == 1 && l.voice_bad == 0 && l.ends == 1 && rx.late_entries == 1,
	      "late entry: headers %d slow %d bad %u ends %d late %u", l.headers, l.slow_headers, l.voice_bad, l.ends,
	      rx.late_entries);
	CHECK(l.voices == 120 - 63 && l.first_frame == 0, "late entry voice from frame 63: %d, first at %u", l.voices,
	      l.first_frame);
	CHECK(memcmp(l.h.my, "N0CALL  ", 8) == 0, "slow header fields");

	// A ruined header: voice still comes by late entry
	run(&l, 80, 0, false, 128 + DSTAR_FRAME_SYNC_BITS, 128 + DSTAR_FRAME_SYNC_BITS + 200, 0, 500, &rx);
	CHECK(l.headers == 0 && rx.headers_bad == 1 && l.slow_headers == 1 && l.voices > 0 && l.ends == 1,
	      "bad header: %d bad %u slow %d voices %d", l.headers, rx.headers_bad, l.slow_headers, l.voices);

	// Errors in voice and slow data frames don't lose the lock
	run(&l, 60, 0, false, hdr_end + 5 * DSTAR_FRAME_BITS, hdr_end + 5 * DSTAR_FRAME_BITS + 3, 0, 500, &rx);
	CHECK(l.headers == 1 && l.voices == 60 && l.voice_bad == 1 && l.ends == 1, "voice error: %d %u %d", l.voices,
	      l.voice_bad, l.ends);

	// The carrier goes without its end pattern: lost after three missing resync patterns, not before
	run(&l, 200, 0, false, 0, 0, hdr_end + 50 * DSTAR_FRAME_BITS, 8000, &rx);
	CHECK(l.ends == 0 && l.losts == 1 && l.voices >= 50 && l.voices <= 50 + 3 * 21, "lost: ends %d losts %d voices %d",
	      l.ends, l.losts, l.voices);

	// Noise alone, for a minute of bits: maybe a false late entry, never anything handed on
	{
		memset(&l, 0, sizeof(l));
		dstar_rx_cb_t cb = cbs;
		cb.ctx = &l;
		dstar_rx_init(&rx, &cb);
		fuzz_t f;
		fuzz_init(&f, 0x0153);
		for (int i = 0; i < 4800 * 60; i++) {
			dstar_rx_bit(&rx, (int16_t)((int)fuzz_below(&f, 129) - 64), 64);
		}
		CHECK(l.headers == 0 && l.voices == 0 && l.messages == 0 && l.ends == 0 && l.losts == 0 && l.slow_headers == 0,
		      "noise produced output: %d %d %d %d %d", l.headers, l.voices, l.messages, l.ends, l.losts);
	}
}

// The transmitter with no codec sends the standard's silence frame
static void test_tx_silence(void)
{
	dstar_header_t h;
	dstar_header_set(&h, 0, "DIRECT", "DIRECT", "CQCQCQ", "N0CALL", "");
	dstar_tx_t tx;
	dstar_tx_init(&tx, &h, NULL, 0, NULL, NULL);
	uint32_t skip = DSTAR_PREAMBLE_MIN + DSTAR_FRAME_SYNC_BITS + DSTAR_HEADER_BITS;
	uint8_t v[DSTAR_VOICE_BYTES] = { 0 };
	for (uint32_t i = 0; i < skip + DSTAR_VOICE_BITS; i++) {
		int b = dstar_tx_bit(&tx);
		if (i >= skip) {
			dstar_bit_put(v, i - skip, b != 0);
		}
	}
	CHECK(memcmp(v, dstar_voice_silence, DSTAR_VOICE_BYTES) == 0, "silence frame");
	// The first data frame is the resync pattern, unscrambled
	uint8_t d[3];
	for (uint32_t i = 0; i < 24; i++) {
		dstar_bit_put(d, i, dstar_tx_bit(&tx) != 0);
	}
	CHECK(memcmp(d, dstar_resync, 3) == 0, "frame 0 data is the resync pattern");
}

// Random soft bits and patterns spliced in: the deframer never reads or writes past its state
static void test_fuzz(void)
{
	static dstar_rx_t rx;
	rx_log_t l;
	memset(&l, 0, sizeof(l));
	dstar_rx_cb_t cb = cbs;
	cb.ctx = &l;
	dstar_rx_init(&rx, &cb);
	fuzz_t f;
	fuzz_init(&f, 0xF022);
	const uint8_t *pats[] = { dstar_frame_sync, dstar_resync, dstar_end };
	const uint32_t lens[] = { DSTAR_FRAME_SYNC_BITS, DSTAR_DATA_BITS, DSTAR_END_BITS };
	for (long i = 0; i < fuzz_iters(20000); i++) {
		if (fuzz_below(&f, 50) == 0) {
			uint32_t p = fuzz_below(&f, 3);
			for (uint32_t k = 0; k < lens[p]; k++) {
				dstar_rx_bit(&rx, dstar_bit(pats[p], k) ? 64 : -64, 64);
			}
		}
		dstar_rx_bit(&rx, (int16_t)(fuzz_next(&f) & 0xFFFFu), (int16_t)fuzz_below(&f, 200));
		CHECK(rx.bit <= DSTAR_HEADER_BITS && rx.frame < DSTAR_SUPERFRAME && rx.slow.hdr_len <= DSTAR_HEADER_BYTES,
		      "state out of range");
		if (failures) {
			break;
		}
	}
}

// A received field as text: the padding cut, everything outside printable ASCII a '?' (an accented letter, a radio's own
// character set, a control byte the UI would take for a glyph code or a text alternate), never a byte passed on
static void test_field_text(void)
{
	char out[DSTAR_MSG_LEN + 1];
	dstar_header_t h;
	uint8_t air[DSTAR_HEADER_BYTES];
	dstar_header_set(&h, 0, "DIRECT", "DIRECT", "CQCQCQ", "EA1\xC9Z", "ID\x82");
	dstar_header_pack(&h, air);
	CHECK(dstar_header_unpack(air, &h), "header back");
	dstar_field_text(out, h.my, sizeof h.my);
	CHECK(strcmp(out, "EA1?Z") == 0, "MY with a Latin-1 letter: '%s'", out);
	dstar_field_text(out, h.suffix, sizeof h.suffix);
	CHECK(strcmp(out, "ID?") == 0, "suffix with a glyph byte: '%s'", out);
	dstar_field_text(out, h.ur, sizeof h.ur);
	CHECK(strcmp(out, "CQCQCQ") == 0, "UR: '%s'", out);
	// The message: UTF-8, a text alternate's marker and a NUL inside; spaces inside stay, trailing ones go
	static const char msg[DSTAR_MSG_LEN] = "Ol\xC3\xA1 \x1d" "a\0b  \x7F     ";
	dstar_field_text(out, msg, DSTAR_MSG_LEN);
	CHECK(strcmp(out, "Ol?? ?a?b  ?") == 0, "message: '%s'", out);
	char blank[DSTAR_CALL_LEN];
	memset(blank, 0, sizeof blank);
	dstar_field_text(out, blank, sizeof blank);
	CHECK(out[0] == '\0', "an empty field");
	for (unsigned c = 0; c < 256u; c++) {
		char one = (char)c;
		dstar_field_text(out, &one, 1);
		bool kept = c > 0x20u && c <= 0x7Eu;
		CHECK(kept ? out[0] == (char)c && out[1] == '\0' :
			     c == 0x20u || c == 0 ? out[0] == '\0' : strcmp(out, "?") == 0, "byte %02X", c);
	}
}

int main(void)
{
	test_vectors();
	test_field_text();
	test_header_errors();
	test_slow_rx();
	test_stream();
	test_tx_silence();
	test_fuzz();
	printf("%s dstarTest: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
	return failures ? 1 : 0;
}
