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
// Half-rate vocoder decoder front end (ambe.h), in integers, after mbelib's: the frame's FEC (the first Golay word,
// the second whitened by a sequence seeded from the first's data), the pitch code, the voicing codebook over 500 Hz
// bands, the gain as a step on half the last frame's, the PRBA vectors (an 8-point DCT giving each of the four residual
// blocks its first two coefficients), the higher-order coefficients, each block's inverse DCT, and the prediction from
// the last frame's log amplitudes. Log amplitudes are log2, Q11.
#include <string.h>
#include "ambe.h"
#include "ambe_q.h"
#include "ambe_tab.h"
#include "mbe_dsp.h"
#include "../dsp/fxp.h"
#include "../fec/golay.h"

// ── d0..d48 ──
static int dbit(const uint8_t *d, int i) { return (d[i >> 3] >> (7 - (i & 7))) & 1; }

static void dset(uint8_t *d, int i, int b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7));

	d[i >> 3] = (uint8_t)(b ? d[i >> 3] | m : d[i >> 3] & ~m);
}

// each value's bits, most significant first (mbelib's layout). b8's lowest bit is not sent
static const uint8_t q_pos[AMBE_Q_N][9] = {
	{ 0, 1, 2, 3, 4, 5, 48 },
	{ 38, 39, 40, 41 },
	{ 6, 7, 8, 9, 42, 43 },
	{ 10, 11, 12, 13, 14, 15, 16, 44, 45 },
	{ 17, 18, 19, 20, 21, 46, 47 },
	{ 22, 23, 25, 26 },
	{ 27, 28, 29, 30 },
	{ 31, 32, 33, 34 },
	{ 35, 36, 37 },
};
static const uint8_t q_len[AMBE_Q_N] = { 7, 4, 6, 9, 7, 4, 4, 4, 3 };

int ambe_q_get(const uint8_t d[AMBE_DATA_BYTES], int b)
{
	int v = 0;

	for (int i = 0; i < q_len[b]; i++)
		v = (v << 1) | dbit(d, q_pos[b][i]);
	return b == AMBE_Q_B8 ? v << 1 : v;
}

void ambe_q_put(uint8_t d[AMBE_DATA_BYTES], int b, int v)
{
	int n = q_len[b];

	if (b == AMBE_Q_B8)
		v >>= 1;
	for (int i = 0; i < n; i++)
		dset(d, q_pos[b][i], (v >> (n - 1 - i)) & 1);
}

int ambe_q_band(uint32_t w0, int l)
{
	int j = (int)(((w0 >> 4) * (uint32_t)l) >> 24);	// l f0 / 500 Hz, f0 = w0 / 2^32 * 8000

	return j > 7 ? 7 : j;
}

int32_t ambe_q_cos(uint32_t unit, int num) { return mbe_cos_i(unit * (uint32_t)num); }

void ambe_state_reset(ambe_state_t *s)
{
	memset(s, 0, sizeof *s);
	s->L = 30;
}

#define RHO	21299	// 0.65, Q15: how much of the last frame's amplitudes is predicted

int32_t ambe_q_predict(ambe_state_t *s, int L, int32_t pr[MBE_MAXL + 1])
{
	int pL = s->L, rem = 0, ik = 0;
	int32_t sum = 0;

	// the last frame's amplitudes past its own count are its last one's (the one after it is read with no weight)
	for (int l = pL + 1; l <= (L > pL ? L : pL) + 1; l++)
		s->lg[l] = s->lg[pL];
	s->lg[0] = s->lg[1];
	for (int l = 1; l <= L; l++) {
		int32_t p0, p1;

		rem += pL;			// harmonic l falls at l * pL / L of the last frame's
		while (rem >= L) {
			rem -= L;
			ik++;
		}
		p0 = s->lg[ik];
		p1 = s->lg[ik + 1];
		pr[l] = fxp_mulshr(p0 + (p1 - p0) * rem / L, RHO, 15);
		sum += pr[l];
	}
	return sum / L;
}

// ── FEC ──
uint8_t ambe_fec_decode(const uint8_t air[AMBE_BYTES], uint8_t d[AMBE_DATA_BYTES])
{
	uint32_t w[4] = { 0, 0, 0, 0 }, pr;
	uint16_t d0, d1;
	int e0, e1;

	for (int i = 0; i < AMBE_FRAME_BITS; i++) {
		uint8_t c = ambe_ilv[i];

		w[c >> 5] |= (uint32_t)((air[i >> 3] >> (i & 7)) & 1) << (c & 31);
	}
	e0 = fec_golay24_decode(w[0], &d0);
	if (e0 < 0) {
		// four errors seen: the (23,12) word inside it gives its nearest data, counted as four
		(void)fec_golay23_decode(w[0] >> 1, &d0);
		e0 = 4;
	}
	// c1 is whitened bit by bit, its highest bit first, by a linear congruential sequence seeded from c0's data
	pr = 16u * d0;
	for (int k = 1; k <= 23; k++) {
		pr = (173u * pr + 13849u) & 0xFFFFu;
		w[1] ^= (pr >> 15) << (23 - k);
	}
	e1 = fec_golay23_decode(w[1] & 0x7FFFFFu, &d1);
	memset(d, 0, AMBE_DATA_BYTES);
	for (int k = 0; k < 12; k++) {
		dset(d, k, (d0 >> (11 - k)) & 1);
		dset(d, 12 + k, (d1 >> (11 - k)) & 1);
	}
	for (int k = 0; k < 11; k++)
		dset(d, 24 + k, (int)(w[2] >> (10 - k)) & 1);
	for (int k = 0; k < 14; k++)
		dset(d, 35 + k, (int)(w[3] >> (13 - k)) & 1);
	return AMBE_ERRS(e0, e1);
}

void ambe_fec_encode(const uint8_t d[AMBE_DATA_BYTES], uint8_t air[AMBE_BYTES])
{
	uint32_t w[4] = { 0, 0, 0, 0 }, pr;
	uint16_t d0 = 0, d1 = 0;

	for (int k = 0; k < 12; k++) {
		d0 = (uint16_t)(d0 << 1 | dbit(d, k));
		d1 = (uint16_t)(d1 << 1 | dbit(d, 12 + k));
	}
	for (int k = 0; k < 11; k++)
		w[2] = w[2] << 1 | (uint32_t)dbit(d, 24 + k);
	for (int k = 0; k < 14; k++)
		w[3] = w[3] << 1 | (uint32_t)dbit(d, 35 + k);
	w[0] = fec_golay24_encode(d0);
	w[1] = fec_golay23_encode(d1);
	pr = 16u * d0;
	for (int k = 1; k <= 23; k++) {
		pr = (173u * pr + 13849u) & 0xFFFFu;
		w[1] ^= (pr >> 15) << (23 - k);
	}
	memset(air, 0, AMBE_BYTES);
	for (int i = 0; i < AMBE_FRAME_BITS; i++) {
		uint8_t c = ambe_ilv[i];

		air[i >> 3] |= (uint8_t)(((w[c >> 5] >> (c & 31)) & 1) << (i & 7));
	}
}

// ── the dequantizer ──
int ambe_unpack(ambe_state_t *s, const uint8_t d[AMBE_DATA_BYTES], mbe_params_t *p)
{
	int b0 = ambe_q_get(d, AMBE_Q_B0), L, vm, l = 1;
	int32_t G[9], R[9], C[AMBE_BLOCKS][AMBE_JMAX + 1], T[MBE_MAXL + 1], pr[MBE_MAXL + 1];
	int32_t gamma, mean_pr, sum_t = 0, big;
	const uint8_t *J;

	if (b0 >= AMBE_B0_VOICE)
		return b0 >= AMBE_B0_SILENCE ? AMBE_FRAME_SILENCE : AMBE_FRAME_INVALID;
	L = ambe_L[b0];
	J = ambe_J[L - 9];
	p->L = (int16_t)L;
	p->w0 = ambe_w0[b0];
	p->flags = 0;

	vm = ambe_vuv[ambe_q_get(d, AMBE_Q_B1)];
	for (int i = 1; i <= L; i++)
		p->v[i - 1] = (uint8_t)((vm >> ambe_q_band(p->w0, i)) & 1);

	// the gain: a step on half the last frame's
	gamma = ambe_dg[ambe_q_get(d, AMBE_Q_B2)] + s->gamma / 2;

	// the PRBA vector (G1 = 0: the gain carries the mean) and its inverse DCT, R1..R8
	G[1] = 0;
	for (int m = 0; m < 3; m++)
		G[2 + m] = ambe_prba24[ambe_q_get(d, AMBE_Q_B3)][m];
	for (int m = 0; m < 4; m++)
		G[5 + m] = ambe_prba58[ambe_q_get(d, AMBE_Q_B4)][m];
	for (int i = 1; i <= 8; i++) {
		int32_t sm = 0;

		for (int m = 1; m <= 8; m++)
			sm += (m == 1 ? 1 : 2) * fxp_mulshr(G[m], ambe_q_cos(1u << 27, (m - 1) * (2 * i - 1)), 15);
		R[i] = sm;
	}
	// each block: its first two coefficients from a pair of R, the next up to four from its codebook, the rest 0
	for (int i = 0; i < AMBE_BLOCKS; i++) {
		const int16_t *h = ambe_hoc[i][ambe_q_get(d, AMBE_Q_B5 + i)];

		C[i][1] = (R[2 * i + 1] + R[2 * i + 2]) / 2;
		C[i][2] = fxp_mulshr(R[2 * i + 1] - R[2 * i + 2], 11585, 15);	// 1 / (2 sqrt 2)
		for (int k = 3; k <= J[i]; k++)
			C[i][k] = k <= 6 ? h[k - 3] : 0;
	}
	for (int i = 0; i < AMBE_BLOCKS; i++) {
		uint32_t unit = (1u << 30) / J[i];

		for (int j = 1; j <= J[i]; j++, l++) {
			int32_t sm = 0;

			for (int k = 1; k <= J[i]; k++)
				sm += (k == 1 ? 1 : 2) * fxp_mulshr(C[i][k], ambe_q_cos(unit, (k - 1) * (2 * j - 1)), 15);
			T[l] = sm;
			sum_t += sm;
		}
	}

	// the amplitudes: the residual, the prediction less its mean, and the gain less half of log2 L and the
	// residual's mean
	mean_pr = ambe_q_predict(s, L, pr);
	big = gamma - mbe_log2_q11((uint32_t)L) / 2 - sum_t / L;
	for (int i = 1; i <= L; i++)
		p->a[i - 1] = (int16_t)fxp_clamp(T[i] + pr[i] - mean_pr + big, -32767, 32767);
	s->L = (int16_t)L;
	s->gamma = (int16_t)fxp_clamp(gamma, -32767, 32767);
	for (int i = 1; i <= L; i++)
		s->lg[i] = p->a[i - 1];
	return AMBE_FRAME_OK;
}

const uint8_t ambe_silence[AMBE_BYTES] = { 0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8 };

// The channel: a running rate of the bits the two Golay words corrected, of the 47 they protect
#define ER_STEP		17848		// 0.05 / 47 per corrected bit, Q24; the rate keeps 0.95 of itself per frame
#define ER_MUTE		1677722		// 0.1: over this the output is muted

void ambe_dec_init(const ambe_mem_t *m)
{
	ambe_state_reset(&m->s->q);
	m->s->err_rate = 0;
	mbe_synth_init(&m->mbe);
}

int ambe_dec_data(const ambe_mem_t *m, const uint8_t d[AMBE_DATA_BYTES], uint8_t errs, int16_t pcm[MBE_FRAME_LEN])
{
	ambe_dec_state_t *s = m->s;
	mbe_params_t *p = m->p;
	int st = AMBE_FRAME_INVALID, run;

	if (errs != AMBE_ERRS_LOST) {
		s->err_rate = s->err_rate - s->err_rate / 20 + ER_STEP * (uint32_t)AMBE_ERRS_TOTAL(errs);
		if (!AMBE_ERRS_REPEAT(errs))
			st = ambe_unpack(&s->q, d, p);
	}
	if (st == AMBE_FRAME_SILENCE || s->err_rate > ER_MUTE) {
		// silence (or a tone, which is not synthesized): the speech stops, nothing is predicted from before it.
		// A channel this bad is muted the same way and counted as bad frames
		int mute = st != AMBE_FRAME_SILENCE;

		memset(p, 0, sizeof *p);
		p->w0 = m->mbe.prev->w0;
		ambe_state_reset(&s->q);
		mbe_synth_frame(&m->mbe, p, 0, pcm);
		return mute ? MBE_REPEAT_MAX + 1 : 0;
	}
	if (st == AMBE_FRAME_OK) {
		mbe_enhance(p->a, p->a, p->L, p->w0);
		p->flags |= MBE_NO_ENHANCE;
		for (int l = 0; l < p->L; l++)
			p->a[l] = (int16_t)fxp_clamp(p->a[l] + AMBE_OUT_GAIN, -32767, 32767);
		return mbe_synth_frame(&m->mbe, p, 0, pcm);
	}
	run = mbe_synth_frame(&m->mbe, NULL, 1, pcm);
	if (run > MBE_REPEAT_MAX)
		ambe_state_reset(&s->q);
	return run;
}

int ambe_dec_frame(const ambe_mem_t *m, const uint8_t air[AMBE_BYTES], int16_t pcm[MBE_FRAME_LEN])
{
	uint8_t d[AMBE_DATA_BYTES];
	uint8_t e = ambe_fec_decode(air, d);

	return ambe_dec_data(m, d, e, pcm);
}
