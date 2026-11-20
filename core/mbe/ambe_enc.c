// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Half-rate vocoder quantizer and encoder (ambe.h): a frame's model parameters to d0..d48, the inverse of the
// dequantizer in ambe_dec.c step for step. The log amplitudes less the prediction from the last frame give the gain
// (their mean) and the residual; the residual's four blocks each take a DCT, whose first two coefficients over the
// blocks make the 8-point PRBA vector and the next four the higher-order coefficients. The vectors are found by a full
// search of their codebooks, the gain step by the nearest value. The frame built is then run through the dequantizer
// itself, so the prediction state is exactly the receiver's.
#include <string.h>
#include "ambe.h"
#include "ambe_q.h"
#include "ambe_tab.h"
#include "mbe_dsp.h"
#include "mbe_est.h"
#include "../dsp/fxp.h"

#define SQRT2		46341	// Q15

static int nearest_dg(int32_t x)
{
	int best = 0;
	int32_t bd = INT32_MAX;

	for (int i = 0; i < 64; i++) {
		int32_t dd = x - ambe_dg[i];

		dd = dd < 0 ? -dd : dd;
		if (dd < bd) {
			bd = dd;
			best = i;
		}
	}
	return best;
}

// the entry of a codebook of n vectors (stride values each) nearest to x[0..dim-1], every step-th entry
static int nearest_vec(const int16_t *cb, int n, int stride, int step, const int32_t *x, int dim)
{
	int best = 0;
	uint32_t bd = UINT32_MAX;

	for (int i = 0; i < n; i += step) {
		uint32_t dist = 0;

		for (int k = 0; k < dim; k++) {
			int32_t dd = fxp_clamp(x[k] - cb[i * stride + k], -16383, 16383);	// four squares fit 32 bits

			dist += (uint32_t)(dd * dd);
		}
		if (dist < bd) {
			bd = dist;
			best = i;
		}
	}
	return best;
}

void ambe_pack(ambe_state_t *s, int b0, int b1, mbe_params_t *p, uint8_t d[AMBE_DATA_BYTES])
{
	int L, l = 1;
	int32_t pr[MBE_MAXL + 1], T[MBE_MAXL + 1], C[AMBE_BLOCKS][7], R[9], G[9], mean_pr, sum = 0, mean;
	const uint8_t *J;
	ambe_state_t st = *s;	// the prediction reads; the dequantizer below advances the real state

	b0 = (int)fxp_clamp(b0, 0, AMBE_B0_VOICE - 1);
	L = ambe_L[b0];
	J = ambe_J[L - 9];
	memset(d, 0, AMBE_DATA_BYTES);
	ambe_q_put(d, AMBE_Q_B0, b0);
	ambe_q_put(d, AMBE_Q_B1, b1 & 15);

	// what the amplitudes are less the prediction: its mean is the gain's, the rest the residual
	mean_pr = ambe_q_predict(&st, L, pr);
	for (int i = 1; i <= L; i++) {
		T[i] = p->a[i - 1] - pr[i] + mean_pr;
		sum += T[i];
	}
	mean = sum / L;
	ambe_q_put(d, AMBE_Q_B2, nearest_dg(mean + mbe_log2_q11((uint32_t)L) / 2 - s->gamma / 2));
	for (int i = 1; i <= L; i++)
		T[i] -= mean;

	// each block's DCT, its first six coefficients
	for (int i = 0; i < AMBE_BLOCKS; i++) {
		uint32_t unit = (1u << 30) / J[i];
		int kmax = J[i] < 6 ? J[i] : 6;

		for (int k = 1; k <= 6; k++)
			C[i][k] = 0;
		for (int k = 1; k <= kmax; k++) {
			int32_t sm = 0;

			for (int j = 1; j <= J[i]; j++)
				sm += fxp_mulshr(T[l + j - 1], ambe_q_cos(unit, (k - 1) * (2 * j - 1)), 15);
			C[i][k] = sm / J[i];
		}
		l += J[i];
	}
	// the PRBA vector: the blocks' first two coefficients as eight values, and their DCT
	for (int i = 0; i < AMBE_BLOCKS; i++) {
		int32_t c2 = fxp_mulshr(C[i][2], SQRT2, 15);

		R[2 * i + 1] = C[i][1] + c2;
		R[2 * i + 2] = C[i][1] - c2;
	}
	for (int m = 2; m <= 8; m++) {
		int32_t sm = 0;

		for (int i = 1; i <= 8; i++)
			sm += fxp_mulshr(R[i], ambe_q_cos(1u << 27, (m - 1) * (2 * i - 1)), 15);
		G[m] = sm / 8;
	}
	ambe_q_put(d, AMBE_Q_B3, nearest_vec(&ambe_prba24[0][0], 512, 3, 1, &G[2], 3));
	ambe_q_put(d, AMBE_Q_B4, nearest_vec(&ambe_prba58[0][0], 128, 4, 1, &G[5], 4));
	// the higher-order coefficients of each block (b8 has only its even entries)
	for (int i = 0; i < AMBE_BLOCKS; i++) {
		int n = (J[i] < 6 ? J[i] : 6) - 2;

		ambe_q_put(d, AMBE_Q_B5 + i, n > 0 ? nearest_vec(&ambe_hoc[i][0][0], 16, 4, i == 3 ? 2 : 1, &C[i][3], n) : 0);
	}
	// advance the state as the receiver's dequantizer will, and hand back what it reconstructs
	(void)ambe_unpack(s, d, p);
}

// --- speech to frames ---

#define B0_UNVOICED	78	// the pitch code of a frame with no pitch: 30 harmonics, about 124 Hz apart
#define B0_SEARCH	3	// pitch codes tried to each side of the time-domain estimate (each 1.5 % apart)
#define B0_STAY		150	// mismatch (Q15) a code must gain for each step away from that estimate
#define PERIODIC_MIN	(-2700)	// log2 Q11 (0.4): a normalized autocorrelation under this is no pitch ...
#define PERIODIC_START	(-1300)	// ... and (0.64) under this none begins (the full-rate encoder's marks)
#define OCTAVE_GAIN	500	// mismatch (Q15) half the period may be worse by and still win
#define PERIOD4_MIN	80	// the shortest period the codes reach, quarter samples (400 Hz)

// The pitch code nearest a period (quarter samples) or the neighbor of it the spectrum fits best, and that code's
// mismatch. The codes' fundamentals fall as the code rises
static int near_code(const int32_t *buf, int period4, int *b0)
{
	uint32_t w = (0xFFFFFFFFu / (uint32_t)period4) << 2;	// 2^32 / (period4 / 4)
	int lo = 0, hi = AMBE_B0_VOICE - 1, c, best = 0x7FFFFFFF, mis_at = 32768;

	while (lo < hi) {
		int mid = (lo + hi) >> 1;

		if (ambe_w0[mid] > w)
			lo = mid + 1;
		else
			hi = mid;
	}
	c = lo;
	if (c > 0 && ambe_w0[c - 1] - w < w - ambe_w0[c])
		c--;
	*b0 = c;
	for (int t = c - B0_SEARCH; t <= c + B0_SEARCH; t++) {
		int mis, cost;

		if (t < 0 || t >= AMBE_B0_VOICE)
			continue;
		mis = mbe_est_mismatch(buf, ambe_w0[t]);
		cost = mis + B0_STAY * (t > c ? t - c : c - t);
		if (cost < best) {
			best = cost;
			mis_at = mis;
			*b0 = t;
		}
	}
	return mis_at;
}

void ambe_enc_init(const ambe_enc_mem_t *m)
{
	ambe_state_reset(m->s);
	mbe_est_init(m->est);
}

void ambe_enc_frame(const ambe_enc_mem_t *m, const int16_t pcm[MBE_FRAME_LEN], uint8_t air[AMBE_BYTES])
{
	mbe_params_t *p = m->p;
	mbe_est_t e;
	uint8_t band[MBE_MAXL], d[AMBE_DATA_BYTES];
	int b0 = B0_UNVOICED, b1;

	mbe_est_frame(m->est, pcm, m->buf, &e);
	if (e.zero) {
		// nothing but zeros: a silence frame, and nothing is predicted from what was before it
		memcpy(air, ambe_silence, AMBE_BYTES);
		ambe_state_reset(m->s);
		m->est->vprev = 0;
		return;
	}
	if (e.period4 >= PERIOD4_MIN && e.periodic >= (m->est->vprev ? PERIODIC_MIN : PERIODIC_START)) {
		int mis = near_code(m->buf, e.period4, &b0);

		// half the period, when the spectrum fits it as well (the time-domain search favors the double of a
		// short period that falls between two of its samples)
		if (e.period4 >= 2 * PERIOD4_MIN) {
			int bh, mis2 = near_code(m->buf, e.period4 >> 1, &bh);

			if (mis2 <= mis + OCTAVE_GAIN) {
				b0 = bh;
				e.period4 >>= 1;
			}
		}
	} else {
		e.period4 = 0;
	}
	memset(p, 0, sizeof *p);
	p->w0 = ambe_w0[b0];
	p->L = ambe_L[b0];
	for (int l = 1; l <= p->L; l++)
		band[l - 1] = (uint8_t)ambe_q_band(p->w0, l);
	b1 = mbe_est_params_map(m->est, m->buf, &e, p, band, 8, ambe_vuv, 16);
	for (int l = 0; l < p->L; l++)		// PCM units to the model's
		p->a[l] = (int16_t)fxp_clamp(p->a[l] - AMBE_OUT_GAIN, -8 * 2048, 32767);
	ambe_pack(m->s, b0, b1, p, d);
	ambe_fec_encode(d, air);
}
