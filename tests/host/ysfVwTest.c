// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host tests for Yaesu System Fusion Voice FR (core/ysf/ysf_vw.c, docs/spec/ysf-modem.md 4.1): the full-rate frame's
 * coding against tools/ysf_vw_ref.py's vectors (tests/vectors/ysf-vw.txt, written apart from the C), its FEC against
 * errors, a whole VW transmission bit for bit, and VW transmissions from our transmitter's dibits into the receiver:
 * the FICH's DT and FN / FT, CSD3, every voice frame in order, late entry, inverted levels, a damaged FICH, errors on
 * the air, DN and VW back to back. Then the codec on top (core/mbe): speech through our encoder, VW frames, the air with
 * errors and the receiver into the decoder, against the same speech decoded without the air.
 *
 *   make -C tests/host run-ysfvw
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ysf/ysf_fec.h"
#include "ysf/ysf_frame.h"
#include "ysf/ysf_vw.h"
#include "mbe/imbe.h"
#include "mbe/mbe_est.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#ifndef VECTORS
#define VECTORS "../vectors/ysf-vw.txt"
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

static uint64_t rng = 0x9E3779B97F4A7C15ULL;

static uint32_t rnd(void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return (uint32_t)((rng * 0x2545F4914F6CDD1DULL) >> 32);
}

// The reference's known frames: xorshift32 from n (tools/ysf_vw_ref.py imbe_pattern)
static void imbe_pattern(uint32_t n, uint8_t imbe[YSF_VW_IMBE_BYTES])
{
	uint32_t x = n * 2654435761u + 54321u;
	memset(imbe, 0, YSF_VW_IMBE_BYTES);
	for (uint32_t i = 0; i < 88u; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		ysf_bit_put(imbe, i, (x & 1u) != 0);
	}
}

// Where each code word's bits sit in the 144: found by encoding single bits of the information through the frame
// with the scrambling seed fixed (u0 = 0 changes only c0 itself), so the test needs no copy of the interleave table
static int word_of_bit[YSF_VW_FRAME_BITS];

static void find_words(void)
{
	// u0 = 0 and the rest zero: then flip one information bit of word k (k >= 1) and see which air bits move. The
	// masks don't change (u0 is fixed), so the moved bits are word k's code bits that depend on that data bit
	static const uint8_t first[8] = { 0, 12, 24, 36, 48, 59, 70, 81 };
	static const uint8_t len[8] = { 12, 12, 12, 12, 11, 11, 11, 7 };
	uint8_t zero[YSF_VW_IMBE_BYTES] = { 0 }, base[18], air[18];
	for (int i = 0; i < (int)YSF_VW_FRAME_BITS; i++) {
		word_of_bit[i] = -1;
	}
	memset(base, 0, sizeof base);
	ysf_vw_frame_encode(zero, base, 0);
	for (int k = 1; k < 8; k++) {
		for (int b = 0; b < len[k]; b++) {
			uint8_t imbe[YSF_VW_IMBE_BYTES] = { 0 };
			ysf_bit_put(imbe, (uint32_t)(first[k] + b), true);
			memset(air, 0, sizeof air);
			ysf_vw_frame_encode(imbe, air, 0);
			for (int i = 0; i < (int)YSF_VW_FRAME_BITS; i++) {
				if (ysf_bit(air, (uint32_t)i) != ysf_bit(base, (uint32_t)i)) {
					word_of_bit[i] = k;
				}
			}
		}
	}
	for (int i = 0; i < (int)YSF_VW_FRAME_BITS; i++) {
		if (word_of_bit[i] < 0) {
			word_of_bit[i] = 0;	// what is left is u0's Golay word
		}
	}
}

// The transmitter's voice source: imbe_pattern(0), (1), ... in turn
static uint32_t pull_n;

static bool pull(void *c, uint8_t imbe[YSF_VW_IMBE_BYTES])
{
	(void)c;
	imbe_pattern(pull_n++, imbe);
	return true;
}

// --- the reference's vectors ---

static void test_vectors(void)
{
	FILE *f = fopen(VECTORS, "r");
	CHECK(f != NULL, "open %s", VECTORS);
	if (f == NULL) {
		return;
	}
	char line[20000];
	int frames = 0, txs = 0;
	while (fgets(line, sizeof line, f) != NULL) {
		char a[64], b[64];
		if (sscanf(line, "frame %63s %63s", a, b) == 2) {
			uint8_t imbe[YSF_VW_IMBE_BYTES], want[18], air[18], back[YSF_VW_IMBE_BYTES];
			unhex(a, imbe, sizeof imbe);
			unhex(b, want, sizeof want);
			memset(air, 0, sizeof air);
			ysf_vw_frame_encode(imbe, air, 0);
			CHECK(memcmp(air, want, sizeof air) == 0, "frame %s encodes as the reference", a);
			uint8_t e = ysf_vw_frame_decode(want, 0, back);
			CHECK(memcmp(back, imbe, sizeof back) == 0 && e == 0, "frame %s decodes (errs %02X)", a, e);
			frames++;
			continue;
		}
		char src[16], dest[16], rem[64];
		unsigned headers, comm;
		int at = 0;
		if (sscanf(line, "tx %15s %15s %63s %u %u %n", src, dest, rem, &headers, &comm, &at) == 5 && at > 0) {
			static uint8_t want[8000];
			size_t n = unhex(line + at, want, sizeof want);
			ysf_tx_config_t cfg;
			memset(&cfg, 0, sizeof cfg);
			ysf_csd_init(&cfg.csd, src, dest);
			unhex(rem, (uint8_t *)cfg.csd.rem, sizeof cfg.csd.rem);
			cfg.headers = (uint8_t)headers;
			cfg.vw = true;
			// The voices are the reference's imbe_pattern(n), pulled in order
			cfg.pull_fr = pull;
			pull_n = 0;
			ysf_tx_t t;
			ysf_tx_start(&t, &cfg);
			static uint8_t got[8000];
			memset(got, 0, sizeof got);
			uint32_t d = 0;
			int v;
			while ((v = ysf_tx_dibit(&t)) >= 0 && d < n * 4u) {
				if (d == (headers + comm) * YSF_FRAME_DIBITS - 1u) {
					ysf_tx_stop(&t);	// at the last dibit of the last communication frame: the terminator next
				}
				got[d / 4u] |= (uint8_t)(v << (6u - 2u * (d % 4u)));
				d++;
			}
			CHECK(d == n * 4u, "tx %s: %u dibits, want %zu", src, d, n * 4u);
			CHECK(memcmp(got, want, n) == 0, "tx %s: bit for bit as the reference", src);
			for (size_t i = 0; i < n; i++) {
				if (got[i] != want[i]) {
					printf("  first difference at byte %zu (frame %zu, bit %zu of it)\n", i, i / 120u, (i % 120u) * 8u);
					break;
				}
			}
			txs++;
		}
	}
	fclose(f);
	CHECK(frames == 5 && txs == 1, "vectors read: %d frames, %d transmissions", frames, txs);
}

// --- the frame's FEC ---

static void test_frame_errors(void)
{
	find_words();
	int counts[8] = { 0 };
	for (int i = 0; i < (int)YSF_VW_FRAME_BITS; i++) {
		counts[word_of_bit[i]]++;
	}
	CHECK(counts[0] == 23 && counts[1] == 23 && counts[3] == 23 && counts[4] == 15 && counts[6] == 15 && counts[7] == 7,
	      "the 144 bits fall into the code words: %d %d %d %d %d %d %d %d", counts[0], counts[1], counts[2], counts[3],
	      counts[4], counts[5], counts[6], counts[7]);
	uint32_t trials = 0, exact = 0, reported = 0;
	for (uint32_t n = 0; n < 4000; n++) {
		uint8_t imbe[YSF_VW_IMBE_BYTES], air[18], back[YSF_VW_IMBE_BYTES];
		imbe_pattern(n + 1000u, imbe);
		memset(air, 0, sizeof air);
		ysf_vw_frame_encode(imbe, air, 0);
		// Up to three errors in each Golay word and one in each Hamming word: all corrected, and counted
		int want = 0, want0 = 0, want4 = 0;
		for (int w = 0; w < 7; w++) {
			int k = (int)(rnd() % (w < 4 ? 4u : 2u));
			int done = 0;
			while (done < k) {
				int i = (int)(rnd() % YSF_VW_FRAME_BITS);
				if (word_of_bit[i] != w) {
					continue;
				}
				// a bit not flipped yet: compare with a clean copy
				uint8_t clean[18];
				memset(clean, 0, sizeof clean);
				ysf_vw_frame_encode(imbe, clean, 0);
				if (ysf_bit(air, (uint32_t)i) != ysf_bit(clean, (uint32_t)i)) {
					continue;
				}
				ysf_bit_put(air, (uint32_t)i, !ysf_bit(air, (uint32_t)i));
				done++;
			}
			want += k;
			want0 = w == 0 ? k : want0;
			want4 = w == 4 ? k : want4;
		}
		uint8_t e = ysf_vw_frame_decode(air, 0, back);
		trials++;
		exact += memcmp(back, imbe, sizeof back) == 0;
		reported += e == YSF_VW_ERRS(want, want0, want4);
	}
	CHECK(exact == trials, "frames with correctable errors decoded: %u of %u", exact, trials);
	CHECK(reported == trials, "corrections reported right: %u of %u", reported, trials);
	// The packing is the codec's: what ysf_vw says the decoder reads the same way
	CHECK(IMBE_ERRS(9, 2, 1) == YSF_VW_ERRS(9, 2, 1) && IMBE_ERRS_REPEAT(YSF_VW_ERRS(11, 2, 0)) &&
	      !IMBE_ERRS_REPEAT(YSF_VW_ERRS(10, 3, 1)), "YSF_VW_ERRS packs as IMBE_ERRS");
	// The seven bare bits are what they are: one flipped comes through flipped, with nothing corrected
	uint8_t imbe[YSF_VW_IMBE_BYTES], air[18], back[YSF_VW_IMBE_BYTES];
	imbe_pattern(5, imbe);
	memset(air, 0, sizeof air);
	ysf_vw_frame_encode(imbe, air, 0);
	for (int i = 0; i < (int)YSF_VW_FRAME_BITS; i++) {
		if (word_of_bit[i] == 7) {
			ysf_bit_put(air, (uint32_t)i, !ysf_bit(air, (uint32_t)i));
			break;
		}
	}
	uint8_t e = ysf_vw_frame_decode(air, 0, back);
	CHECK(e == 0 && memcmp(back, imbe, 10) == 0 && back[10] != imbe[10], "u7 is unprotected");
	// Four errors in u0's word: the codec's repeat rule sees it whenever the rest went wrong with it
	uint32_t caught = 0;
	for (uint32_t n = 0; n < 200; n++) {
		imbe_pattern(n + 7000u, imbe);
		memset(air, 0, sizeof air);
		ysf_vw_frame_encode(imbe, air, 0);
		int k = 0;
		while (k < 4) {
			int i = (int)(rnd() % YSF_VW_FRAME_BITS);
			uint8_t clean[18];
			memset(clean, 0, sizeof clean);
			ysf_vw_frame_encode(imbe, clean, 0);
			if (word_of_bit[i] == 0 && ysf_bit(air, (uint32_t)i) == ysf_bit(clean, (uint32_t)i)) {
				ysf_bit_put(air, (uint32_t)i, !ysf_bit(air, (uint32_t)i));
				k++;
			}
		}
		e = ysf_vw_frame_decode(air, 0, back);
		caught += IMBE_ERRS_C0(e) == 3 || IMBE_ERRS_REPEAT(e);
	}
	CHECK(caught == 200, "four errors in u0's word always show as three corrected there: %u of 200", caught);
}

// --- transmissions through the receiver ---

typedef struct {
	uint32_t frames, firsts, voices, ends, calls, wrong, errs;
	uint32_t next;			// the imbe_pattern number expected next
	uint8_t dt, fn[64], ft[64], nfr;
	ysf_csd_t csd;
	uint8_t have;
	bool lost;
} sink_t;

static void s_frame(void *ctx, const ysf_call_t *call, bool first)
{
	sink_t *s = ctx;
	s->frames++;
	s->firsts += first;
	s->dt = call->fich.dt;
	if (call->fich.fi == YSF_FI_COMM && s->nfr < 64u) {
		s->fn[s->nfr] = call->fich.fn;
		s->ft[s->nfr++] = call->fich.ft;
	}
}

static void s_call(void *ctx, const ysf_call_t *call)
{
	sink_t *s = ctx;
	s->calls++;
	s->csd = call->csd;
	s->have = call->have;
}

static void s_voice(void *ctx, const uint8_t bits[YSF_VOICE_BITS], uint8_t q)
{
	(void)ctx;
	(void)bits;
	(void)q;
	failures++;
	printf("FAIL: a VW transmission gave a DN voice frame\n");
}

static void s_voice_fr(void *ctx, const uint8_t imbe[YSF_VW_IMBE_BYTES], uint8_t errs)
{
	sink_t *s = ctx;
	uint8_t want[YSF_VW_IMBE_BYTES];
	imbe_pattern(s->next++, want);
	s->voices++;
	s->errs += YSF_VW_ERRS_TOTAL(errs);
	if (memcmp(want, imbe, sizeof want) != 0) {
		s->wrong++;
	}
}

static void s_end(void *ctx, ysf_end_t why)
{
	sink_t *s = ctx;
	s->ends++;
	s->lost = why == YSF_END_LOST;
}

static void rx_init(ysf_rx_t *r, sink_t *s)
{
	memset(s, 0, sizeof(*s));
	ysf_rx_cb_t cb = { s_frame, s_call, s_voice, NULL, s_end, s };
	ysf_rx_init(r, &cb);
	ysf_rx_voice_fr(r, s_voice_fr);
}

// A VW transmission of `comm` communication frames from our transmitter into the receiver, as soft symbols (strength
// 3 of a +1 level 1) with `flips` dibits in every 1000 decided wrong. skip: dibits left out at the start (late entry).
// invert: the levels upside down
static void run_tx(ysf_rx_t *r, uint32_t comm, uint32_t skip, bool invert, uint32_t flips, uint32_t fich_hit)
{
	ysf_tx_config_t cfg;
	memset(&cfg, 0, sizeof cfg);
	ysf_csd_init(&cfg.csd, "N0CALL", "ALL");
	memcpy(cfg.csd.rem[3], "00123", 5);
	cfg.headers = 1;
	cfg.vw = true;
	cfg.pull_fr = pull;
	ysf_tx_t t;
	pull_n = 0;
	ysf_tx_start(&t, &cfg);
	uint32_t d = 0;
	int v;
	static const int8_t level[4] = { 1, 3, -1, -3 };
	while ((v = ysf_tx_dibit(&t)) >= 0) {
		if (d == (1u + comm) * YSF_FRAME_DIBITS - 1u) {
			ysf_tx_stop(&t);
		}
		uint32_t in_frame = d % YSF_FRAME_DIBITS, frame = d / YSF_FRAME_DIBITS;
		if (flips != 0 && rnd() % 1000u < flips) {
			v ^= 1 + (int)(rnd() % 3u);
		}
		// A FICH hit: half its dibits inverted in frame fich_hit
		if (fich_hit != 0 && frame == fich_hit && in_frame >= YSF_SYNC_DIBITS && in_frame < 120u && (in_frame & 1u)) {
			v ^= 2;
		}
		if (d++ < skip) {
			continue;
		}
		int16_t y = (int16_t)(level[v] * 1000);
		ysf_rx_symbol(r, invert ? (int16_t)-y : y, 1000);
	}
	for (int i = 0; i < 2000; i++) {
		ysf_rx_symbol(r, (int16_t)((rnd() % 2001u) - 1000), 1000);	// noise after
	}
}

static void test_receive(void)
{
	static ysf_rx_t r;
	sink_t s;

	// Clean: the header's callsigns, CSD3 from the first frame, all voices in order (2 + 5 * 5), the end
	rx_init(&r, &s);
	run_tx(&r, 6, 0, false, 0, 0);
	CHECK(s.firsts == 1 && s.ends == 1 && !s.lost, "clean: one transmission, ended by its terminator");
	CHECK(s.dt == YSF_DT_VOICE_FR, "the FICH says Voice FR (%u)", s.dt);
	CHECK(s.voices == 27 && s.wrong == 0 && s.errs == 0, "clean: %u voices (27), %u wrong", s.voices, s.wrong);
	CHECK(r.voice_frs == 27 && r.voices == 0, "counted as VW frames: %u", r.voice_frs);
	CHECK((s.have & (YSF_HAVE_SRC | YSF_HAVE_DEST | YSF_HAVE_REM34)) == (YSF_HAVE_SRC | YSF_HAVE_DEST | YSF_HAVE_REM34),
	      "callsigns and CSD3 (have %02X)", s.have);
	CHECK(memcmp(s.csd.rem[3], "00123", 5) == 0, "CSD3 Rem4: our radio ID");
	CHECK(memcmp(s.csd.src, "N0CALL    ", 10) == 0, "source");
	CHECK(s.nfr == 6 && s.fn[0] == 0 && s.ft[0] == 1 && s.fn[1] == 1 && s.ft[1] == 1 && s.fn[2] == 0 && s.ft[2] == 0 &&
	      s.fn[5] == 0 && s.ft[5] == 0, "FN / FT: 0/1, 1/1, then 0/0 (got %u/%u %u/%u %u/%u)", s.fn[0], s.ft[0], s.fn[1],
	      s.ft[1], s.fn[2], s.ft[2]);
	CHECK(r.csd_bad == 0 && r.fich_bad == 0, "no bad units");

	// Inverted levels
	rx_init(&r, &s);
	run_tx(&r, 4, 0, true, 0, 0);
	CHECK(s.voices == 17 && s.wrong == 0 && s.ends == 1, "inverted: %u voices, %u wrong", s.voices, s.wrong);

	// Late entry: joined in the third communication frame. No header, no CSD3; the voices from where it joined
	rx_init(&r, &s);
	s.next = 7;	// the first two communication frames (2 + 5 voices) gone: the first heard is pattern 7
	run_tx(&r, 5, 3u * YSF_FRAME_DIBITS - 30u, false, 0, 0);
	CHECK(r.late_syncs == 1 && s.voices == 15 && s.wrong == 0, "late entry: %u voices from the third frame, %u wrong",
	      s.voices, s.wrong);
	CHECK((s.have & YSF_HAVE_REM34) == 0 && (s.have & YSF_HAVE_SRC) != 0, "late entry: the terminator's callsigns only");

	// The first frame's FICH damaged: the receiver takes it as the first communication frame after the header, so
	// CSD3 and its two voices still come
	rx_init(&r, &s);
	run_tx(&r, 4, 0, false, 0, 1);
	CHECK(r.fich_bad == 1 && s.voices == 17 && s.wrong == 0 && (s.have & YSF_HAVE_REM34),
	      "first frame's FICH lost: %u voices, CSD3 %s", s.voices, (s.have & YSF_HAVE_REM34) ? "kept" : "lost");
	// The second damaged: assumed FN 1 FT 1, five voices; the fourth: FN 0 FT 0, five voices
	for (uint32_t hit = 2; hit <= 4; hit += 2) {
		rx_init(&r, &s);
		run_tx(&r, 5, 0, false, 0, hit);
		CHECK(r.fich_bad == 1 && s.voices == 22 && s.wrong == 0 && s.nfr == 4,
		      "communication frame %u's FICH lost: %u voices, %u wrong", hit, s.voices, s.wrong);
	}

	// Errors on the air: one dibit in 200 wrong (a raw BER near 4e-3). The Golay and Hamming words take them
	rx_init(&r, &s);
	rng = 12345;
	run_tx(&r, 30, 0, false, 5, 0);
	CHECK(s.voices == 147 && s.wrong <= 2 && s.errs > 50, "1 dibit in 200 wrong: %u voices, %u wrong, %u bits corrected",
	      s.voices, s.wrong, s.errs);
}

// DN then VW then DN: the receiver follows each transmission's DT
static uint32_t dn_voices;

static void dn_voice(void *ctx, const uint8_t bits[YSF_VOICE_BITS], uint8_t q)
{
	(void)ctx;
	(void)bits;
	(void)q;
	dn_voices++;
}

static void send(ysf_rx_t *r, bool vw, uint32_t comm)
{
	ysf_tx_config_t cfg;
	memset(&cfg, 0, sizeof cfg);
	ysf_csd_init(&cfg.csd, "N0CALL", "ALL");
	cfg.headers = 1;
	cfg.vw = vw;
	cfg.pull_fr = pull;
	ysf_tx_t t;
	pull_n = 0;
	ysf_tx_start(&t, &cfg);
	uint32_t d = 0;
	int v;
	static const int8_t level[4] = { 1, 3, -1, -3 };
	while ((v = ysf_tx_dibit(&t)) >= 0) {
		if (d++ == (1u + comm) * YSF_FRAME_DIBITS - 1u) {
			ysf_tx_stop(&t);
		}
		ysf_rx_symbol(r, (int16_t)(level[v] * 1000), 1000);
	}
	for (int i = 0; i < 300; i++) {
		ysf_rx_symbol(r, 0, 1000);
	}
}

static void test_back_to_back(void)
{
	static ysf_rx_t r;
	sink_t s;
	memset(&s, 0, sizeof s);
	ysf_rx_cb_t cb = { s_frame, s_call, dn_voice, NULL, s_end, &s };
	ysf_rx_init(&r, &cb);
	ysf_rx_voice_fr(&r, s_voice_fr);
	dn_voices = 0;
	send(&r, false, 6);
	CHECK(dn_voices == 30 && s.voices == 0, "DN: %u DN voices, %u VW", dn_voices, s.voices);
	s.next = 0;
	send(&r, true, 3);
	CHECK(dn_voices == 30 && s.voices == 12 && s.wrong == 0, "then VW: %u VW voices, %u wrong", s.voices, s.wrong);
	send(&r, false, 2);
	CHECK(dn_voices == 40 && s.voices == 12 && s.ends == 3, "then DN again: %u DN voices, %u ends", dn_voices, s.ends);
	// DN's transmitter is as before: V/D mode 2 in the FICH
	CHECK(s.dt == YSF_DT_VD2, "a DN transmission says DT 2 (%u)", s.dt);
}

// --- the codec over the air ---

// Speech-like test sound: a vowel (a 120 Hz pulse train through two formant resonators) with a slow pitch glide and
// a level envelope, 8 kHz. Integer only, so the test's numbers are the same everywhere
static void vowel(int16_t *out, uint32_t n0, uint32_t n)
{
	static int32_t y1a, y2a, y1b, y2b;
	static uint32_t phase;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t t = n0 + i;
		uint32_t period = 60u + (t / 400u) % 12u;	// 133..111 Hz
		int32_t x = (phase++ % period) == 0 ? 8000 : 0;
		// two resonators (700 Hz and 1200 Hz, Q about 8), Q14 coefficients
		int32_t ya = x + ((2 * 13722 * y1a) >> 14) - ((15550 * y2a) >> 14);
		y2a = y1a;
		y1a = ya / 2;
		int32_t yb = ya / 4 + ((2 * 11019 * y1b) >> 14) - ((15550 * y2b) >> 14);
		y2b = y1b;
		y1b = yb / 2;
		int32_t env = (int32_t)((t / 160u) % 50u < 40u ? 1 : 0);
		int32_t v = env * yb;
		out[i] = (int16_t)(v > 20000 ? 20000 : v < -20000 ? -20000 : v);
	}
}

#define CODEC_FRAMES	120u
static uint8_t enc_frames[CODEC_FRAMES][YSF_VW_IMBE_BYTES];
static uint32_t enc_next, rx_next;
static uint8_t rx_frames[CODEC_FRAMES + 8u][YSF_VW_IMBE_BYTES];
static uint8_t rx_errs[CODEC_FRAMES + 8u];

static bool pull_enc(void *c, uint8_t imbe[YSF_VW_IMBE_BYTES])
{
	(void)c;
	if (enc_next >= CODEC_FRAMES) {
		return false;	// the transmitter sends its silence frame
	}
	memcpy(imbe, enc_frames[enc_next++], YSF_VW_IMBE_BYTES);
	return true;
}

static void got_fr(void *ctx, const uint8_t imbe[YSF_VW_IMBE_BYTES], uint8_t errs)
{
	(void)ctx;
	if (rx_next < CODEC_FRAMES + 8u) {
		memcpy(rx_frames[rx_next], imbe, YSF_VW_IMBE_BYTES);
		rx_errs[rx_next++] = errs;
	}
}

static imbe_dec_state_t dst;
static mbe_params_t dpr;
static mbe_state_t dms;
static mbe_frame_t dprev, dcur;
static int32_t dbuf[MBE_BUF_WORDS];
static mbe_est_state_t est;

static void decode_all(uint8_t (*fr)[YSF_VW_IMBE_BYTES], const uint8_t *errs, uint32_t n, int16_t *pcm, uint32_t *bad)
{
	imbe_mem_t m = { &dst, &dpr, { &dms, &dprev, &dcur, dbuf } };
	imbe_dec_init(&m);
	*bad = 0;
	for (uint32_t i = 0; i < n; i++) {
		*bad += imbe_dec_frame(&m, fr[i], errs != NULL ? errs[i] : 0, pcm + i * MBE_FRAME_LEN) != 0;
	}
}

static void test_codec(void)
{
	static int16_t speech[CODEC_FRAMES * MBE_FRAME_LEN];
	vowel(speech, 0, CODEC_FRAMES * MBE_FRAME_LEN);
	imbe_enc_mem_t em = { &dst.q, &est, &dpr, dbuf };
	imbe_enc_init(&em);
	uint32_t silent = 0, sound = 0;
	for (uint32_t i = 0; i < CODEC_FRAMES; i++) {
		imbe_enc_frame(&em, speech + i * MBE_FRAME_LEN, enc_frames[i]);
		if (i % 50u < 40u) {	// the vowel is on (vowel's envelope)
			sound++;
			silent += memcmp(enc_frames[i], imbe_silence, IMBE_BYTES) == 0;
		}
	}
	CHECK(silent == 0, "the encoder made %u silence frames of %u of sound", silent, sound);

	// Over the air: 5 dibits in 1000 wrong
	static ysf_rx_t r;
	sink_t s;
	memset(&s, 0, sizeof s);
	ysf_rx_cb_t cb = { s_frame, NULL, NULL, NULL, s_end, &s };
	ysf_rx_init(&r, &cb);
	ysf_rx_voice_fr(&r, got_fr);
	ysf_tx_config_t cfg;
	memset(&cfg, 0, sizeof cfg);
	ysf_csd_init(&cfg.csd, "N0CALL", "ALL");
	cfg.headers = 1;
	cfg.vw = true;
	memcpy(cfg.silence_fr, imbe_silence, IMBE_BYTES);
	cfg.pull_fr = pull_enc;
	ysf_tx_t t;
	enc_next = rx_next = 0;
	rng = 777;
	ysf_tx_start(&t, &cfg);
	int v;
	static const int8_t level[4] = { 1, 3, -1, -3 };
	while ((v = ysf_tx_dibit(&t)) >= 0) {
		if (enc_next >= CODEC_FRAMES) {
			ysf_tx_stop(&t);
		}
		if (rnd() % 1000u < 5u) {
			v ^= 1 + (int)(rnd() % 3u);
		}
		ysf_rx_symbol(&r, (int16_t)(level[v] * 1000), 1000);
	}
	CHECK(rx_next >= CODEC_FRAMES && t.silences >= 1, "%u frames received (%u sent, %u silences after)", rx_next,
	      CODEC_FRAMES, t.silences);
	uint32_t same = 0;
	for (uint32_t i = 0; i < CODEC_FRAMES; i++) {
		same += memcmp(rx_frames[i], enc_frames[i], YSF_VW_IMBE_BYTES) == 0;
	}
	// u7's seven bits are bare: about one frame in 60 loses one at this error rate, and decodes slightly off
	CHECK(same >= CODEC_FRAMES - 6u, "%u of %u frames through the air unchanged", same, CODEC_FRAMES);
	for (uint32_t i = CODEC_FRAMES; i < rx_next; i++) {
		CHECK(memcmp(rx_frames[i], imbe_silence, IMBE_BYTES) == 0 || rx_errs[i] != 0, "the frames after: silence");
	}

	// Decoded: the air's frames against the encoder's own, sample for sample where the frames match, and both loud
	static int16_t ref[CODEC_FRAMES * MBE_FRAME_LEN], air[CODEC_FRAMES * MBE_FRAME_LEN];
	uint32_t bad_ref, bad_air;
	decode_all(enc_frames, NULL, CODEC_FRAMES, ref, &bad_ref);
	decode_all(rx_frames, rx_errs, CODEC_FRAMES, air, &bad_air);
	int64_t e_ref = 0, e_diff = 0, e_in = 0;
	for (uint32_t i = 0; i < CODEC_FRAMES * MBE_FRAME_LEN; i++) {
		e_ref += (int64_t)ref[i] * ref[i];
		e_diff += (int64_t)(ref[i] - air[i]) * (ref[i] - air[i]);
		e_in += (int64_t)speech[i] * speech[i];
	}
	CHECK(bad_ref == 0 && bad_air <= 2, "bad frames: %u clean, %u over the air", bad_ref, bad_air);
	CHECK(e_ref > e_in / 50, "the decoded vowel has its level (%lld against %lld)", (long long)e_ref, (long long)e_in);
	CHECK(e_diff * 20 < e_ref, "over the air sounds as the clean decode (difference %lld of %lld)", (long long)e_diff,
	      (long long)e_ref);
}

int main(void)
{
	test_vectors();
	test_frame_errors();
	test_receive();
	test_back_to_back();
	test_codec();
	if (failures) {
		printf("ysfVwTest: %d FAILED\n", failures);
		return 1;
	}
	printf("ysfVwTest: all passed\n");
	return 0;
}
