// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The half-rate vocoder of D-STAR voice (core/mbe/ambe.h): its FEC, the quantizer against the dequantizer, and the
// encoder and decoder on signals whose parameters are known.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/mbe/ambe.h"
#include "../../core/mbe/ambe_q.h"
#include "../../core/mbe/ambe_tab.h"
#include "../../core/mbe/mbe_est.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 0x2468ACEu;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

// The silence frame every D-STAR transmitter sends (JARL standard 6.6): no error in either Golay word, pitch code 124,
// and the FEC gives back the same 72 bits
static void test_silence_frame(void)
{
	uint8_t d[AMBE_DATA_BYTES], air[AMBE_BYTES];
	uint8_t e = ambe_fec_decode(ambe_silence, d);

	CHECK(e == AMBE_ERRS(0, 0), "silence frame: errors %02x", e);
	CHECK(ambe_q_get(d, AMBE_Q_B0) == AMBE_B0_SILENCE, "silence frame: pitch code %d", ambe_q_get(d, AMBE_Q_B0));
	ambe_fec_encode(d, air);
	CHECK(memcmp(air, ambe_silence, AMBE_BYTES) == 0, "silence frame: not the same after the FEC");
}

// Each of the 72 bits is used once
static void test_interleave(void)
{
	uint8_t seen[4][32];

	memset(seen, 0, sizeof seen);
	for (int i = 0; i < AMBE_FRAME_BITS; i++)
		seen[ambe_ilv[i] >> 5][ambe_ilv[i] & 31]++;
	int bad = 0, size[4] = { 24, 23, 11, 14 };

	for (int w = 0; w < 4; w++)
		for (int b = 0; b < 32; b++)
			bad += seen[w][b] != (b < size[w]);
	CHECK(bad == 0, "interleave: %d positions used other than once", bad);
}

// Random data with up to three bit errors in the first two code words (each word correcting its own), and any errors
// in the last two (unprotected: they pass as they are)
static void test_fec(void)
{
	int wrong = 0, miscount = 0;

	for (int n = 0; n < 5000; n++) {
		uint8_t d[AMBE_DATA_BYTES], back[AMBE_DATA_BYTES], air[AMBE_BYTES];
		int flips = (int)(rnd() % 4u), want = 0;

		for (int i = 0; i < AMBE_DATA_BYTES; i++)
			d[i] = (uint8_t)rnd();
		d[6] &= 0x80;
		ambe_fec_encode(d, air);
		// flip bits of one Golay word only (the second word's whitening follows the first's data)
		int word = (int)(rnd() & 1u);
		int pos[3] = { -1, -1, -1 };

		for (int f = 0; f < flips; f++) {
			int i;

			do {
				i = (int)(rnd() % AMBE_FRAME_BITS);
			} while ((ambe_ilv[i] >> 5) != word || i == pos[0] || i == pos[1] || i == pos[2]);
			pos[f] = i;
			air[i >> 3] ^= (uint8_t)(1u << (i & 7));
			want++;
		}
		uint8_t e = ambe_fec_decode(air, back);

		wrong += memcmp(d, back, AMBE_DATA_BYTES) != 0;
		miscount += AMBE_ERRS_TOTAL(e) != want;
	}
	CHECK(wrong == 0, "fec: %d of 5000 frames with up to three errors in a word not corrected", wrong);
	CHECK(miscount == 0, "fec: %d of 5000 frames with the wrong error count", miscount);
}

// Frames of random bits (a voice pitch code) through the dequantizer, the parameters back through the quantizer from
// the same state: the same state and the same parameters, frame after frame, and the same bits where the dequantizer
// reads them (a block of two harmonics has no higher-order coefficients, one of three to five uses only the first
// ones of its codebook's entries). A frame whose amplitudes the dequantizer clipped has no exact inverse; they must
// be rare
static void test_bits_round_trip(void)
{
	ambe_state_t sd, se;
	int clipped = 0, frames = 20000, bad = 0;

	ambe_state_reset(&sd);
	ambe_state_reset(&se);
	for (int n = 0; n < frames; n++) {
		uint8_t f[AMBE_DATA_BYTES], g[AMBE_DATA_BYTES];
		mbe_params_t p, pq;
		int b0 = (int)(rnd() % AMBE_B0_VOICE), clip = 0;

		for (int i = 0; i < AMBE_DATA_BYTES; i++)
			f[i] = (uint8_t)rnd();
		f[3] &= 0x7F;		// d24
		f[6] &= 0x80;
		ambe_q_put(f, AMBE_Q_B0, b0);
		CHECK(ambe_unpack(&sd, f, &p) == AMBE_FRAME_OK, "frame %d: not valid", n);
		CHECK(p.L == ambe_L[b0], "frame %d: L", n);
		for (int l = 0; l < p.L; l++)
			if (p.a[l] <= -32767 || p.a[l] >= 32767)
				clip = 1;
		pq = p;
		ambe_pack(&se, b0, ambe_q_get(f, AMBE_Q_B1), &pq, g);
		if (clip) {
			clipped++;
			se = sd;
			continue;
		}
		// the same codebook value: some codebooks hold an entry twice
		int fields_differ = ambe_q_get(f, AMBE_Q_B0) != ambe_q_get(g, AMBE_Q_B0) ||
				    ambe_q_get(f, AMBE_Q_B1) != ambe_q_get(g, AMBE_Q_B1) ||
				    ambe_dg[ambe_q_get(f, AMBE_Q_B2)] != ambe_dg[ambe_q_get(g, AMBE_Q_B2)] ||
				    memcmp(ambe_prba24[ambe_q_get(f, AMBE_Q_B3)], ambe_prba24[ambe_q_get(g, AMBE_Q_B3)], 6) ||
				    memcmp(ambe_prba58[ambe_q_get(f, AMBE_Q_B4)], ambe_prba58[ambe_q_get(g, AMBE_Q_B4)], 8);
		if (fields_differ || memcmp(&sd, &se, sizeof sd) ||
		    memcmp(p.a, pq.a, sizeof(int16_t) * (size_t)p.L) || memcmp(p.v, pq.v, (size_t)p.L) || p.w0 != pq.w0) {
			if (bad++ < 5) {
				printf("  frame %d b0 %d L %d:", n, b0, p.L);
				for (int b = 0; b < AMBE_Q_N; b++)
					printf(" b%d %d/%d", b, ambe_q_get(f, b), ambe_q_get(g, b));
				printf("\n");
			}
			se = sd;
		}
	}
	CHECK(bad == 0, "%d of %d frames did not come back", bad, frames);
	CHECK(clipped < frames / 20, "%d of %d random frames clipped", clipped, frames);
	printf("# bits round trip: %d frames, %d clipped and skipped, %d differ\n", frames, clipped, bad);
}

// Smooth envelopes at every pitch, at speech levels (a harmonic of about 1000 PCM units is 7 in the model's log2),
// with a ripple: what the receiver reconstructs is close to what was encoded once the prediction has settled on a
// repeated frame
static void test_envelope(void)
{
	double worst = 0, sum = 0;
	int n = 0;

	for (int b0 = 0; b0 < AMBE_B0_VOICE; b0 += 3) {
		ambe_state_t s;
		int L = ambe_L[b0];
		double err = 0;

		ambe_state_reset(&s);
		for (int rep = 0; rep < 12; rep++) {
			mbe_params_t p;
			uint8_t d[AMBE_DATA_BYTES];

			memset(&p, 0, sizeof p);
			for (int l = 0; l < L; l++)
				p.a[l] = (int16_t)lrint(2048 * (10.0 - 1.5 * log2(1 + l) + 0.8 * sin(l * 0.7)));
			mbe_params_t want = p;

			ambe_pack(&s, b0, 15, &p, d);
			if (rep == 11) {
				for (int l = 0; l < L; l++)
					err += fabs((p.a[l] - want.a[l]) / 2048.0) * 6.0206;
				err /= L;
			}
		}
		if (err > 2.5)
			printf("  b0 %d L %d: %.2f dB\n", b0, L, err);
		sum += err;
		n++;
		if (err > worst)
			worst = err;
	}
	printf("# envelope: settled error %.2f dB mean, %.2f dB worst\n", sum / n, worst);
	CHECK(worst < 4.0, "settled envelope error %.2f dB", worst);
	CHECK(sum / n < 2.5, "mean settled envelope error %.2f dB", sum / n);
}

// --- the estimator and the whole encoder, on signals whose parameters are known ---

typedef struct {
	ambe_state_t qs;
	mbe_est_state_t est;
	mbe_params_t p;
	int32_t buf[MBE_BUF_WORDS];
	ambe_enc_mem_t m;
	ambe_dec_state_t ds;
	mbe_params_t dp;
	mbe_state_t ms;
	mbe_frame_t prev, cur;
	int32_t dbuf[MBE_BUF_WORDS];
	ambe_mem_t d;
} rig_t;

static void rig_init(rig_t *r)
{
	memset(r, 0, sizeof *r);
	r->m = (ambe_enc_mem_t){ &r->qs, &r->est, &r->p, r->buf };
	ambe_enc_init(&r->m);
	r->d = (ambe_mem_t){ &r->ds, &r->dp, { &r->ms, &r->prev, &r->cur, r->dbuf } };
	ambe_dec_init(&r->d);
}

static double gauss(void)
{
	double u = ((rnd() >> 8) + 1.0) / 16777217.0, v = (rnd() >> 8) / 16777216.0;

	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static double rms_of(const int16_t *x, int n)
{
	double s = 0;

	for (int i = 0; i < n; i++)
		s += (double)x[i] * x[i];
	return sqrt(s / n);
}

static int frame_b0(const uint8_t air[AMBE_BYTES])
{
	uint8_t d[AMBE_DATA_BYTES];

	(void)ambe_fec_decode(air, d);
	return ambe_q_get(d, AMBE_Q_B0);
}

// harmonics of a pitch at every fifth code, falling 6 dB per octave: the pitch code found, and the level out of a
// decoder
static void test_harmonic(void)
{
	rig_t *r = malloc(sizeof *r);
	static int16_t in[40 * MBE_FRAME_LEN], out[40 * MBE_FRAME_LEN];
	double worst_lvl = 0;
	int worst_b0 = 0, unvoiced = 0;

	for (int b0 = 2; b0 < AMBE_B0_VOICE; b0 += 5) {
		double f0 = ambe_w0[b0] / 4294967296.0, w = 2 * M_PI * f0;
		int L = ambe_L[b0], got = 0;

		rig_init(r);
		for (int n = 0; n < 40 * MBE_FRAME_LEN; n++) {
			double v = 0;

			for (int l = 1; l <= L && l * f0 < 0.45; l++)
				v += 3000.0 / l * cos(w * l * n + l * l * 0.37);
			in[n] = (int16_t)lrint(v);
		}
		for (int f = 0; f < 40; f++) {
			uint8_t air[AMBE_BYTES];

			ambe_enc_frame(&r->m, in + f * MBE_FRAME_LEN, air);
			ambe_dec_frame(&r->d, air, out + f * MBE_FRAME_LEN);
			if (f >= 10) {
				int c = frame_b0(air);

				if (abs(c - b0) > got)
					got = abs(c - b0);
				for (int l = 0; l < r->p.L && (l + 1) * f0 < 0.375; l++)
					unvoiced += !r->p.v[l];
			}
		}
		if (got > worst_b0)
			worst_b0 = got;
		CHECK(got <= 2, "b0 %d: the encoder's pitch code is off by %d", b0, got);
		double lvl = fabs(20 * log10(rms_of(out + 15 * MBE_FRAME_LEN, 20 * MBE_FRAME_LEN) /
					     rms_of(in + 15 * MBE_FRAME_LEN, 20 * MBE_FRAME_LEN)));

		if (lvl > worst_lvl)
			worst_lvl = lvl;
		CHECK(lvl < 3.0, "b0 %d: level through encoder and decoder off by %.2f dB", b0, lvl);
	}
	printf("# harmonic: pitch code off by at most %d; level within %.2f dB; %d harmonics under 3 kHz unvoiced\n",
	       worst_b0, worst_lvl, unvoiced);
	CHECK(unvoiced < 50, "%d harmonics of a periodic signal unvoiced", unvoiced);
	free(r);
}

// white noise: hardly a band voiced, and the level out of a decoder near the level in
static void test_noise(void)
{
	rig_t *r = malloc(sizeof *r);
	static int16_t in[60 * MBE_FRAME_LEN], out[60 * MBE_FRAME_LEN];
	int voiced = 0, total = 0;

	rig_init(r);
	for (int n = 0; n < 60 * MBE_FRAME_LEN; n++)
		in[n] = (int16_t)lrint(1500 * gauss());
	for (int f = 0; f < 60; f++) {
		uint8_t air[AMBE_BYTES];

		ambe_enc_frame(&r->m, in + f * MBE_FRAME_LEN, air);
		ambe_dec_frame(&r->d, air, out + f * MBE_FRAME_LEN);
		if (f >= 10)
			for (int l = 0; l < r->p.L; l++, total++)
				voiced += r->p.v[l];
	}
	double lvl = 20 * log10(rms_of(out + 20 * MBE_FRAME_LEN, 40 * MBE_FRAME_LEN) /
				rms_of(in + 20 * MBE_FRAME_LEN, 40 * MBE_FRAME_LEN));

	printf("# noise: %d of %d harmonics voiced, level %+.2f dB\n", voiced, total, lvl);
	CHECK(voiced * 20 < total, "%d of %d harmonics of noise voiced", voiced, total);
	CHECK(fabs(lvl) < 4.0, "noise level off by %.2f dB", lvl);
	free(r);
}

// zeros are sent as silence frames, and a decoder plays them as silence
static void test_zeros(void)
{
	rig_t *r = malloc(sizeof *r);
	int16_t in[MBE_FRAME_LEN], out[MBE_FRAME_LEN];
	int not_silent = 0, loud = 0;

	rig_init(r);
	memset(in, 0, sizeof in);
	for (int f = 0; f < 20; f++) {
		uint8_t air[AMBE_BYTES];

		ambe_enc_frame(&r->m, in, air);
		not_silent += memcmp(air, ambe_silence, AMBE_BYTES) != 0;
		ambe_dec_frame(&r->d, air, out);
		loud += rms_of(out, MBE_FRAME_LEN) > 1.0;
	}
	CHECK(not_silent == 0, "%d of 20 frames of zeros not sent as silence frames", not_silent);
	CHECK(loud == 0, "%d of 20 silence frames decoded to sound", loud);
	free(r);
}

// a burst of corrupt frames: repeated, then muted, and decoding recovers when clean frames come back
static void test_errors(void)
{
	rig_t *r = malloc(sizeof *r);
	static int16_t in[60 * MBE_FRAME_LEN];
	int16_t out[MBE_FRAME_LEN];
	int muted = 0, back = 0;
	double f0 = ambe_w0[40] / 4294967296.0;

	rig_init(r);
	for (int n = 0; n < 60 * MBE_FRAME_LEN; n++)
		in[n] = (int16_t)lrint(2000 * cos(2 * M_PI * f0 * n) + 1000 * cos(4 * M_PI * f0 * n));
	for (int f = 0; f < 60; f++) {
		uint8_t air[AMBE_BYTES];

		ambe_enc_frame(&r->m, in + f * MBE_FRAME_LEN, air);
		if (f >= 20 && f < 30)
			for (int i = 0; i < AMBE_BYTES; i++)
				air[i] ^= (uint8_t)rnd();
		int run = ambe_dec_frame(&r->d, air, out);

		if (f >= 26 && f < 30)
			muted += run > 0;
		if (f >= 50)
			back += run == 0 && rms_of(out, MBE_FRAME_LEN) > 300;
	}
	CHECK(muted >= 3, "only %d of 4 frames late in an error burst concealed", muted);
	CHECK(back == 10, "only %d of 10 clean frames after the burst decoded", back);
	free(r);
}

int main(void)
{
	test_silence_frame();
	test_interleave();
	test_fec();
	test_bits_round_trip();
	test_envelope();
	test_harmonic();
	test_noise();
	test_zeros();
	test_errors();
	printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ok", checks - failures, checks);
	return failures != 0;
}
