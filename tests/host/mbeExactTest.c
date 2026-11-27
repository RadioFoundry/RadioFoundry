// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The full-rate codec (core/mbe) sample for sample and bit for bit as upstream RadioFoundry wrote it: a CRC of what the
// encoder makes of 12 s of test speech and of what the decoder makes of those frames, of random frames (loud ones
// among them, the oscillators' wide path) and of the same with channel errors (repeats, mutes, concealment), against
// the CRCs upstream's core/mbe gave (48658c8d).
//
// The Makefile builds this twice: as the host runs core/dsp/fxp.h (64-bit products) and with -DFXP_SPLIT16, the
// 16-bit halves a C-SKY build without HI/LO runs. Both must give the same CRCs, so neither the products from halves nor
// the decoder's faster loops (fxp_mulshr16, the narrow oscillator, the unfolded cosine table) changed any output. A
// change that is meant to change the codec's output changes these CRCs: write the new ones down with the reason.
//
// And twice more with -DCONFIG_MBE2400=1, the core's per-frame options (CONFIG_MBE_OPTS) built in: the full-rate CRCs
// are the same ones, so the options change nothing for a front end that does not set them; and the 2400 bit/s codec's
// own CRCs (the same speech, random frames, lost frames) hold with 64-bit products and with the halves.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbe/imbe.h"
#include "mbe/mbe_est.h"
#include "mbe/mbe_dsp.h"
#if CONFIG_MBE2400
#include "mbe/mbe2400.h"
#endif

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// what upstream's core/mbe gave (the build that printed them: this file against 6c8dd48f's core/mbe)
#define GOLD_ENC	0x1F8F2166u
#define GOLD_DEC	0xD68E15CCu

#define NSP	600u	// frames of test speech (12 s)
#define NRND	600u	// random frames

static uint32_t rng_state = 0x1234567u;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static uint32_t crc32(uint32_t crc, const void *p, size_t n)
{
	const uint8_t *b = p;

	crc = ~crc;
	while (n--) {
		crc ^= *b++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static struct {
	mbe_est_state_t est;
	imbe_dec_state_t st;
	mbe_params_t pr;
	mbe_state_t ms;
	mbe_frame_t prev, cur;
	int32_t buf[MBE_BUF_WORDS];
} m;

// Test speech: voiced stretches (a pulse train through two resonators, the pitch gliding from 70 to 300 Hz and
// jumping), noise bursts, loud and quiet passages and silence, so the encoder takes every path
static void speech(int16_t *out, uint32_t n)
{
	int32_t y1a = 0, y2a = 0, y1b = 0, y2b = 0;
	uint32_t ph = 0;

	for (uint32_t t = 0; t < n; t++) {
		uint32_t seg = t / 4000u, k = seg % 6u;		// half a second each
		uint32_t period = 27u + (t / 97u) % 87u;	// 70 .. 300 Hz, a jump every 87 steps
		int32_t gain = k == 4 ? 4 : k == 5 ? 0 : 1 + (int32_t)(seg % 3u);
		int32_t x;

		ph++;
		if (k == 3)
			x = (int32_t)(rng() >> 21) - 1024;	// a noise burst (fricative)
		else
			x = ph % period == 0 ? 6000 : 0;
		int32_t ya = x + ((2 * 13722 * y1a) >> 14) - ((15550 * y2a) >> 14);
		y2a = y1a;
		y1a = ya / 2;
		int32_t yb = ya / 4 + ((2 * 11019 * y1b) >> 14) - ((15550 * y2b) >> 14);
		y2b = y1b;
		y1b = yb / 2;
		yb *= gain;
		out[t] = (int16_t)(yb > 32767 ? 32767 : yb < -32768 ? -32768 : yb);
	}
}

#if CONFIG_MBE2400
// what this test's first build with the codec gave (core/mbe at the commit that added these lines)
#define GOLD24_ENC	0x5173C97Fu
#define GOLD24_DEC	0xF39DCA6Bu

// The 2400 bit/s codec the same way: the speech through its encoder, those frames and random bytes through its decoder,
// then again with frames lost one at a time and in a run past the repeats
static void test_2400(const int16_t *in)
{
	static uint8_t fr[NSP + NRND][MBE2400_BYTES];
	static struct {
		mbe2400_state_t es, ds;
		int32_t ebuf[MBE_BUF_WORDS];
	} k;
	int16_t pcm[MBE_FRAME_LEN];
	uint32_t enc = 0, dec = 0;
	mbe2400_enc_mem_t em = { &k.es, &m.est, &m.pr, k.ebuf };
	mbe2400_mem_t dm = { &k.ds, &m.pr, { &m.ms, &m.prev, &m.cur, m.buf } };

	mbe2400_enc_init(&em);
	for (uint32_t i = 0; i < NSP; i++) {
		mbe2400_enc_frame(&em, in + i * MBE_FRAME_LEN, fr[i]);
		enc = crc32(enc, fr[i], MBE2400_BYTES);
	}
	for (uint32_t i = NSP; i < NSP + NRND; i++) {
		for (int j = 0; j < MBE2400_BYTES; j++)
			fr[i][j] = (uint8_t)rng();
		if (i % 7u == 0)
			memcpy(fr[i], mbe2400_silence, MBE2400_BYTES);
	}
	mbe2400_dec_init(&dm);
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < NSP + NRND; i++) {
			int lost = pass == 1 && (rng() % 16u >= 13u || (i >= 300u && i < 340u));
			uint8_t errs;

			mbe2400_dec_frame(&dm, lost ? NULL : fr[i], &errs, pcm);
			dec = crc32(dec, pcm, sizeof pcm);
			dec = crc32(dec, &errs, 1);
		}
	}
	printf("2400 bit/s encoder: %u frames, CRC %08X; decoder: %u frames, CRC %08X\n", NSP, enc, 2u * (NSP + NRND), dec);
	CHECK(enc == GOLD24_ENC, "2400 bit/s encoder CRC %08X, was %08X", enc, GOLD24_ENC);
	CHECK(dec == GOLD24_DEC, "2400 bit/s decoder CRC %08X, was %08X", dec, GOLD24_DEC);
}
#endif

// The cosine the oscillators and mbe_cos read, from the unfolded table (built with -DFXP_SPLIT16) or folded: each
// entry the quarter wave's own value
static void test_cos_table(void)
{
	int same = 0;

	for (uint32_t i = 0; i < 1024u; i++)
		same += mbe_cos_idx(i) == mbe_sin_idx(i + 256u);
	CHECK(same == 1024, "%d of 1024 cosine entries as mbe_sin_idx gives them", same);
	same = 0;
	for (uint32_t k = 0; k < 4096u; k++) {
		uint32_t ph = k * 1048573u + (k << 20);

		same += mbe_cos(ph) == mbe_sin_idx(((ph + 0x200000u) >> 22) + 256u);
	}
	CHECK(same == 4096, "%d of 4096 phases: mbe_cos as the folded lookup", same);
}

int main(void)
{
	static int16_t in[NSP * MBE_FRAME_LEN];
	static uint8_t fr[NSP + NRND][IMBE_BYTES];
	int16_t pcm[MBE_FRAME_LEN];
	uint32_t enc = 0, dec = 0;
	imbe_enc_mem_t em = { &m.st.q, &m.est, &m.pr, m.buf };
	imbe_mem_t dm = { &m.st, &m.pr, { &m.ms, &m.prev, &m.cur, m.buf } };
	int peak = 0;

	test_cos_table();
	speech(in, NSP * MBE_FRAME_LEN);
	imbe_enc_init(&em);
	for (uint32_t i = 0; i < NSP; i++) {
		imbe_enc_frame(&em, in + i * MBE_FRAME_LEN, fr[i]);
		enc = crc32(enc, fr[i], IMBE_BYTES);
	}
	// random frames with a used pitch code (the first 8 bits' b0 in 0..207), as mbeTest's golden ones
	for (uint32_t i = NSP; i < NSP + NRND; i++) {
		for (int j = 0; j < IMBE_BYTES; j++)
			fr[i][j] = (uint8_t)rng();
		if (i % 7u == 0)
			memcpy(fr[i], imbe_silence, IMBE_BYTES);
	}
	// every frame clean, then again with corrections reported: some repeated, a burst muted, a frame lost
	imbe_dec_init(&dm);
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < NSP + NRND; i++) {
			uint8_t errs = 0;

			if (pass == 1) {
				uint32_t r = rng() % 16u;

				errs = r < 10u ? 0 : r < 13u ? IMBE_ERRS(3, 1, 0) : r < 15u ? IMBE_ERRS(11, 2, 1) : IMBE_ERRS_LOST;
				if (i >= 300u && i < 340u)
					errs = IMBE_ERRS(15, 3, 1);	// a long bad stretch: past the repeats, muted
			}
			imbe_dec_frame(&dm, errs == IMBE_ERRS_LOST ? imbe_silence : fr[i], errs, pcm);
			for (int n = 0; n < MBE_FRAME_LEN; n++)
				peak = abs(pcm[n]) > peak ? abs(pcm[n]) : peak;
			dec = crc32(dec, pcm, sizeof pcm);
		}
	}
	printf("encoder: %u frames, CRC %08X; decoder: %u frames, CRC %08X, peak %d (%s)\n", NSP, enc,
	       2u * (NSP + NRND), dec, peak,
#if defined(FXP_SPLIT16)
	       "products from 16-bit halves"
#else
	       "64-bit products"
#endif
	);
	CHECK(enc == GOLD_ENC, "encoder CRC %08X, upstream's %08X", enc, GOLD_ENC);
	CHECK(dec == GOLD_DEC, "decoder CRC %08X, upstream's %08X", dec, GOLD_DEC);
	// Loud enough for the oscillators' wide path (a harmonic over 4,096): an instrumented build counted 2,161 of
	// 124,080 pieces there
	CHECK(peak > 8000, "peak %d: the loud frames should reach the oscillators' wide path", peak);
#if CONFIG_MBE2400
	test_2400(in);
#endif
	printf("mbeExactTest: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
