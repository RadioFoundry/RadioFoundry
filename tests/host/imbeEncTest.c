// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The full-rate vocoder's encoder (core/mbe): the quantizer against the dequantizer.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/mbe/imbe.h"
#include "../../core/mbe/mbe_est.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 0x1234567u;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

// Frames of random bits (a valid pitch code, the unused last bit 0) through the dequantizer, the parameters back
// through the quantizer from the same state: the same bits, the same state and the same parameters, frame after frame.
// A random frame can ask for an amplitude outside what a parameter holds; the dequantizer clips it, and such a frame
// has no exact inverse. They are counted and must be rare.
static void test_bits_round_trip(void)
{
	imbe_state_t sd, se;
	int clipped = 0, frames = 20000, bad = 0;

	imbe_state_reset(&sd);
	imbe_state_reset(&se);
	for (int n = 0; n < frames; n++) {
		uint8_t f[IMBE_BYTES], g[IMBE_BYTES];
		mbe_params_t p, pq;
		int b0 = (int)(rnd() % 208u), clip = 0;

		for (int i = 0; i < IMBE_BYTES; i++)
			f[i] = (uint8_t)rnd();
		f[0] = (uint8_t)((f[0] & 3) | ((b0 >> 2) << 2));
		f[10] = (uint8_t)((f[10] & 0xF8) | (((b0 >> 1) & 1) << 2) | ((b0 & 1) << 1));
		CHECK(imbe_unpack(&sd, f, &p) == IMBE_FRAME_OK, "frame %d: not valid", n);
		CHECK(p.L == imbe_pitch_harmonics(b0), "frame %d: L", n);
		for (int l = 0; l < p.L; l++)
			if (p.a[l] <= -32767 || p.a[l] >= 32767)
				clip = 1;
		pq = p;
		imbe_pack(&se, b0, &pq, g);
		if (clip) {
			clipped++;
			se = sd;		// no exact inverse: carry on from the same state
			continue;
		}
		if (memcmp(f, g, IMBE_BYTES) || memcmp(&sd, &se, sizeof sd) || memcmp(p.a, pq.a, sizeof(int16_t) * (size_t)p.L)
		    || memcmp(p.v, pq.v, (size_t)p.L) || p.w0 != pq.w0) {
			if (bad++ < 5) {
				printf("  frame %d b0 %d L %d:", n, b0, p.L);
				for (int i = 0; i < IMBE_BYTES; i++)
					printf(" %02x/%02x", f[i], g[i]);
				printf("\n");
			}
			se = sd;
		}
	}
	CHECK(bad == 0, "%d of %d frames did not come back bit for bit", bad, frames);
	CHECK(clipped < frames / 20, "%d of %d random frames clipped", clipped, frames);
	printf("# bits round trip: %d frames, %d clipped and skipped, %d differ\n", frames, clipped, bad);
}

// Smooth spectral envelopes at every pitch: what the receiver reconstructs is close to what was encoded, and it gets
// closer as the frames repeat (the prediction carries what one frame's bits cannot)
static void test_envelope(void)
{
	double worst_first = 0, worst_settled = 0, sum_settled = 0;
	int n = 0;

	for (int b0 = 0; b0 < 208; b0 += 3) {
		imbe_state_t s;
		int L = imbe_pitch_harmonics(b0);
		mbe_params_t in, p;
		uint8_t f[IMBE_BYTES];
		double tilt = -3.0 * (double)(rnd() % 1000u) / 1000.0, bump = (double)(rnd() % 1000u) / 1000.0;

		memset(&in, 0, sizeof in);
		for (int l = 0; l < L; l++) {
			double x = (l + 1.0) / L;

			in.a[l] = (int16_t)lrint(2048.0 * (6.0 + tilt * x + 1.5 * sin(2 * M_PI * (x * 2.5 + bump))));
			in.v[l] = (uint8_t)(l < L / 2);
		}
		imbe_state_reset(&s);
		for (int rep = 0; rep < 8; rep++) {
			double e2 = 0;

			p = in;
			imbe_pack(&s, b0, &p, f);
			for (int l = 0; l < L; l++) {
				double d = (p.a[l] - in.a[l]) / 2048.0 * 6.0206;	// dB

				e2 += d * d;
			}
			e2 = sqrt(e2 / L);
			if (rep == 0 && e2 > worst_first)
				worst_first = e2;
			if (rep == 7) {
				if (e2 > worst_settled)
					worst_settled = e2;
				sum_settled += e2;
				n++;
			}
			for (int l = 0; l < L; l++)
				CHECK(p.v[l] == (uint8_t)((l / 3 < 12 ? l / 3 : 11) * 3 < L / 2), "b0 %d: voicing of harmonic %d", b0, l + 1);
		}
	}
	printf("# envelope: rms error first frame worst %.2f dB; settled mean %.2f dB, worst %.2f dB\n", worst_first,
	       sum_settled / n, worst_settled);
	CHECK(worst_first < 6.0, "first frame error %.2f dB", worst_first);
	CHECK(worst_settled < 2.0, "settled error %.2f dB", worst_settled);
	CHECK(sum_settled / n < 1.5, "mean settled error %.2f dB", sum_settled / n);
}

// --- the estimator and the whole encoder, on signals whose parameters are known ---

typedef struct {
	imbe_state_t qs;
	mbe_est_state_t est;
	mbe_params_t p;
	int32_t buf[MBE_BUF_WORDS];
	imbe_enc_mem_t m;
	// a decoder beside it
	imbe_dec_state_t ds;
	mbe_params_t dp;
	mbe_state_t ms;
	mbe_frame_t prev, cur;
	int32_t dbuf[MBE_BUF_WORDS];
	imbe_mem_t d;
} rig_t;

static void rig_init(rig_t *r)
{
	memset(r, 0, sizeof *r);
	r->m = (imbe_enc_mem_t){ &r->qs, &r->est, &r->p, r->buf };
	imbe_enc_init(&r->m);
	r->d = (imbe_mem_t){ &r->ds, &r->dp, { &r->ms, &r->prev, &r->cur, r->dbuf } };
	imbe_dec_init(&r->d);
}

static double gauss(void)
{
	double u = ((rnd() >> 8) + 1.0) / 16777217.0, v = (rnd() >> 8) / 16777216.0;

	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static int frame_b0(const uint8_t f[IMBE_BYTES]) { return (f[0] & 0xFC) | ((f[10] >> 1) & 3); }
static double rms_of(const int16_t *x, int n)
{
	double s = 0;

	for (int i = 0; i < n; i++)
		s += (double)x[i] * x[i];
	return sqrt(s / n);
}

// harmonics of a pitch at every tenth code, falling 6 dB per octave: the pitch found, every band below 3.6 kHz voiced,
// the amplitudes before the quantizer, and the level out of a decoder
static void test_harmonic(void)
{
	rig_t *r = malloc(sizeof *r);
	static int16_t in[40 * MBE_FRAME_LEN], out[40 * MBE_FRAME_LEN];
	double worst_amp = 0, worst_lvl = 0;
	int worst_b0 = 0, unv = 0;

	for (int b0 = 2; b0 < 208; b0 += 10) {
		double period = (b0 + 39.5) / 2, w = 2 * M_PI / period;
		int L = imbe_pitch_harmonics(b0), got = 0;

		rig_init(r);
		for (int n = 0; n < 40 * MBE_FRAME_LEN; n++) {
			double v = 0;

			for (int l = 1; l <= L; l++)
				v += 3000.0 / l * cos(w * l * n + l * l * 0.37);
			in[n] = (int16_t)lrint(v);
		}
		for (int f = 0; f < 40; f++) {
			uint8_t bits[IMBE_BYTES];

			if (f == 20) {		// the estimator alone on one settled frame
				mbe_est_t e;
				mbe_params_t p;

				mbe_est_state_t save = r->est;
				mbe_est_frame(&r->est, in + f * MBE_FRAME_LEN, r->buf, &e);
				memset(&p, 0, sizeof p);
				p.L = (int16_t)L;
				p.w0 = (uint32_t)(4294967296.0 / period);
				mbe_est_params(&r->est, r->buf, &e, &p, 3, L < 37 ? (L + 2) / 3 : 12);
				for (int l = 1; l <= L; l++) {
					double d = fabs(p.a[l - 1] / 2048.0 - log2(3000.0 / l)) * 6.0206;

					if (l * 4000.0 / (period / 2 * L) > 3600)	// near the top the band is cut off
						continue;
					// a period over 85 samples has little more than two periods under the window: its harmonics run into each
					// other, and the estimate ripples by up to 2.5 dB
					CHECK(d < (period > 85 ? 2.5 : 0.5), "b0 %d harmonic %d: amplitude off by %.2f dB", b0, l, d);
					if (d > worst_amp)
						worst_amp = d;
					unv += !p.v[l - 1];
				}
				CHECK(abs(e.period4 - (int)lrint(4 * period)) <= 6, "b0 %d: time-domain period %.2f, true %.2f", b0,
				      e.period4 / 4.0, period);
				r->est = save;
			}
			imbe_enc_frame(&r->m, in + f * MBE_FRAME_LEN, bits);
			imbe_dec_frame(&r->d, bits, 0, out + f * MBE_FRAME_LEN);
			if (f >= 10 && abs(frame_b0(bits) - b0) > got)
				got = abs(frame_b0(bits) - b0);
		}
		if (got > worst_b0)
			worst_b0 = got;
		CHECK(got <= 1, "b0 %d: the encoder's pitch code is off by %d", b0, got);
		{
			double lvl = fabs(20 * log10(rms_of(out + 15 * MBE_FRAME_LEN, 20 * MBE_FRAME_LEN)
						       / rms_of(in + 15 * MBE_FRAME_LEN, 20 * MBE_FRAME_LEN)));

			if (lvl > worst_lvl)
				worst_lvl = lvl;
			CHECK(lvl < 1.5, "b0 %d: level through encoder and decoder off by %.2f dB", b0, lvl);
		}
	}
	printf("# harmonic: pitch code off by at most %d; amplitudes within %.2f dB; level within %.2f dB; %d unvoiced\n",
	       worst_b0, worst_amp, worst_lvl, unv);
	CHECK(unv == 0, "%d harmonics of a periodic signal unvoiced", unv);
	free(r);
}

// white noise: no band voiced, and the level out of a decoder is the level in
static void test_noise(void)
{
	rig_t *r = malloc(sizeof *r);
	static int16_t in[60 * MBE_FRAME_LEN], out[60 * MBE_FRAME_LEN];
	int voiced = 0, total = 0;
	double lvl;

	rig_init(r);
	for (int n = 0; n < 60 * MBE_FRAME_LEN; n++)
		in[n] = (int16_t)lrint(1500 * gauss());
	for (int f = 0; f < 60; f++) {
		uint8_t bits[IMBE_BYTES];
		mbe_params_t p;
		imbe_dec_state_t tmp = r->ds;

		imbe_enc_frame(&r->m, in + f * MBE_FRAME_LEN, bits);
		imbe_unpack(&tmp.q, bits, &p);
		for (int l = 0; l < p.L; l++, total++)
			voiced += p.v[l];
		imbe_dec_frame(&r->d, bits, 0, out + f * MBE_FRAME_LEN);
	}
	lvl = 20 * log10(rms_of(out + 10 * MBE_FRAME_LEN, 50 * MBE_FRAME_LEN) / rms_of(in + 10 * MBE_FRAME_LEN, 50 * MBE_FRAME_LEN));
	printf("# noise: %d of %d harmonics voiced; level through encoder and decoder %+.2f dB\n", voiced, total, lvl);
	CHECK(voiced * 50 < total, "%d of %d harmonics of noise voiced", voiced, total);
	CHECK(fabs(lvl) < 1.5, "noise level off by %.2f dB", lvl);
	free(r);
}

// a voice that starts creaky, every second pulse weaker, and then settles: the pitch search finds twice the period
// at the start, and holds on to it after, since a period's double fits a periodic signal as well as the period. The
// spectrum has to settle it (a harmonic in every band, or in every second one)
static void test_period_not_doubled(void)
{
	rig_t *r = malloc(sizeof *r);
	static int16_t in[40 * MBE_FRAME_LEN];
	int wrong = 0, frames = 0;

	for (int b0 = 23; b0 <= 63; b0 += 4) {		// periods of 31.25 to 51.25 samples
		double period = (b0 + 39.5) / 2, w = 2 * M_PI / period;
		int L = imbe_pitch_harmonics(b0);

		rig_init(r);
		for (int n = 0; n < 40 * MBE_FRAME_LEN; n++) {
			double v = 0;

			for (int l = 1; l <= L; l++) {
				v += 3000.0 / l * cos(w * l * n + l * l * 0.37);
				if (n < 8 * MBE_FRAME_LEN)
					v += 1500.0 / l * cos(w * (l - 0.5) * n + l * 1.3);
			}
			in[n] = (int16_t)lrint(v);
		}
		for (int f = 0; f < 40; f++) {
			uint8_t bits[IMBE_BYTES];

			imbe_enc_frame(&r->m, in + f * MBE_FRAME_LEN, bits);
			if (f >= 12) {
				frames++;
				wrong += abs(frame_b0(bits) - b0) > 1;
			}
		}
	}
	printf("# after a creaky start: %d of %d frames with another pitch\n", wrong, frames);
	CHECK(wrong == 0, "%d of %d frames coded with another pitch (the double period held?)", wrong, frames);
	free(r);
}

// quiet input. Samples of a few units are still sound, not a silence frame; only zeros are. And a quiet hum, one
// harmonic far above the rest with the frame's mean under what the gain code can say, keeps its level: the weak
// harmonics are raised, not the whole frame
static void test_quiet(void)
{
	rig_t *r = malloc(sizeof *r);
	static int16_t in[60 * MBE_FRAME_LEN], out[60 * MBE_FRAME_LEN];
	int silent = 0, not_silent = 0;
	double lvl;

	rig_init(r);
	for (int n = 0; n < 60 * MBE_FRAME_LEN; n++)
		in[n] = (int16_t)lrint(2 * gauss());
	for (int f = 0; f < 60; f++) {
		uint8_t bits[IMBE_BYTES];

		imbe_enc_frame(&r->m, in + f * MBE_FRAME_LEN, bits);
		silent += !memcmp(bits, imbe_silence, IMBE_BYTES);
	}
	CHECK(silent == 0, "%d of 60 frames of quiet noise sent as silence frames", silent);
	memset(in, 0, sizeof in);
	for (int f = 0; f < 60; f++) {
		uint8_t bits[IMBE_BYTES];

		imbe_enc_frame(&r->m, in + f * MBE_FRAME_LEN, bits);
		if (f >= 5)		// the high-pass and the window run out first
			not_silent += !!memcmp(bits, imbe_silence, IMBE_BYTES);
	}
	CHECK(not_silent == 0, "%d of 55 frames of zeros not sent as silence frames", not_silent);

	rig_init(r);
	for (int n = 0; n < 60 * MBE_FRAME_LEN; n++)
		in[n] = (int16_t)lrint(60 * cos(2 * M_PI * 110 * n / 8000.0) + 0.4 * gauss());
	for (int f = 0; f < 60; f++) {
		uint8_t bits[IMBE_BYTES];

		imbe_enc_frame(&r->m, in + f * MBE_FRAME_LEN, bits);
		imbe_dec_frame(&r->d, bits, 0, out + f * MBE_FRAME_LEN);
	}
	lvl = 20 * log10(rms_of(out + 10 * MBE_FRAME_LEN, 50 * MBE_FRAME_LEN) / rms_of(in + 10 * MBE_FRAME_LEN, 50 * MBE_FRAME_LEN));
	printf("# quiet hum: level through encoder and decoder %+.2f dB\n", lvl);
	CHECK(fabs(lvl) < 3, "quiet hum's level off by %.2f dB", lvl);
	free(r);
}

// noise with a weak periodic part under it (a fan, hum): now and then the autocorrelation reaches the mark that keeps
// a pitch speech had, never the one that starts a pitch. No frame gets a voiced band (29 of 100 did with one mark)
static void test_no_voicing_from_hum(void)
{
	rig_t *r = malloc(sizeof *r);
	static int16_t in[100 * MBE_FRAME_LEN];
	int voiced = 0;

	rig_init(r);
	for (int n = 0; n < 100 * MBE_FRAME_LEN; n++) {
		double v = 1000 * gauss();

		for (int l = 1; l <= 6; l++)
			v += 210 * cos(2 * M_PI * 150 * l * n / 8000.0 + l * 1.1);
		in[n] = (int16_t)lrint(v);
	}
	for (int f = 0; f < 100; f++) {
		uint8_t bits[IMBE_BYTES];
		mbe_params_t p;
		imbe_dec_state_t tmp = r->ds;
		int any = 0;

		imbe_enc_frame(&r->m, in + f * MBE_FRAME_LEN, bits);
		imbe_unpack(&tmp.q, bits, &p);
		r->ds = tmp;
		for (int l = 0; l < p.L; l++)
			any |= p.v[l];
		voiced += any;
	}
	printf("# hum under noise: %d of 100 frames with a voiced band\n", voiced);
	CHECK(voiced == 0, "%d of 100 frames of hum under noise got a voiced band", voiced);
	free(r);
}

int main(void)
{
	test_bits_round_trip();
	test_envelope();
	test_harmonic();
	test_noise();
	test_period_not_doubled();
	test_quiet();
	test_no_voicing_from_hum();
	printf("%s: %d checks, %d failures\n", failures ? "imbeEncTest FAILED" : "# imbeEncTest PASSED", checks, failures);
	return failures != 0;
}
