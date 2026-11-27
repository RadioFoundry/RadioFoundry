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

// The gain code sets the mean of the frame's residual (the mean of its six blocks' means), and its lowest value is
// A_MEAN_MIN. A quiet frame with a few harmonics far above the rest (hum, rumble) has a mean below that: coded as it
// is, the gain code would stop at its lowest value and lift every harmonic by the difference, the strong ones too.
// So the weak harmonics are raised to a common floor instead, the lowest one that brings the mean up to what the
// gain code can say. A frame a dequantizer made is never changed: its mean is a gain code's
#define A_MEAN_MIN	(-5824)		// log2 Q11: the gain codebook's first entry, -2.842, less the rounding of a frame

// six times the mean of the block means of v[0..L-1], each value at least f, times q (q + 1) to stay in integers
#define BLOCK_MEAN(sum, L, expr) do { \
	int q_ = (L) / 6, r_ = (L) % 6, l = 0; \
	for (int i_ = 1; i_ <= 6; i_++) { \
		int J_ = i_ <= 6 - r_ ? q_ : q_ + 1; \
		int32_t blk_ = 0; \
		for (int j_ = 0; j_ < J_; j_++, l++) \
			blk_ += (expr); \
		(sum) += blk_ * (J_ == q_ ? q_ + 1 : q_); \
	} \
} while (0)

// pr: what the last frame predicts for harmonics 1..L (Q16), the part of the residual the amplitudes do not set
static void lift_floor(mbe_params_t *p, int L, const int32_t *pr)
{
	int32_t unit = 6 * (L / 6) * (L / 6 + 1), want = 0, have = 0, lo = -32768, hi;

	BLOCK_MEAN(want, L, pr[l + 1] >> 5);
	want += unit * A_MEAN_MIN;
	BLOCK_MEAN(have, L, p->a[l]);
	if (have >= want)
		return;
	hi = want / unit + 2;			// with every amplitude at this, the mean is enough
	while (hi - lo > 4) {			// and the mean rises with the floor
		int32_t f = (lo + hi) >> 1;

		have = 0;
		BLOCK_MEAN(have, L, p->a[l] > f ? p->a[l] : f);
		if (have >= want)
			hi = f;
		else
			lo = f;
	}
	for (int l = 0; l < L; l++)
		if (p->a[l] < hi)
			p->a[l] = (int16_t)fxp_clamp(hi, -32767, 32767);
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
	lift_floor(p, L, T);
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

// the code nearest a period in quarter samples: the period is (b0 + 39.5) / 2 samples
static int code_of(int period4) { return (period4 - 79 + 1) >> 1; }
static const mbe_grid_t grid = { code_of, imbe_q_w0, IMBE_W0_STEPS, 79 };

void imbe_enc_init(const imbe_enc_mem_t *m)
{
	imbe_state_reset(m->s);
	mbe_est_init(m->est);
}

void imbe_enc_frame(const imbe_enc_mem_t *m, const int16_t pcm[MBE_FRAME_LEN], uint8_t out[IMBE_BYTES])
{
	mbe_params_t *p = m->p;
	mbe_est_t e;
	int b0, K;

	mbe_est_frame(m->est, pcm, m->buf, &e);
	if (e.zero) {
		// nothing but zeros: a silence frame, and nothing is predicted from what was before it
		memcpy(out, imbe_silence, IMBE_BYTES);
		imbe_state_reset(m->s);
		m->est->vprev = 0;
		return;
	}
	b0 = mbe_est_pitch(m->est, m->buf, &e, &grid);
	if (b0 < 0)
		b0 = B0_UNVOICED;
	memset(p, 0, sizeof *p);
	p->w0 = imbe_q_w0(b0);
	p->L = (int16_t)imbe_pitch_harmonics(b0);
	K = p->L < 37 ? (p->L + 2) / 3 : 12;
	mbe_est_params(m->est, m->buf, &e, p, 3, K);
	for (int l = 0; l < p->L; l++)		// PCM units to the model's
		p->a[l] = (int16_t)fxp_clamp(p->a[l] - IMBE_OUT_GAIN, -8 * 2048, 32767);
	imbe_pack(m->s, b0, p, out);
}
