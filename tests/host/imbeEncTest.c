// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The full-rate vocoder's encoder (core/mbe): the quantizer against the dequantizer.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/mbe/imbe.h"

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

int main(void)
{
	test_bits_round_trip();
	test_envelope();
	printf("%s: %d checks, %d failures\n", failures ? "imbeEncTest FAILED" : "# imbeEncTest PASSED", checks, failures);
	return failures != 0;
}
