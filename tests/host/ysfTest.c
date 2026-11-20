// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host tests for Yaesu System Fusion framing (core/ysf/ysf_fec.c, ysf_frame.c; docs/spec/ysf-modem.md 3, 4): the
 * CRC, whitening, Golay code, FICH, DCH units and V/D mode 2 voice against the vectors of tools/ysf_ref.py (an
 * independent reference written from Yaesu's published standard, tests/vectors/ysf.txt), the decoders against bit
 * errors, and whole transmissions from the transmitter's dibits into the receiver: header callsigns, voice, the
 * terminator, late entry from the DCH, inverted levels, V/D mode 1, a lost signal, noise and a fuzz loop.
 *
 *   make -C tests/host run-ysf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ysf/ysf_fec.h"
#include "ysf/ysf_frame.h"
#include "ysf/ysf_c4fm.h"
#include "fuzz.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#ifndef VECTORS
#define VECTORS "../vectors/ysf.txt"
#endif

static size_t unhex(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;
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

static void to_soft(const uint8_t *air, uint32_t nbits, int8_t strength, uint8_t *soft)
{
	for (uint32_t i = 0; i < nbits; i++) {
		ysf_soft_put(soft, i, ysf_bit(air, i) ? strength : (int8_t)-strength);
	}
}

// The reference's known voice frames: xorshift32 from n (tools/ysf_ref.py voice_pattern)
static void voice_pattern(uint32_t n, uint8_t bits[YSF_VOICE_BITS])
{
	uint32_t x = n * 2654435761u + 12345u;
	for (uint32_t i = 0; i < YSF_VOICE_BITS; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		bits[i] = (uint8_t)(x & 1u);
	}
}

static uint64_t rng = 0x9E3779B97F4A7C15ULL;

static uint32_t rnd(void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return (uint32_t)((rng * 0x2545F4914F6CDD1DULL) >> 32);
}

// --- the reference's vectors ---

typedef struct {
	uint32_t next, limit;
} src_t;

static bool pull_pattern(void *ctx, uint8_t bits[YSF_VOICE_BITS])
{
	src_t *s = ctx;
	if (s->limit && s->next >= s->limit) {
		return false;
	}
	voice_pattern(s->next++, bits);
	return true;
}

static void tx_config(ysf_tx_config_t *c, const char *src, const char *dest, src_t *s)
{
	memset(c, 0, sizeof(*c));
	ysf_csd_init(&c->csd, src, dest);
	c->headers = 1;
	c->order = YSF_WHITEN_DATA;
	c->pull = pull_pattern;
	c->ctx = s;
}

// The dibits of a transmission of `frames` V/D 2 frames
static uint32_t tx_stream(uint8_t *dib, uint32_t max, const char *src, const char *dest, uint32_t frames)
{
	ysf_tx_t tx;
	ysf_tx_config_t c;
	src_t s = { 0, 0 };
	tx_config(&c, src, dest, &s);
	ysf_tx_start(&tx, &c);
	uint32_t n = 0;
	int d;
	while ((d = ysf_tx_dibit(&tx)) >= 0 && n < max) {
		dib[n++] = (uint8_t)d;
		// Stop once the voice frames asked for have started (the frame in progress finishes)
		if (tx.state == YSF_TX_VOICE && tx.frames >= 1u + frames && !tx.stop) {
			ysf_tx_stop(&tx);
		}
	}
	return n;
}

// The vectors' "yaesu" over: what a Yaesu radio sends (tools/ysf_ref.py yaesu_transmission), for test_yaesu
static uint8_t yaesu_dib[16384];
static uint32_t yaesu_n, yaesu_preamble, yaesu_frames;

static void test_vectors(void)
{
	FILE *f = fopen(VECTORS, "r");
	CHECK(f != NULL, "no %s", VECTORS);
	if (!f) {
		return;
	}
	static char line[16384];
	int n_crc = 0, n_pn = 0, n_golay = 0, n_fich = 0, n_dch = 0, n_vd2 = 0, n_tx = 0, n_yaesu = 0;
	while (fgets(line, sizeof(line), f)) {
		static char kind[16], a[256], b[256], c[16384];
		if (line[0] == '#') {
			continue;
		}
		int got = sscanf(line, "%15s %255s %255s %16383[^\n]", kind, a, b, c);
		if (got < 2) {
			continue;
		}
		static uint8_t x[2048], y[2048];
		if (strcmp(kind, "crc") == 0) {
			size_t n = unhex(a, x, sizeof(x));
			unsigned want;
			sscanf(b, "%x", &want);
			CHECK(ysf_crc(x, (uint32_t)n * 8u) == want, "crc %s: %04X, want %04X", a, ysf_crc(x, (uint32_t)n * 8u), want);
			n_crc++;
		} else if (strcmp(kind, "pn") == 0) {
			size_t n = unhex(a, x, sizeof(x));
			for (uint32_t i = 0; i < 511u && i / 8u < n; i++) {
				CHECK(ysf_pn_bit(i) == ysf_bit(x, i), "pn bit %u", i);
				CHECK(ysf_pn_bit(i + 511u) == ysf_bit(x, i), "pn period at %u", i);
			}
			memset(y, 0, 64);
			ysf_whiten(y, 511);
			CHECK(memcmp(y, x, 63) == 0, "whiten zeros gives the sequence");
			n_pn++;
		} else if (strcmp(kind, "golay") == 0) {
			unsigned d, cw;
			sscanf(a, "%x", &d);
			sscanf(b, "%x", &cw);
			CHECK(ysf_golay_encode((uint16_t)d) == cw, "golay %03X: %06X, want %06X", d,
			      (unsigned)ysf_golay_encode((uint16_t)d), cw);
			uint16_t v;
			CHECK(ysf_golay_decode(cw, &v) == 0 && v == d, "golay decode clean %03X", d);
			n_golay++;
		} else if (strcmp(kind, "fich") == 0) {
			unsigned v;
			sscanf(a, "%x", &v);
			CHECK(unhex(b, y, sizeof(y)) == YSF_FICH_BITS / 8u, "fich length");
			uint8_t air[YSF_FICH_BITS / 8u];
			ysf_fich_encode(v, air);
			CHECK(memcmp(air, y, sizeof(air)) == 0, "fich encode %08X differs from the reference", v);
			uint8_t soft[YSF_FICH_BITS / 2u];
			to_soft(y, YSF_FICH_BITS, 4, soft);
			uint32_t back;
			ysf_fich_result_t r = ysf_fich_decode(soft, &back);
			CHECK(r.crc_ok && r.golay == 0 && r.corrected == 0 && back == v, "fich decode %08X", v);
			// The fields round trip (bit 15 is reserved: not kept)
			ysf_fich_t fs;
			ysf_fich_unpack(v, &fs);
			CHECK(ysf_fich_pack(&fs) == (v & ~0x8000u), "fich fields %08X", v);
			n_fich++;
		} else if (strcmp(kind, "dch") == 0) {
			ysf_whiten_t order = strcmp(a, "all") == 0 ? YSF_WHITEN_ALL : YSF_WHITEN_DATA;
			size_t n = unhex(b, x, sizeof(x));
			size_t m = unhex(c, y, sizeof(y));
			uint32_t bits = n == YSF_DCH_BYTES_SHORT ? YSF_DCH_BITS_SHORT : YSF_DCH_BITS_LONG;
			CHECK(m == bits / 8u, "dch length");
			uint8_t air[YSF_DCH_BITS_LONG / 8u], soft[YSF_DCH_BITS_LONG / 2u], back[YSF_DCH_BYTES_LONG];
			memset(air, 0, sizeof(air));
			ysf_dch_encode(x, (uint32_t)n, order, air);
			CHECK(memcmp(air, y, m) == 0, "dch %s encode %s differs from the reference", a, b);
			to_soft(y, bits, 4, soft);
			ysf_unit_result_t r = ysf_dch_decode(soft, (uint32_t)n, back);
			CHECK(r.crc_ok && r.order == order && r.corrected == 0 && memcmp(back, x, n) == 0, "dch decode %s %s",
			      a, b);
			n_dch++;
		} else if (strcmp(kind, "vd2") == 0) {
			unhex(a, x, sizeof(x));
			CHECK(unhex(b, y, sizeof(y)) == YSF_VD2_VOICE_BITS / 8u, "vd2 length");
			uint8_t bits[YSF_VOICE_BITS], air[YSF_VD2_VOICE_BITS / 8u], soft[YSF_VD2_VOICE_BITS / 2u];
			for (uint32_t i = 0; i < YSF_VOICE_BITS; i++) {
				bits[i] = ysf_bit(x, i) ? 1u : 0u;
			}
			ysf_vd2_voice_encode(bits, air);
			CHECK(memcmp(air, y, sizeof(air)) == 0, "vd2 encode %s differs from the reference", a);
			uint8_t back[YSF_VOICE_BITS];
			to_soft(y, YSF_VD2_VOICE_BITS, 4, soft);
			CHECK(ysf_vd2_voice_decode(soft, back) == 0 && memcmp(back, bits, sizeof(back)) == 0, "vd2 decode %s", a);
			n_vd2++;
		} else if (strcmp(kind, "tx") == 0) {
			// tx <src> <dest> <headers> <frames> <bits>
			char hdrs[16], frames[16], hex[16384];
			if (sscanf(c, "%15s %15s %16383s", hdrs, frames, hex) != 3) {
				CHECK(false, "tx line");
				continue;
			}
			static uint8_t want[8192], dib[16384];
			size_t m = unhex(hex, want, sizeof(want));
			uint32_t n = tx_stream(dib, sizeof(dib), a, b, (uint32_t)atoi(frames));
			CHECK(n * 2u == m * 8u, "tx length %u dibits, want %u", n, (unsigned)(m * 4u));
			uint32_t bad = 0, first = 0;
			for (uint32_t i = 0; i < n && i * 2u < m * 8u; i++) {
				uint8_t w = (uint8_t)((ysf_bit(want, 2u * i) ? 2u : 0u) | (ysf_bit(want, 2u * i + 1u) ? 1u : 0u));
				if (w != dib[i] && bad++ == 0) {
					first = i;
				}
			}
			CHECK(bad == 0, "tx stream: %u dibits differ from the reference, the first at %u (frame %u)", bad, first,
			      first / YSF_FRAME_DIBITS);
			n_tx++;
		} else if (strcmp(kind, "yaesu") == 0) {
			// yaesu <src> <radio ID> <preamble> <frames> <bits>
			char pre[16], frames[16], hex[16384];
			static uint8_t bits[8192];
			if (sscanf(c, "%15s %15s %16383s", pre, frames, hex) != 3) {
				CHECK(false, "yaesu line");
				continue;
			}
			size_t m = unhex(hex, bits, sizeof(bits));
			yaesu_n = (uint32_t)(m * 4u);
			for (uint32_t i = 0; i < yaesu_n && i < sizeof(yaesu_dib); i++) {
				yaesu_dib[i] = (uint8_t)((ysf_bit(bits, 2u * i) ? 2u : 0u) | (ysf_bit(bits, 2u * i + 1u) ? 1u : 0u));
			}
			yaesu_preamble = (uint32_t)atoi(pre);
			yaesu_frames = (uint32_t)atoi(frames);
			n_yaesu++;
		}
	}
	fclose(f);
	CHECK(n_crc >= 3 && n_pn == 1 && n_golay >= 6 && n_fich >= 6 && n_dch >= 8 && n_vd2 >= 4 && n_tx == 1 &&
	      n_yaesu == 1, "vectors read: crc %d pn %d golay %d fich %d dch %d vd2 %d tx %d yaesu %d", n_crc, n_pn,
	      n_golay, n_fich, n_dch, n_vd2, n_tx, n_yaesu);
}

// --- the decoders against errors ---

static void test_golay(void)
{
	uint32_t wrong = 0, missed4 = 0;
	for (int i = 0; i < 40000; i++) {
		uint16_t d = (uint16_t)(rnd() & 0xFFFu);
		uint32_t cw = ysf_golay_encode(d), e = 0;
		int ne = i % 5;	// 0..4 errors
		while (__builtin_popcount(e) < ne) {
			e |= 1u << (rnd() % 24u);
		}
		uint16_t got;
		int r = ysf_golay_decode(cw ^ e, &got);
		if (ne <= 3) {
			wrong += (uint32_t)(r != ne || got != d);
		} else {
			missed4 += (uint32_t)(r >= 0);	// distance 8: four errors are always detected
		}
	}
	CHECK(wrong == 0, "golay: %u words with three errors or fewer decoded wrong", wrong);
	CHECK(missed4 == 0, "golay: %u words with four errors not detected", missed4);
}

static void test_fich_errors(void)
{
	// Random errors in the 200 coded bits at full confidence (the worst kind for a soft decoder): 12 of them still
	// decode about 96 % of the time (386 of 400 when written)
	uint32_t ok = 0, false_ok = 0;
	for (int i = 0; i < 400; i++) {
		uint32_t v = rnd() & ~0x8000u;
		uint8_t air[YSF_FICH_BITS / 8u], soft[YSF_FICH_BITS / 2u];
		ysf_fich_encode(v, air);
		to_soft(air, YSF_FICH_BITS, 4, soft);
		for (int e = 0; e < 12; e++) {
			uint32_t at = rnd() % YSF_FICH_BITS;
			ysf_soft_put(soft, at, (int8_t)-ysf_soft_get(soft, at));
		}
		uint32_t back;
		ysf_fich_result_t r = ysf_fich_decode(soft, &back);
		ok += (uint32_t)(r.crc_ok && back == v);
		false_ok += (uint32_t)(r.crc_ok && back != v);
	}
	CHECK(ok >= 370, "fich with 12 random errors: %u of 400 decoded", ok);
	CHECK(false_ok == 0, "fich: %u wrong values passed the CRC", false_ok);
	// A 5 ms burst (48 bits) is spread by the interleaver across the code
	uint8_t air[YSF_FICH_BITS / 8u], soft[YSF_FICH_BITS / 2u];
	ysf_fich_encode(0x601D022Au, air);
	to_soft(air, YSF_FICH_BITS, 4, soft);
	for (uint32_t i = 60; i < 84; i++) {
		ysf_soft_put(soft, i, 0);	// erased, as a fade gives
	}
	uint32_t back;
	ysf_fich_result_t r = ysf_fich_decode(soft, &back);
	CHECK(r.crc_ok && back == 0x601D022Au, "fich with 24 erased bits");
	// Noise alone never passes
	uint32_t noise_ok = 0;
	for (int i = 0; i < 2000; i++) {
		for (uint32_t k = 0; k < YSF_FICH_BITS; k++) {
			ysf_soft_put(soft, k, (int8_t)((int)(rnd() % 15u) - 7));
		}
		noise_ok += (uint32_t)ysf_fich_decode(soft, &back).crc_ok;
	}
	CHECK(noise_ok <= 1, "fich: %u of 2000 noise blocks passed the CRC", noise_ok);
}

static void test_dch_errors(void)
{
	const uint8_t d20[] = "N0CALL    G4XYZ     ";
	uint8_t air[YSF_DCH_BITS_LONG / 8u], soft[YSF_DCH_BITS_LONG / 2u], back[YSF_DCH_BYTES_LONG];
	uint32_t ok20 = 0, ok10 = 0;
	// Random full-confidence errors, about 5.5 % of the coded bits: 159 and 182 of 200 decoded when written
	for (int i = 0; i < 200; i++) {
		ysf_dch_encode(d20, 20, YSF_WHITEN_ALL, air);
		to_soft(air, YSF_DCH_BITS_LONG, 4, soft);
		for (int e = 0; e < 20; e++) {
			uint32_t at = rnd() % YSF_DCH_BITS_LONG;
			ysf_soft_put(soft, at, (int8_t)-ysf_soft_get(soft, at));
		}
		ysf_unit_result_t r = ysf_dch_decode(soft, 20, back);
		ok20 += (uint32_t)(r.crc_ok && r.order == YSF_WHITEN_ALL && memcmp(back, d20, 20) == 0);
		ysf_dch_encode(d20, 10, YSF_WHITEN_DATA, air);
		to_soft(air, YSF_DCH_BITS_SHORT, 4, soft);
		for (int e = 0; e < 11; e++) {
			uint32_t at = rnd() % YSF_DCH_BITS_SHORT;
			ysf_soft_put(soft, at, (int8_t)-ysf_soft_get(soft, at));
		}
		r = ysf_dch_decode(soft, 10, back);
		ok10 += (uint32_t)(r.crc_ok && r.order == YSF_WHITEN_DATA && memcmp(back, d20, 10) == 0);
	}
	CHECK(ok20 >= 140, "dch 20 bytes with 20 random errors: %u of 200", ok20);
	CHECK(ok10 >= 170, "dch 10 bytes with 11 random errors: %u of 200", ok10);
}

static void test_vd2_errors(void)
{
	// One error in every protected triple still decodes; the 22 unprotected bits take what comes
	uint8_t bits[YSF_VOICE_BITS], back[YSF_VOICE_BITS], air[YSF_VD2_VOICE_BITS / 8u], soft[YSF_VD2_VOICE_BITS / 2u];
	voice_pattern(99, bits);
	ysf_vd2_voice_encode(bits, air);
	to_soft(air, YSF_VD2_VOICE_BITS, 3, soft);
	// Flip copy i % 3 of triple i: find each S index's air bit by encoding a single-bit difference
	for (uint32_t i = 0; i < 27u; i++) {
		uint32_t j = 3u * i + i % 3u;
		uint32_t k = (j % 26u) * 4u + j / 26u;
		ysf_soft_put(soft, k, (int8_t)-ysf_soft_get(soft, k));
	}
	uint8_t q = ysf_vd2_voice_decode(soft, back);
	CHECK(memcmp(back, bits, 27) == 0 && memcmp(back + 27, bits + 27, 22) == 0, "vd2 with one error a triple");
	CHECK(q == 27, "vd2 quality %u, want 27", q);
}

static void test_callsigns(void)
{
	char cs[YSF_CALLSIGN_LEN], out[YSF_CALLSIGN_LEN + 1];
	ysf_callsign_set(cs, "g4xyz/p");
	ysf_callsign_get(cs, out);
	CHECK(strcmp(out, "G4XYZ/P") == 0, "callsign case: '%s'", out);
	ysf_callsign_set(cs, "LONGERTHANTEN");
	CHECK(memcmp(cs, "LONGERTHAN", 10) == 0, "callsign cut to 10");
	const char bad[YSF_CALLSIGN_LEN] = { 'A', '\n', 'B', (char)0xFF, ' ', ' ', ' ', ' ', ' ', ' ' };
	ysf_callsign_get(bad, out);
	CHECK(strcmp(out, "A B ") == 0 || strcmp(out, "A B") == 0, "callsign from air sanitized: '%s'", out);
}

// --- whole transmissions ---

typedef struct {
	uint32_t frames, firsts, calls, voices, voice_bad, voice72s, ends, losts;
	uint32_t next;		// the voice frame expected next
	bool counting;
	ysf_call_t last;
	uint8_t dt;
	uint8_t v72[9];
} rx_result_t;

static void on_frame(void *ctx, const ysf_call_t *call, bool first)
{
	rx_result_t *r = ctx;
	r->frames++;
	r->dt = call->fich.dt;
	if (first) {
		r->firsts++;
		r->counting = call->fich.fi == YSF_FI_HEADER;
		r->next = 0;
	}
}

static void on_call(void *ctx, const ysf_call_t *call)
{
	rx_result_t *r = ctx;
	r->calls++;
	r->last = *call;
}

static void on_voice(void *ctx, const uint8_t bits[YSF_VOICE_BITS], uint8_t q)
{
	rx_result_t *r = ctx;
	r->voices++;
	if (!r->counting) {
		return;
	}
	uint8_t want[YSF_VOICE_BITS];
	voice_pattern(r->next++, want);
	r->voice_bad += (uint32_t)(memcmp(want, bits, sizeof(want)) != 0);
}

static void on_voice72(void *ctx, const uint8_t f[9])
{
	rx_result_t *r = ctx;
	r->voice72s++;
	memcpy(r->v72, f, 9);
}

static void on_end(void *ctx, ysf_end_t why)
{
	rx_result_t *r = ctx;
	if (why == YSF_END_TERMINATOR) {
		r->ends++;
	} else {
		r->losts++;
	}
	r->counting = false;
}

static void rx_setup(ysf_rx_t *rx, rx_result_t *res)
{
	memset(res, 0, sizeof(*res));
	ysf_rx_cb_t cb = { on_frame, on_call, on_voice, on_voice72, on_end, res };
	ysf_rx_init(rx, &cb);
}

// Feed dibits as soft symbols of the levels (unit 1000), optionally inverted
static void feed(ysf_rx_t *rx, const uint8_t *dib, uint32_t n, bool invert)
{
	for (uint32_t i = 0; i < n; i++) {
		int lv = ysf_c4fm_level(dib[i]) * 1000;
		ysf_rx_symbol(rx, (int16_t)(invert ? -lv : lv), 1000);
	}
}

static void noise(ysf_rx_t *rx, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		ysf_rx_symbol(rx, (int16_t)((int)(rnd() % 6001u) - 3000), 1000);
	}
}

static bool call_is(const ysf_call_t *c, const char *src, const char *dest)
{
	char s[11], d[11];
	ysf_callsign_get(c->csd.src, s);
	ysf_callsign_get(c->csd.dest, d);
	return strcmp(s, src) == 0 && strcmp(d, dest) == 0;
}

static void test_stream(void)
{
	static uint8_t dib[YSF_FRAME_DIBITS * 40];
	ysf_rx_t rx;
	rx_result_t res;
	uint32_t n = tx_stream(dib, sizeof(dib), "G4XYZ", "ALL", 13);	// 1 header, 13 V/D 2 frames, the terminator
	CHECK(n == YSF_FRAME_DIBITS * 15u, "stream length %u", n);

	// Clean, after some noise
	rx_setup(&rx, &res);
	noise(&rx, 1000);
	feed(&rx, dib, n, false);
	noise(&rx, 2000);
	CHECK(res.firsts == 1 && res.frames == 15, "clean: %u transmissions, %u frames", res.firsts, res.frames);
	CHECK(res.voices == 65 && res.voice_bad == 0, "clean: %u voice frames, %u wrong", res.voices, res.voice_bad);
	CHECK(res.ends == 1 && res.losts == 0, "clean: end %u lost %u", res.ends, res.losts);
	CHECK(call_is(&res.last, "G4XYZ", "ALL"), "clean: callsigns");
	CHECK((res.last.have & (YSF_HAVE_SRC | YSF_HAVE_DEST)) == (YSF_HAVE_SRC | YSF_HAVE_DEST), "clean: have");
	CHECK(res.dt == YSF_DT_VD2, "clean: mode DN");
	CHECK(rx.fich_bad == 0 && rx.csd_bad == 0 && rx.sync_errors == 0, "clean: fich bad %u csd bad %u sync errors %u",
	      rx.fich_bad, rx.csd_bad, rx.sync_errors);
	CHECK(rx.order_all == 0, "clean: the transmitter's whitening order is the figure's");

	// Inverted levels: found by the sync, decoded the same
	rx_setup(&rx, &res);
	noise(&rx, 777);
	feed(&rx, dib, n, true);
	CHECK(res.voices == 65 && res.voice_bad == 0 && res.ends == 1 && rx.invert, "inverted: %u voices %u bad",
	      res.voices, res.voice_bad);

	// Late entry: from the middle of frame 4; the callsigns come from the DCH (FN 0 dest, FN 1 src)
	rx_setup(&rx, &res);
	feed(&rx, dib + YSF_FRAME_DIBITS * 4u + 123u, n - YSF_FRAME_DIBITS * 4u - 123u, false);
	CHECK(res.firsts == 1 && rx.late_syncs == 1, "late: %u transmissions, late %u", res.firsts, rx.late_syncs);
	CHECK(call_is(&res.last, "G4XYZ", "ALL"), "late: callsigns from the DCH");
	CHECK(res.ends == 1, "late: the terminator");

	// A burst of errors over one frame's FICH: the frame carries on by its place in the rolling pattern
	rx_setup(&rx, &res);
	static uint8_t hit[sizeof(dib)];
	memcpy(hit, dib, n);
	for (uint32_t i = 0; i < 60; i++) {
		hit[YSF_FRAME_DIBITS * 6u + YSF_SYNC_DIBITS + 10u + i] ^= 3u;
	}
	feed(&rx, hit, n, false);
	CHECK(rx.fich_bad == 1 && res.voices == 65 && res.voice_bad == 0 && res.ends == 1,
	      "fich burst: bad %u, voices %u wrong %u", rx.fich_bad, res.voices, res.voice_bad);

	// Lost: the signal stops mid-call; four missed syncs end it
	rx_setup(&rx, &res);
	feed(&rx, dib, YSF_FRAME_DIBITS * 7u + 200u, false);
	noise(&rx, YSF_FRAME_DIBITS * 6u);
	CHECK(res.losts == 1 && res.ends == 0, "lost: %u, ended %u", res.losts, res.ends);

	// Back to back: two transmissions, each found
	rx_setup(&rx, &res);
	feed(&rx, dib, n, false);
	feed(&rx, dib, n, false);
	CHECK(res.firsts == 2 && res.ends == 2 && res.voice_bad == 0, "back to back: %u, ends %u", res.firsts, res.ends);

	// Hard dibits (a chip's own receiver)
	rx_setup(&rx, &res);
	for (uint32_t i = 0; i < n; i++) {
		ysf_rx_dibit(&rx, dib[i]);
	}
	CHECK(res.voices == 65 && res.voice_bad == 0 && res.ends == 1, "hard dibits: %u voices", res.voices);
}

// A Yaesu radio's DN over as an FT-5D sends it (ysf-modem.md 3.5; the vectors' "yaesu" line): its preamble, one
// header, radio ID mode, FT 6 with data at FN 6. The receiver must take the callsigns and the radio ID, play every
// voice frame, and keep the data frame out of the callsign fields, also when that frame's FICH is lost and its place
// in the rolling pattern has to be assumed (FN 6 after FN 5, from the FT the frames carry, not FN 0)
static void yaesu_check(const char *what, const uint8_t *dib, uint32_t n, bool hard, uint32_t want_bad)
{
	ysf_rx_t rx;
	rx_result_t res;
	rx_setup(&rx, &res);
	noise(&rx, 500);
	if (hard) {
		for (uint32_t i = 0; i < n; i++) {
			ysf_rx_dibit(&rx, dib[i]);
		}
	} else {
		feed(&rx, dib, n, false);
	}
	noise(&rx, 500);
	uint32_t frames = yaesu_frames + 2u - want_bad;	// a frame whose FICH failed is carried on, not announced
	CHECK(res.firsts == 1 && rx.late_syncs == 0 && res.frames == frames, "%s: %u transmissions (late %u), %u frames",
	      what, res.firsts, rx.late_syncs, res.frames);
	CHECK(rx.fich_bad == want_bad, "%s: %u FICH bad, want %u", what, rx.fich_bad, want_bad);
	CHECK(res.voices == 5u * yaesu_frames && res.voice_bad == 0 && res.ends == 1, "%s: %u voices, %u wrong, end %u",
	      what, res.voices, res.voice_bad, res.ends);
	const ysf_call_t *c = &res.last;
	CHECK(c->fich.cm == YSF_CM_RADIO_ID && c->fich.ft == 6, "%s: CM %u FT %u", what, c->fich.cm, c->fich.ft);
	CHECK(memcmp(c->csd.dest, "*****E0123", 10) == 0 && memcmp(c->csd.src, "N0CALL    ", 10) == 0,
	      "%s: dest '%.10s' src '%.10s'", what, c->csd.dest, c->csd.src);
	CHECK(memcmp(c->csd.rem[3], "E0123", 5) == 0 && memcmp(c->csd.rem[0], "     ", 5) == 0, "%s: Rem4 '%.5s'", what,
	      c->csd.rem[3]);
	char shown[11];
	ysf_dest_show(c->fich.cm, c->csd.dest, shown);
	CHECK(strcmp(shown, "ALL") == 0, "%s: destination shown as '%s'", what, shown);
}

static void test_yaesu(void)
{
	CHECK(yaesu_n == yaesu_preamble + YSF_FRAME_DIBITS * (yaesu_frames + 2u), "yaesu: %u dibits", yaesu_n);
	yaesu_check("yaesu", yaesu_dib, yaesu_n, false, 0);
	yaesu_check("yaesu hard", yaesu_dib, yaesu_n, true, 0);
	// FN 6's FICH lost (the eighth frame: header, FN 0-6): its data unit has a good CRC, so only the frame number
	// the receiver assumes keeps it out of the destination
	static uint8_t hit[sizeof(yaesu_dib)];
	memcpy(hit, yaesu_dib, yaesu_n);
	uint32_t at6 = yaesu_preamble + YSF_FRAME_DIBITS * 7u + YSF_SYNC_DIBITS;
	for (uint32_t i = 0; i < 60; i++) {
		hit[at6 + 10u + i] ^= 3u;
	}
	yaesu_check("yaesu FN 6 FICH lost", hit, yaesu_n, false, 1);

	// ysf_dest_show: radio ID mode to one station shows its ID; group mode the field itself
	char out[11];
	ysf_dest_show(YSF_CM_RADIO_ID, "E0456E0123", out);
	CHECK(strcmp(out, "E0456") == 0, "dest shown '%s'", out);
	ysf_dest_show(YSF_CM_GROUP, "*****E0123", out);
	CHECK(strcmp(out, "*****E0123") == 0, "group dest shown '%s'", out);
	ysf_dest_show(YSF_CM_GROUP, "ALL       ", out);
	CHECK(strcmp(out, "ALL") == 0, "group ALL shown '%s'", out);
}

// Our transmission in radio ID mode with the preamble (what radio_ysf.c sends to everyone when it has a radio ID):
// the preamble alternates -3 / +3 into the sync, the destination is "*****" and the ID, and it decodes
static void test_preamble_radio_id(void)
{
	ysf_tx_t tx;
	ysf_tx_config_t c;
	src_t s = { 0, 0 };
	tx_config(&c, "N0CALL", "ALL", &s);
	memcpy(c.csd.rem[3], "E0123", 5);
	ysf_csd_radio_id(&c.csd, YSF_RADIO_ID_ALL);
	c.fich.cm = YSF_CM_RADIO_ID;
	c.preamble = 239;	// odd: rounded up, so the last symbol is +3 and the alternation runs into the sync's -3
	ysf_tx_start(&tx, &c);
	static uint8_t dib[YSF_FRAME_DIBITS * 12];
	uint32_t n = 0;
	int d;
	while ((d = ysf_tx_dibit(&tx)) >= 0 && n < sizeof(dib)) {
		dib[n++] = (uint8_t)d;
		if (tx.state == YSF_TX_VOICE && tx.frames >= 8u && !tx.stop) {
			ysf_tx_stop(&tx);
		}
	}
	uint32_t alt = 0;
	for (uint32_t i = 0; i < 240u; i++) {
		alt += (uint32_t)(ysf_c4fm_level(dib[i]) == ((i & 1u) ? 3 : -3));
	}
	CHECK(alt == 240u, "preamble: %u of 240 symbols alternate -3 / +3", alt);
	uint64_t sync = 0;
	for (uint32_t i = 240u; i < 260u; i++) {
		sync = (sync << 2) | dib[i];
	}
	CHECK(sync == YSF_SYNC && n == 240u + YSF_FRAME_DIBITS * 9u, "preamble: the sync follows (%u dibits)", n);
	ysf_rx_t rx;
	rx_result_t res;
	rx_setup(&rx, &res);
	feed(&rx, dib, n, false);
	CHECK(res.firsts == 1 && rx.late_syncs == 0 && rx.syncs == 1 && res.voices == 35 && res.voice_bad == 0 &&
	      res.ends == 1, "preamble: %u syncs, %u transmissions, %u voices %u wrong", rx.syncs, res.firsts, res.voices,
	      res.voice_bad);
	CHECK(res.last.fich.cm == YSF_CM_RADIO_ID && memcmp(res.last.csd.dest, "*****E0123", 10) == 0 &&
	      memcmp(res.last.csd.rem[3], "E0123", 5) == 0, "radio ID mode: CM %u dest '%.10s'", res.last.fich.cm,
	      res.last.csd.dest);
}

static void test_noise(void)
{
	// Ten minutes of noise: syncs within three bits turn up now and then, a FICH never passes
	ysf_rx_t rx;
	rx_result_t res;
	rx_setup(&rx, &res);
	noise(&rx, 4800u * 600u);
	CHECK(res.firsts == 0 && res.voices == 0, "noise: %u transmissions, %u voices (%u syncs)", res.firsts,
	      res.voices, rx.syncs);
	printf("noise: %u false syncs in ten minutes, none with a FICH\n", rx.syncs);
}

static void test_vd1(void)
{
	// A V/D mode 1 frame built from the standard's layout: FICH dt 00, five DCH pieces of the CSD1 unit and five
	// 72-bit codec frames
	uint8_t frame[YSF_FRAME_DIBITS / 4u];
	memset(frame, 0, sizeof(frame));
	for (uint32_t i = 0; i < 40u; i++) {
		ysf_bit_put(frame, i, ((YSF_SYNC >> (39u - i)) & 1u) != 0);
	}
	ysf_fich_t f = { .fi = YSF_FI_COMM, .cs = 2, .fn = 0, .ft = 2, .dt = YSF_DT_VD1 };
	uint8_t air[YSF_DCH_BITS_LONG / 8u];
	ysf_fich_encode(ysf_fich_pack(&f), air);
	for (uint32_t i = 0; i < YSF_FICH_BITS; i++) {
		ysf_bit_put(frame, 40u + i, ysf_bit(air, i));
	}
	ysf_dch_encode((const uint8_t *)"CQ        M0ABC     ", 20, YSF_WHITEN_DATA, air);
	const uint8_t v72[9] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x0F };
	for (uint32_t s = 0; s < 5u; s++) {
		for (uint32_t i = 0; i < 72u; i++) {
			ysf_bit_put(frame, 240u + 144u * s + i, ysf_bit(air, 72u * s + i));
			ysf_bit_put(frame, 240u + 144u * s + 72u + i, ysf_bit(v72, i));
		}
	}
	uint8_t dib[YSF_FRAME_DIBITS + YSF_SYNC_DIBITS];
	for (uint32_t i = 0; i < YSF_FRAME_DIBITS; i++) {
		dib[i] = (uint8_t)((ysf_bit(frame, 2u * i) ? 2u : 0u) | (ysf_bit(frame, 2u * i + 1u) ? 1u : 0u));
	}
	ysf_rx_t rx;
	rx_result_t res;
	rx_setup(&rx, &res);
	feed(&rx, dib, YSF_FRAME_DIBITS, false);
	CHECK(res.firsts == 1 && res.dt == YSF_DT_VD1, "vd1: frame");
	CHECK(res.voice72s == 5 && memcmp(res.v72, v72, 9) == 0, "vd1: %u codec frames", res.voice72s);
	CHECK(call_is(&res.last, "M0ABC", "CQ"), "vd1: CSD1 from FN 0");
}

static void test_fuzz(void)
{
	// Random symbols with frames of a real transmission spliced in at random places: no crash, the counters agree
	static uint8_t dib[YSF_FRAME_DIBITS * 20];
	uint32_t n = tx_stream(dib, sizeof(dib), "N0CALL", "ALL", 6);
	fuzz_t fz;
	fuzz_init(&fz, 0x5EED);
	ysf_rx_t rx;
	rx_result_t res;
	rx_setup(&rx, &res);
	for (long it = 0; it < fuzz_iters(3000); it++) {
		uint32_t kind = fuzz_below(&fz, 4);
		if (kind == 0) {
			uint32_t at = fuzz_below(&fz, n), len = fuzz_below(&fz, n - at) + 1u;
			feed(&rx, dib + at, len, fuzz_below(&fz, 2) != 0);
		} else {
			uint32_t len = fuzz_below(&fz, 700);
			for (uint32_t i = 0; i < len; i++) {
				ysf_rx_symbol(&rx, (int16_t)((int)fuzz_below(&fz, 65536) - 32768), (int16_t)fuzz_below(&fz, 4000));
			}
		}
		CHECK(rx.state <= YSF_RX_PAYLOAD && rx.pos <= YSF_PAYLOAD_DIBITS, "fuzz: state %u pos %u", rx.state, rx.pos);
	}
	CHECK(res.ends + res.losts <= res.firsts && res.firsts <= res.ends + res.losts + 1u,
	      "fuzz: %u started, %u ended, %u lost", res.firsts, res.ends, res.losts);
}

int main(void)
{
	test_vectors();
	test_golay();
	test_fich_errors();
	test_dch_errors();
	test_vd2_errors();
	test_callsigns();
	test_stream();
	test_yaesu();
	test_preamble_radio_id();
	test_noise();
	test_vd1();
	test_fuzz();
	printf("%s ysfTest: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
	return failures ? 1 : 0;
}
