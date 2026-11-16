// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Full-rate vocoder quantizer (imbe.h): a frame's model parameters to its 88 bits. The inverse of the dequantizer in
// imbe_dec.c, step for step, after TIA-102.BABA: the prediction residual of the log amplitudes in six blocks, each
// block's DCT, the DCT of the six block means (the gain vector), the scalar quantizers, and the bit order. The frame
// built is then run through the dequantizer itself, so the prediction state is exactly the receiver's.
#include <string.h>
#include "imbe.h"
#include "imbe_q.h"
#include "imbe_tab.h"
#include "mbe_dsp.h"
#include "mbe_est.h"
#include "../dsp/fxp.h"

static int32_t cos_turn(uint32_t unit, int num) { return mbe_cos_i(unit * (uint32_t)num); }

// The code b of a uniform quantizer of 2^bits levels for x (Q16): the level nearest to x, found against the same
// level values the dequantizer computes (the border between b and b + 1 is the even multiple of the half step between
// their levels)
static uint16_t quant(int32_t x, int bits, int32_t step)
{
	int lo = 0, hi = (1 << bits) - 1;

	while (lo < hi) {
		int mid = (lo + hi) >> 1;

		if (x >= imbe_q_level(2 * mid - (1 << bits) + 2, step))
			lo = mid + 1;
		else
			hi = mid;
	}
	return (uint16_t)lo;
}

int imbe_pitch_harmonics(int b0)
{
	return (9254 * ((b0 + 40) >> 2)) / 10000;
}

void imbe_pack(imbe_state_t *s, int b0, mbe_params_t *p, uint8_t out[IMBE_BYTES])
{
	imbe_bits_t c;
	uint8_t nb[MBE_MAXL + 2];
	int32_t T[MBE_MAXL + 1], R[7];
	int L, L9, K, q, r, e = 0, l = 1;

	b0 = (int)fxp_clamp(b0, 0, IMBE_W0_STEPS - 1);
	L = imbe_q_alloc(b0, &K, nb);
	L9 = L - 9;
	memset(&c, 0, sizeof c);
	memset(out, 0, IMBE_BYTES);
	c.out = out;
	c.i = 6;

	// voicing: a band is three harmonics (the last one takes what is left), the lowest band in the highest bit
	for (int k = 0; k < K; k++)
		if (p->v[3 * k])
			c.val[1] |= (uint16_t)(1u << (K - 1 - k));

	// the residual: the log amplitudes less what the last frame predicts
	imbe_q_predict(s, L, T);
	for (int i = 1; i <= L; i++)
		T[i] = p->a[i - 1] * 32 - T[i];

	// each block's DCT; the coefficients after the first are quantized here
	q = L / 6;
	r = L % 6;
	for (int i = 1; i <= 6; i++) {
		int J = i <= 6 - r ? q : q + 1;
		uint32_t unit = (1u << 30) / (uint32_t)J;
		int32_t rcp = (int32_t)(65536u / (uint32_t)J);

		for (int k = 1; k <= J; k++) {
			int32_t sm = 0, C;

			for (int j = 1; j <= J; j++)
				sm += fxp_mulshr(T[l + j - 1], cos_turn(unit, (k - 1) * (2 * j - 1)), 15);
			C = fxp_mulshr(sm, rcp, 16);
			if (k == 1) {
				R[i] = C;
			} else {
				int bits = nb[8 + e];

				if (bits)
					c.val[8 + e] = quant(C, bits, imbe_qstep[bits - 1] * (int32_t)imbe_sdev[k - 2]);
				e++;
			}
		}
		l += J;
	}

	// the gain vector: the DCT of the six block means. The first from the codebook, the others uniform
	for (int m = 1; m <= 6; m++) {
		int32_t sm = 0, G;

		for (int i = 1; i <= 6; i++)
			sm += fxp_mulshr(R[i], cos_turn(178956971u, (m - 1) * (2 * i - 1)), 15);
		G = sm / 6;
		if (m == 1) {
			int32_t want = G / 16 + 3 * 4096, best = INT32_MAX;

			for (int i = 0; i < 64; i++) {
				int32_t d = want - (int32_t)imbe_b2[i];

				if (d < 0)
					d = -d;
				if (d < best) {
					best = d;
					c.val[2] = (uint16_t)i;
				}
			}
		} else {
			int bits = imbe_ba_bits[L9 * 5 + m - 2];

			if (bits)
				c.val[m + 1] = quant(G, bits, imbe_ba_step[L9 * 5 + m - 2]);
		}
	}

	// the frame: the pitch code's top six bits, the prioritized bits, its low two; the last bit stays 0
	out[0] = (uint8_t)((b0 >> 2) << 2);
	imbe_q_layout(&c, L, K, nb);
	out[10] |= (uint8_t)((((b0 >> 1) & 1) << 2) | ((b0 & 1) << 1));
	// advance the state as the receiver's dequantizer will, and hand back what it reconstructs
	(void)imbe_unpack(s, out, p);
}

// --- speech to frames ---

#define B0_UNVOICED	100	// the pitch code of a frame with no pitch: 32 harmonics, a band every 94 Hz
#define B0_SEARCH	3	// pitch codes tried to each side of the time-domain estimate (1.5 samples of period)
#define B0_STAY		150	// mismatch (Q15) a code must gain for each step away from that estimate: at long periods the
				// spectrum hardly tells neighbors apart and the time-domain estimate is the better one
#define PERIODIC_MIN	(-2700)	// log2 Q11 (0.4): a normalized autocorrelation under this is no pitch

void imbe_enc_init(const imbe_enc_mem_t *m)
{
	imbe_state_reset(m->s);
	mbe_est_init(m->est);
}

void imbe_enc_frame(const imbe_enc_mem_t *m, const int16_t pcm[MBE_FRAME_LEN], uint8_t out[IMBE_BYTES])
{
	mbe_params_t *p = m->p;
	mbe_est_t e;
	int b0 = B0_UNVOICED, K;

	mbe_est_frame(m->est, pcm, m->buf, &e);
	if (!e.period4 && e.periodic <= -32768 && !e.exp) {
		// nothing but zeros: a silence frame, and nothing is predicted from what was before it
		memcpy(out, imbe_silence, IMBE_BYTES);
		imbe_state_reset(m->s);
		m->est->vprev = 0;
		return;
	}
	if (e.period4 && e.periodic >= PERIODIC_MIN) {
		// the pitch code nearest the estimate (period = (b0 + 39.5) / 2 samples), then the best of its neighbors
		int c = (e.period4 - 79 + 1) >> 1, best = 0x7FFFFFFF;

		for (int t = c - B0_SEARCH; t <= c + B0_SEARCH; t++) {
			int mis;

			if (t < 0 || t >= IMBE_W0_STEPS)
				continue;
			mis = mbe_est_mismatch(m->buf, imbe_q_w0(t)) + B0_STAY * (t > c ? t - c : c - t);
			if (mis < best) {
				best = mis;
				b0 = t;
			}
		}
	} else {
		e.period4 = 0;
	}
	memset(p, 0, sizeof *p);
	p->w0 = imbe_q_w0(b0);
	p->L = (int16_t)imbe_pitch_harmonics(b0);
	K = p->L < 37 ? (p->L + 2) / 3 : 12;
	mbe_est_params(m->est, m->buf, &e, p, 3, K);
	for (int l = 0; l < p->L; l++)		// PCM units to the model's
		p->a[l] = (int16_t)fxp_clamp(p->a[l] - IMBE_OUT_GAIN, -8 * 2048, 32767);
	imbe_pack(m->s, b0, p, out);
}
