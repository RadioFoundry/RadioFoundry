// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2010 mbelib Author
 * GPG Key ID: 0xEA5EFE2C (9E7A 5527 9CDC EBF7 BF1B  D772 4F98 E863 EA5E FE2C)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
// Copyright (c) 2026 The RadioFoundry Authors
//
// Full-rate vocoder decoder front end (imbe.h), in integers. The dequantization follows TIA-102.BABA: the pitch code,
// the voicing bands, the gain and the DCT blocks of the log amplitudes, and the prediction from the last frame (the
// equations of the log magnitude prediction, with its mean subtracted). Log amplitudes are Q11 throughout the state
// (Q16 inside a frame); cosines come from the core's table.
#include <string.h>
#include "imbe.h"
#include "imbe_q.h"
#include "imbe_tab.h"
#include "mbe_dsp.h"
#include "../dsp/fxp.h"

#define OUT_GAIN	IMBE_OUT_GAIN
#define W0_STEPS	IMBE_W0_STEPS
#define Q16_STEP	IMBE_Q16_STEP

typedef imbe_bits_t bits_t;

// one bit between the frame and a quantizer value: out of the frame, or into it when the frame is being built
static void take(bits_t *c, int b, int pos)
{
	int sh = 7 - (c->i & 7);

	if (c->out)
		c->out[c->i >> 3] |= (uint8_t)(((c->val[b] >> pos) & 1) << sh);
	else
		c->val[b] |= (uint16_t)(((c->in[c->i >> 3] >> sh) & 1) << pos);
	c->i++;
}

static int bit_at(const uint8_t *in, int i) { return (in[i >> 3] >> (7 - (i & 7))) & 1; }

// The frame's bits 6..84 in the order of the standard's prioritized layout: the top three bits of b2; then the bits of
// b3.. by position, highest first, lowest b first; after the 39th of those the K voicing bits and two more of b2; the
// last bit is b2's lowest. nb[b] is the bit count of b3..b(L+1).
void imbe_q_layout(imbe_bits_t *c, int L, int K, const uint8_t *nb)
{
	int n = 0;

	take(c, 2, 5);
	take(c, 2, 4);
	take(c, 2, 3);
	for (int pos = 9; pos >= 0; pos--) {
		for (int b = 3; b <= L + 1; b++) {
			if (nb[b] <= pos)
				continue;
			take(c, b, pos);
			if (++n == 39) {
				for (int k = K - 1; k >= 0; k--)
					take(c, 1, k);
				take(c, 2, 2);
				take(c, 2, 1);
			}
		}
	}
	take(c, 2, 0);
}

static int32_t cos_turn(uint32_t unit, int num) { return mbe_cos_i(unit * (uint32_t)num); }

void imbe_state_reset(imbe_state_t *s)
{
	memset(s, 0, sizeof *s);
	s->L = 30;
}

uint32_t imbe_q_w0(int b0)
{
	// w0 = 4 pi / (b0 + 39.5) rad per sample, as a 2^32 = one turn phase step
	return (0xFFFFFFFFu / (uint32_t)(2 * b0 + 79)) << 2;
}

int imbe_q_alloc(int b0, int *K, uint8_t nb[MBE_MAXL + 2])
{
	int L = (9254 * ((b0 + 40) >> 2)) / 10000;	// harmonics up to 4 kHz
	int L9 = L - 9, off = L9 * (L9 + 5) / 2;

	*K = L < 37 ? (L + 2) / 3 : 12;
	memset(nb, 0, MBE_MAXL + 2);
	for (int m = 0; m < 5; m++)
		nb[3 + m] = imbe_ba_bits[L9 * 5 + m];
	for (int b = 8; b <= L + 1; b++, off++)
		nb[b] = (imbe_hoba[off >> 1] >> ((off & 1) * 4)) & 15;
	return L;
}

void imbe_q_predict(imbe_state_t *s, int L, int32_t pr[MBE_MAXL + 1])
{
	int32_t rho, rcp, sum = 0, S;
	int rem = 0, ik = 0;
	int32_t *ya = pr;

	// prediction from the last frame: its log amplitudes resampled to this frame's harmonic count (linear between
	// neighbors), scaled by rho, the mean of the prediction removed
	if (L <= 15)
		rho = 13107;
	else if (L <= 24)
		rho = ((30 * L - 50) * 32768 + 500) / 1000;
	else
		rho = 22938;
	if (L > s->L)
		for (int l = s->L + 1; l <= L; l++)
			s->lg[l] = s->lg[s->L];
	rcp = (int32_t)((1u << 30) / (uint32_t)L);
	for (int l = 1; l <= L; l++) {
		int32_t p0, p1, d;

		rem += s->L;
		while (rem >= L) {
			rem -= L;
			ik++;
		}
		p0 = s->lg[ik];
		p1 = s->lg[ik + 1];
		d = fxp_mulshr(rem, rcp, 15);		// rem / L, Q15 (rem < L: rem * rcp < 2^30)
		ya[l] = p0 * 32 + fxp_mulshr((p1 - p0) * 32, d, 15);
		sum += ya[l];
	}
	S = fxp_mulshr(fxp_mulshr(sum, rho, 15), rcp, 30);
	for (int l = 1; l <= L; l++)
		pr[l] = fxp_mulshr(ya[l], rho, 15) - S;
}

int imbe_unpack(imbe_state_t *s, const uint8_t imbe[IMBE_BYTES], mbe_params_t *p)
{
	int b0 = (((imbe[0] >> 2) & 0x3F) << 2) | (bit_at(imbe, 85) << 1) | bit_at(imbe, 86);
	int L, L9, K, q, r, e = 0;
	bits_t c;
	uint8_t nb[MBE_MAXL + 2];
	int32_t G[7], R[7], T[MBE_MAXL + 1];
	int32_t ya[MBE_MAXL + 1];

	if (b0 >= W0_STEPS)
		return (b0 >= 216 && b0 <= 219) ? IMBE_FRAME_SILENCE : IMBE_FRAME_INVALID;
	// pitch: w0 = 4 pi / (b0 + 39.5) rad per sample, as a 2^32 = one turn phase step; L harmonics up to 4 kHz
	p->w0 = imbe_q_w0(b0);
	L = imbe_q_alloc(b0, &K, nb);
	L9 = L - 9;
	p->L = (int16_t)L;
	p->flags = 0;

	// the quantizer values
	memset(&c, 0, sizeof c);
	c.in = imbe;
	c.i = 6;
	imbe_q_layout(&c, L, K, nb);

	// voicing: one bit for each three harmonics, the lowest band first
	{
		int j = 1, k = K - 1;

		for (int l = 1; l <= L; l++) {
			p->v[l - 1] = (c.val[1] >> k) & 1;
			if (j == 3) {
				j = 1;
				if (k > 0)
					k--;
			} else {
				j++;
			}
		}
	}

	// gain: G1 from the codebook, G2..G6 uniform; Q16
	G[1] = ((int32_t)imbe_b2[c.val[2]] - 3 * 4096) * 16;
	for (int m = 2; m <= 6; m++) {
		int bits = imbe_ba_bits[L9 * 5 + m - 2];
		int32_t lev = 2 * (int32_t)c.val[m + 1] - (1 << bits) + 1;

		G[m] = fxp_mulshr(lev * (int32_t)imbe_ba_step[L9 * 5 + m - 2], Q16_STEP, 16);
	}
	// inverse DCT of G into the six blocks' first coefficients
	for (int i = 1; i <= 6; i++) {
		int32_t sm = 0;

		for (int m = 1; m <= 6; m++)
			sm += (m == 1 ? 1 : 2) * fxp_mulshr(G[m], cos_turn(178956971u, (m - 1) * (2 * i - 1)), 15);
		R[i] = sm;
	}
	// the blocks' other coefficients, each block's inverse DCT into the residual T_l
	q = L / 6;
	r = L % 6;
	{
		int l = 1;

		for (int i = 1; i <= 6; i++) {
			int J = i <= 6 - r ? q : q + 1;
			int32_t C[11];
			uint32_t unit = (1u << 30) / (uint32_t)J;

			C[1] = R[i];
			for (int k = 2; k <= J; k++, e++) {
				int bits = nb[8 + e];

				if (!bits) {
					C[k] = 0;
				} else {
					int32_t lev = 2 * (int32_t)c.val[8 + e] - (1 << bits) + 1;

					C[k] = fxp_mulshr(lev * imbe_qstep[bits - 1] * (int32_t)imbe_sdev[k - 2], Q16_STEP, 16);
				}
			}
			for (int j = 1; j <= J; j++) {
				int32_t sm = 0;

				for (int k = 1; k <= J; k++)
					sm += (k == 1 ? 1 : 2) * fxp_mulshr(C[k], cos_turn(unit, (k - 1) * (2 * j - 1)), 15);
				T[l++] = sm;
			}
		}
	}

	imbe_q_predict(s, L, ya);
	for (int l = 1; l <= L; l++) {
		int32_t y = T[l] + ya[l];

		p->a[l - 1] = (int16_t)fxp_clamp((y + 16) >> 5, -32767, 32767);
	}
	s->L = (int16_t)L;
	s->lg[0] = 0;
	for (int l = 1; l <= L; l++)
		s->lg[l] = p->a[l - 1];
	return IMBE_FRAME_OK;
}

const uint8_t imbe_silence[IMBE_BYTES] = { 0xD8 };

// The decoder's handling of a channel with errors, after the vocoder's published description (the expired US 5,870,405,
// equations 52 to 74): a running rate of corrected bits, a running frame energy, and from them a level above which a
// harmonic is taken as voiced whatever its bit says, and a limit on the sum of the amplitudes
#define ER_STEP		5973		// 0.000356 per corrected bit, Q24; the rate keeps 0.95 of itself per frame
#define ER_CLEAN	83886		// 0.005: at or under this (and few bits in the frame) nothing is smoothed
#define ER_SOME		209715		// 0.0125
#define ER_MUTE		1426063		// 0.085: over this the output is muted
#define ENERGY_MIN	10000u
#define ENERGY_INIT	75000u
#define AMP_MAX_CLEAN	20480

void imbe_dec_init(const imbe_mem_t *m)
{
	imbe_state_reset(&m->s->q);
	m->s->err_rate = 0;
	m->s->energy = ENERGY_INIT;
	m->s->amp_max = AMP_MAX_CLEAN;
	mbe_synth_init(&m->mbe);
}

// A decoded frame, model units: the energy, the enhancement, then the smoothing
static void enhance_and_smooth(imbe_dec_state_t *s, mbe_params_t *p, int total, int c4)
{
	uint32_t r0 = 0, am = 0;
	int L = p->L;

	for (int l = 0; l < L; l++)
		r0 += (uint32_t)mbe_exp2_q11(fxp_clamp(2 * p->a[l], -32768, 26 * 2048));
	s->energy = s->energy - s->energy / 20 + r0 / 20;
	if (s->energy < ENERGY_MIN)
		s->energy = ENERGY_MIN;
	mbe_enhance(p->a, p->a, L, p->w0);

	// voicing: V = 45.255 E^0.375 / exp(277.26 rate) with some errors, 1.414 E^0.375 with more; as log2
	if (s->err_rate > ER_CLEAN || total > 4) {
		int32_t lv = (3 * mbe_log2_q11(s->energy)) >> 3;

		lv += s->err_rate <= ER_SOME && !c4 ? 11264 - (int32_t)((400u * s->err_rate) >> 13) : 1024;
		for (int l = 0; l < L; l++)
			if (p->a[l] > lv)
				p->v[l] = 1;
	}
	// amplitudes: their sum is held to a limit that is 20480 on a clean channel
	if (s->err_rate <= ER_CLEAN && total <= 6)
		s->amp_max = AMP_MAX_CLEAN;
	else
		s->amp_max = (int32_t)fxp_clamp(s->amp_max + 6000 - 300 * total, 1, 1 << 26);
	for (int l = 0; l < L; l++)
		am += (uint32_t)mbe_exp2_q11(fxp_clamp(p->a[l] + 4 * 2048, -32768, 24 * 2048));	// Q4
	if (((uint32_t)s->amp_max << 4) <= am) {
		int32_t lg = mbe_log2_q11((uint32_t)s->amp_max << 4) - mbe_log2_q11(am);

		for (int l = 0; l < L; l++)
			p->a[l] = (int16_t)fxp_clamp(p->a[l] + lg, -32767, 32767);
	}
}

int imbe_dec_frame(const imbe_mem_t *m, const uint8_t imbe[IMBE_BYTES], uint8_t errs, int16_t pcm[MBE_FRAME_LEN])
{
	imbe_dec_state_t *s = m->s;
	mbe_params_t *p = m->p;
	int st = IMBE_FRAME_INVALID, run, mute;

	// a frame that never came says nothing about the channel
	if (errs != IMBE_ERRS_LOST) {
		s->err_rate = s->err_rate - s->err_rate / 20 + ER_STEP * (uint32_t)IMBE_ERRS_TOTAL(errs);
		if (!IMBE_ERRS_REPEAT(errs))
			st = imbe_unpack(&s->q, imbe, p);
	}
	if (st == IMBE_FRAME_SILENCE || s->err_rate > ER_MUTE) {
		// A silence frame is not a loss: the speech stops, and nothing is predicted from what was before it.
		// A channel this bad is muted the same way, whatever decodes, and counted as bad frames
		mute = st != IMBE_FRAME_SILENCE;
		memset(p, 0, sizeof *p);
		p->w0 = m->mbe.prev->w0;
		imbe_state_reset(&s->q);
		mbe_synth_frame(&m->mbe, p, 0, pcm);
		return mute ? MBE_REPEAT_MAX + 1 : 0;
	}
	if (st == IMBE_FRAME_OK) {
		enhance_and_smooth(s, p, IMBE_ERRS_TOTAL(errs), IMBE_ERRS_C4(errs));
		p->flags |= MBE_NO_ENHANCE;
		for (int l = 0; l < p->L; l++)
			p->a[l] = (int16_t)fxp_clamp(p->a[l] + OUT_GAIN, -32767, 32767);
		return mbe_synth_frame(&m->mbe, p, 0, pcm);
	}
	run = mbe_synth_frame(&m->mbe, NULL, 1, pcm);
	if (run > MBE_REPEAT_MAX)
		imbe_state_reset(&s->q);
	return run;
}
