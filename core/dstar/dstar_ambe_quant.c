// SPDX-License-Identifier: MIT AND ISC
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 cleanq contributors
// SPDX-FileCopyrightText: 2010 mbelib Author (the bitstream it restates, ISC license, see LICENSES/ISC.txt)
/*
 * D-STAR AMBE 3600x2400 parameter quantizer and dequantizer in integer arithmetic (dstar_ambe_quant.h).
 *
 * The bitstream is taken from mbelib's decoder, mbe_decodeAmbe2400Parms in ambe3600x2400.c (szechyjs/mbelib 9a04ed5),
 * and its tables:
 *
 *   Copyright (C) 2010 mbelib Author
 *   Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
 *   granted, provided that the above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
 *   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 *   INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
 *   IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 *   PERFORMANCE OF THIS SOFTWARE.
 *
 * The dequantizer restates that decoder; the quantizer inverts it step by step. The search methods (nearest-neighbor
 * scalar quantization, weighted-MSE vector search with M survivors) are textbook vector quantization (Gersho & Gray,
 * "Vector Quantization and Signal Compression", 1992, ch. 10-12 and 17); the MBE model is Griffin & Lim, IEEE Trans.
 * ASSP 36(8), 1988. Written clean room from those alone (docs/spec/dstar-codec.md, Provenance).
 *
 * Decoder recap (all logs are log2, all indices mbelib's):
 *   b0 -> f0 = 2^(-4.3118 - 0.021336 (b0 + 0.5)), L from a table
 *   b1 -> eight voicing bands, harmonic l in band floor(16 l f0)
 *   b2 -> gamma = Dg[b2] + gamma_prev / 2
 *   b3, b4 -> G2..G4, G5..G8; an 8-point DCT gives R1..R8; each pair (R_2i-1, R_2i) gives the first two DCT
 *             coefficients of block i
 *   b5..b8 -> coefficients 3..6 of blocks 1..4 (b8's low bit is not sent)
 *   Blocks of lengths J1..J4 (sum L) are inverse-DCT'd to T_l, and
 *   log2M_l = T_l - mean(T) + P_l - mean(P) + gamma - log2(L)/2,
 *   with P_l = 0.65 x previous log2M linearly interpolated at l L_prev / L.
 * So the frame's mean log magnitude is set by gamma alone, and T carries only the shape of the prediction residual. The
 * quantizer uses exactly that.
 */
#include "dstar/dstar_ambe_quant.h"

#include <stddef.h>

_Static_assert((-1 >> 1) == -1, "arithmetic right shift required");

#include "dstar_ambe_tables.inc"

#define LOG_MAX		(31 << 12)	// log magnitudes kept in +-31: no overflow below
#define RT_MAX		(12 << 12)	// R-domain targets; the codebooks reach about +-15
// PRBA24 candidates carried into the PRBA58 search. 2 to 16 gave the same MCD on the test speech
#define SURVIVORS	4

static int32_t clamp32(int32_t v, int32_t lim)
{
	return v > lim ? lim : (v < -lim ? -lim : v);
}

// Division rounded to nearest, ties away from zero; d > 0.
static int32_t rdiv(int32_t n, int32_t d)
{
	return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

void dstar_ambe_init(dstar_ambe_state_t *s)
{
	s->gamma_q16 = 0;
	s->L = 30;
	for (unsigned l = 0; l <= DSTAR_AMBE_MAX_L; ++l)
		s->log2_amp[l] = 0;
}

// Previous frame's log magnitude at harmonic idx. mbelib copies log2M[1] to [0] and, when L grows, repeats
// log2M[L_prev] upward; reading past L_prev otherwise only happens with interpolation weight zero. Clamping the index
// reproduces all three cases.
static int32_t prev_at(const dstar_ambe_state_t *s, unsigned idx)
{
	if (idx == 0)
		idx = 1;
	if (idx > s->L)
		idx = s->L;
	return s->log2_amp[idx];
}

// c[l] = 0.65 x interpolated previous log magnitude; returns mean(c). Interpolation uses the exact integer remainder r
// = (L_prev l) mod L, so the weights (L - r)/L and r/L carry no rounding; 0.65 is 13/20.
static int32_t predict(const dstar_ambe_state_t *s, unsigned L, int32_t c[DSTAR_AMBE_MAX_L + 1])
{
	unsigned pL = s->L;
	int32_t sum = 0;
	for (unsigned l = 1; l <= L; ++l) {
		unsigned num = pL * l, a = num / L, r = num - a * L;
		int32_t v = prev_at(s, a) * (int32_t)(L - r) + prev_at(s, a + 1) * (int32_t)r;
		c[l] = rdiv(13 * v, 20 * (int32_t)L);
		sum += c[l];
	}
	return rdiv(sum, (int32_t)L);
}

// R_i (Q12) = sum over m of 2 G_m cos(pi (m-1)(i-1/2)/8), G in Q7. The two halves are separate so the encoder's
// searches pay only for the half they vary.
static void prba24_to_R(const int16_t *g, int32_t R[8])
{
	for (unsigned i = 0; i < 8; ++i) {
		const int16_t *Bi = &tab_B_q14[i * 7];
		R[i] = g[0] * Bi[0] + g[1] * Bi[1] + g[2] * Bi[2];	// Q21
	}
}

static void prba58_to_R(const int8_t *g, int32_t R[8])
{
	for (unsigned i = 0; i < 8; ++i) {
		const int16_t *Bi = &tab_B_q14[i * 7 + 3];
		R[i] = g[0] * Bi[0] + g[1] * Bi[1] + g[2] * Bi[2] + g[3] * Bi[3];
	}
}

static void prba_to_R(const int16_t *g24, const int8_t *g58, int32_t R[8])
{
	int32_t X[8];
	prba24_to_R(g24, R);
	prba58_to_R(g58, X);
	for (unsigned i = 0; i < 8; ++i)
		R[i] = (R[i] + X[i] + 256) >> 9;
}

int dstar_ambe_dequantize(dstar_ambe_state_t *s, dstar_ambe_scratch_t *t, const uint16_t b[DSTAR_AMBE_FIELDS],
			  dstar_ambe_params_t *out)
{
	unsigned b0 = b[0] & 127u;
	if (b0 >= 126) {
		// mbelib synthesizes silence for tone and reserved frames and resets its parameter memory; doing the same keeps
		// the predictors aligned.
		dstar_ambe_init(s);
		out->L = 0;
		out->f0_q24 = 0;
		return DSTAR_AMBE_TONE;
	}
	unsigned L = tab_L[b0];
	uint32_t f0 = tab_f0_q24[b0];
	unsigned vm = tab_vuv[b[1] & 15u];
	out->f0_q24 = f0;
	out->L = (uint8_t)L;
	for (unsigned l = 1; l <= L; ++l)
		out->voiced[l] = (uint8_t)((vm >> ((l * f0) >> 20)) & 1u);

	int32_t gamma = (int32_t)tab_dg_q16[b[2] & 63u] + (s->gamma_q16 >> 1);

	int32_t R[8];
	prba_to_R(&tab_prba24_q7[(b[3] & 511u) * 3], &tab_prba58_q7[(b[4] & 127u) * 4], R);

	const uint8_t *Jv = &tab_J[(L - 9) * 4];
	unsigned hidx[4] = { b[5] & 15u, b[6] & 15u, b[7] & 15u, b[8] & 14u };
	int32_t *T = t->u.deq.T, sumT = 0;
	unsigned pos = 1;
	for (unsigned i = 0; i < 4; ++i) {
		unsigned J = Jv[i], nk = J < 6 ? J : 6;
		int32_t C[6];
		C[0] = (R[2 * i] + R[2 * i + 1] + 1) >> 1;
		C[1] = ((R[2 * i] - R[2 * i + 1]) * 11585 + (1 << 14)) >> 15;	// 1/(2 sqrt 2)
		const int8_t *h = &tab_hoc_q7[(i * 16 + hidx[i]) * 4];
		for (unsigned k = 2; k < nk; ++k)
			C[k] = (int32_t)h[k - 2] * 32;
		const int16_t *cs = &tab_cos_q14[tab_cos_off[J]];
		for (unsigned j = 0; j < J; ++j, cs += nk - 1) {
			int32_t acc = 0;
			for (unsigned k = 1; k < nk; ++k)
				acc += C[k] * cs[k - 1];
			int32_t v = C[0] + ((acc + (1 << 12)) >> 13);	// 2 cos, Q14
			T[pos++] = v;
			sumT += v;
		}
	}

	int32_t *c = t->u.deq.c;
	int32_t mean_c = predict(s, L, c);
	int32_t big = ((gamma + 8) >> 4) - tab_half_log2L_q12[L] - rdiv(sumT, (int32_t)L);
	for (unsigned l = 1; l <= L; ++l)
		out->log2_amp[l] = clamp32(T[l] + c[l] - mean_c + big, LOG_MAX);

	s->gamma_q16 = gamma;
	s->L = (uint8_t)L;
	for (unsigned l = 1; l <= L; ++l)
		s->log2_amp[l] = out->log2_amp[l];
	return DSTAR_AMBE_VOICE;
}

// --- encoder ---

// 2^(x/4096) for x <= 0, Q15.
static uint32_t exp2neg_q15(int32_t x)
{
	uint32_t n = (uint32_t)(-x);
	unsigned ip = n >> 12, f = n & 4095u, i = f >> 8, fr = f & 255u;
	if (ip >= 15)
		return 0;
	uint32_t v = tab_exp2neg_q15[i] - (((tab_exp2neg_q15[i] - tab_exp2neg_q15[i + 1]) * fr) >> 8);
	return v >> ip;
}

// The PRBA search as a quadratic form.
//
// From the decoder recap, an error e in R (8 values) moves the log magnitudes of block i by a sum of squares J_i
// ((e_2i-1 - m)^2 + (e_2i - m)^2) / 2 once the decoder has taken the frame mean off, m = sum_k u_k e_k with u_k = J/2L
// being the part common to every harmonic. With a weight w_i per block that is E(e) = e' Q e, Q = (I - u 1')' W (I - 1
// u'), W = diag(J w / 2). For a candidate c (G2..G8, Q7) and target t, e = t - X c, so
//     E = t'Qt - 2 c'X'Qt + c'X'QXc = t'Qt + score(c),
// and only A = X'QX (7x7) and g = X'Qt (7) depend on the frame. Building them costs a few hundred multiplies once; each
// candidate then costs one small matrix-vector product instead of a pass over the spectrum, which is what makes a full
// search of all 512 + 4 x 128 candidates cheap (the precomputed-correlation trick of CELP codebook searches).
//
// Formats: A in Q11, g in Q8, score in Q15 of squared log2 units. The setup uses 64-bit integers (libgcc's integer
// helpers on CK803S; no floating point). g is clamped to +-1024 so a score never leaves int32 (|c| <= 1.75 per
// coordinate). Q x is carried scaled by (2L)^2 to avoid dividing early.
static int64_t rshift64(int64_t v, unsigned s)
{
	return (v + ((int64_t)1 << (s - 1))) >> s;
}

static void prba_form(const uint8_t *Jv, const uint16_t wj[4], unsigned L, const int32_t Rt[8], const int32_t t1[8],
		      int64_t Qx[7][8], int32_t A[7][7], int32_t g[7], int32_t g1[3])
{
	const int64_t twoL = 2 * (int64_t)L, d = twoL * twoL;
	for (unsigned a = 0; a < 7; ++a) {
		int64_t sx = 0, sz = 0, z[8];
		for (unsigned k = 0; k < 8; ++k)
			sx += (int64_t)Jv[k >> 1] * tab_B_q14[k * 7 + a];
		for (unsigned k = 0; k < 8; ++k) {
			z[k] = (int64_t)wj[k >> 1] * (twoL * tab_B_q14[k * 7 + a] - sx);	// W y, x 2L
			sz += z[k];
		}
		for (unsigned k = 0; k < 8; ++k)
			Qx[a][k] = twoL * z[k] - (int64_t)Jv[k >> 1] * sz;
	}
	// x Q14, Qx Q20 x (2L)^2 once w's Q6 is counted: A = x.Qx is Q34.
	for (unsigned a = 0; a < 7; ++a) {
		for (unsigned b = a; b < 7; ++b) {
			int64_t acc = 0;
			for (unsigned k = 0; k < 8; ++k)
				acc += Qx[a][k] * tab_B_q14[k * 7 + b];
			A[a][b] = A[b][a] = (int32_t)rshift64(acc / d, 23);
		}
	}
	for (unsigned a = 0; a < 7; ++a) {
		int64_t acc = 0, acc1 = 0;
		for (unsigned k = 0; k < 8; ++k) {
			acc += Qx[a][k] * Rt[k];
			if (a < 3)
				acc1 += Qx[a][k] * t1[k];
		}
		g[a] = clamp32((int32_t)rshift64(acc / d, 24), 1 << 18);	// Q32 -> Q8
		if (a < 3)
			g1[a] = clamp32((int32_t)rshift64(acc1 / d, 24), 1 << 18);
	}
}

// score(c) = c'Ac - 2 c'g over n coordinates starting at A[o][o].
static int32_t prba_score(int32_t A[7][7], const int32_t *g, unsigned o, unsigned n, const int32_t *c)
{
	int32_t s = 0;
	for (unsigned a = 0; a < n; ++a) {
		int32_t v = 0;
		for (unsigned b = 0; b < n; ++b)
			v += A[o + a][o + b] * c[b];	// Q18
		s += c[a] * ((v >> 10) - 2 * g[a]);	// Q15
	}
	return s;
}

static uint32_t isqrt32(uint32_t x)
{
	uint32_t r = 0, bit = 1u << 30;
	while (bit > x)
		bit >>= 2;
	while (bit) {
		if (x >= r + bit) {
			x -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

// Per-block weights wj = J x w (w in Q5) for the PRBA search: w = sqrt(700 / (700 + f)), the square root of the mel
// scale's resolution at the block's center frequency. On the four test recordings mean MCD was 25.20 dB with no weight
// (plain log-spectral error), 25.15 dB with the mel resolution itself and 25.04 dB with its square root
static void block_weights(const uint8_t *Jv, uint32_t f0, uint16_t wj[4])
{
	unsigned start = 1;
	for (unsigned i = 0; i < 4; ++i) {
		uint32_t twice_h = 2 * start + Jv[i] - 1;	// 2 x center harmonic
		uint32_t hz = (((twice_h * f0) >> 12) * 4000u) >> 12;
		uint32_t w = (32u * 700u + (700u + hz) / 2) / (700u + hz);
		w = isqrt32(w << 5);
		wj[i] = (uint16_t)(Jv[i] * w);
		start += Jv[i];
	}
}

// Input log magnitude at harmonic idx, held flat past either end.
static int32_t in_at(const dstar_ambe_params_t *in, unsigned Lin, unsigned idx)
{
	if (idx < 1)
		idx = 1;
	if (idx > Lin)
		idx = Lin;
	return clamp32(in->log2_amp[idx], LOG_MAX);
}

// --- the perceptual re-ranking ---
//
// The searches above minimize squared error in log magnitude per harmonic, which counts a 3 dB error on a harmonic
// 50 dB down in a spectral valley the same as one on a formant peak. What a listener (and the mel-cepstral distance)
// hears is closer to the error in the log energy of each critical band, where a band's energy is dominated by its
// loudest harmonics. So the final choice between the best few PRBA pairs, and of the higher-order vectors for each, is
// made under that measure:
//
//   d_b = sum_l s_bl e_l,   s_bl = tri_b(f_l) P_l / (E_b + floor),   E_b = sum_l tri_b(f_l) P_l,
//   error = sum_b v_b (d_b - dbar)^2,   dbar = sum v_b d_b / sum v_b,
//
// with e_l the log2 magnitude error of harmonic l, P_l its target power, tri_b the 24 triangular mel bands from 100 Hz
// to 3.8 kHz, the floor 3e-6 (-55 dB) of the loudest harmonic's power, and v_b = (E_b / P_max)^0.2, which leans
// further towards the loud bands. d_b is the first-order change in band b's log energy, so this is the mel-cepstral
// distance's leading term with c0 (level) left out; dbar goes because the frame mean is the gain's job.
//
// Every decoded quantity is linear in the block DCT coefficients, so the measure is a sum of squares over 24 bands of
// a linear map of them. The frame setup computes each coefficient's band response once (at most 24 columns of 24); a
// candidate then costs one 24-point combination. The higher-order choice within a block uses the Gram matrix of its
// columns: h'Gh per codeword once a frame, and per pair only the linear term.
//
// Search: the PAIRS best PRBA pairs of the quadratic search are scored with the plain search's higher-order vectors,
// and the best REFINE of those get their higher-order vectors re-chosen, block by block.
//
// The research measured it on 12 inputs (4 flite voices, 8 real recordings), mean MCD / STOI: the plain search 22.51
// dB / 0.855, this 21.70 dB / 0.865, unquantized 20.70 dB / 0.891. Re-ranking under plain log-magnitude error gained
// nothing (22.49 dB), so the plain search was already near its own optimum: the gain is the measure. Of the choices:
//   - the floor and v_b trade MCD against STOI: floor 1e-5 with v_b's exponent 0 / 0.1 / 0.2 gave 21.71 / 21.76 /
//     21.86 dB against STOI 0.861 / 0.863 / 0.864; floor 3e-6 with 0.2 kept most of both
//   - all 8 or 16 pairs refined scored within 0.05 dB of 16 scored and 3 refined, for twice the cycles; a second
//     higher-order pass gave nothing, nor did more PRBA24 survivors (8)
//   - a double-precision prototype that decoded every candidate exactly and evaluated band energies without
//     linearizing scored within 0.05 dB
// Cost: about 110 k host instructions a frame (docs/spec/dstar-codec.md 8).
#define NB		DSTAR_AMBE_BANDS
#define PAIRS		16	// PRBA pairs scored by the perceptual measure
#define REFINE		3	// pairs whose higher-order vectors are re-chosen
#define FLOOR_Q27	403	// 3e-6 (-55 dB) of the loudest harmonic's power
#define BAND_EXP_Q12	410	// half of v_b's exponent 0.2, Q12

// Mel band edges (HTK mel, 2595 log10(1 + f / 700); 26 points evenly spaced from 100 Hz to 3.8 kHz) in Q24 cycles per
// sample at 8 kHz: 700 (10^(m / 2595) - 1) / 8000 x 2^24
static const uint32_t tab_mel_q24[NB + 2] = {
	209715, 329725, 458319, 596112, 743761, 901972, 1071500, 1253154, 1447803,
	1656375, 1879866, 2119344, 2375952, 2650916, 2945548, 3261256, 3599547,
	3962036, 4350454, 4766657, 5212631, 5690507, 6202565, 6751252, 7339187, 7969178,
};

// 2^(x/4096) for x <= 0, Q20; 0 below 2^-20.
static uint32_t exp2neg_q20(int32_t x)
{
	uint32_t n = (uint32_t)(-x);
	unsigned ip = n >> 12, f = n & 4095u, i = f >> 8, fr = f & 255u;
	if (ip > 20)
		return 0;
	uint32_t v = tab_exp2neg_q15[i] - (((tab_exp2neg_q15[i] - tab_exp2neg_q15[i + 1]) * fr) >> 8);
	return (v << 5) >> ip;
}

// log2(x), Q12, x >= 1: the integer part exactly, the fraction linear (error under 0.09; it only scales the band
// weights and nudges the gain).
static int32_t ilog2_q12(uint32_t x)
{
	int n = 31;
	while (!(x >> n))
		--n;
	uint32_t fr = n >= 12 ? (x >> (n - 12)) & 4095u : (x << (12 - n)) & 4095u;
	return (int32_t)(((uint32_t)n << 12) + fr);
}

// The frame setup. a: the target log2 magnitudes on the quantized grid (Q12), amax their maximum, c: the prediction (the
// target residual is a - c). w: working values, dead after the call
static void perc_setup(dstar_ambe_perc_t *P, const int32_t *a, int32_t amax, const int32_t *c, unsigned L, uint32_t f0,
		       const uint8_t *Jv, uint32_t *E, uint32_t *fl, int32_t *lm, int32_t *sv, int32_t *v10, int32_t *tr,
		       int32_t *ssum, uint8_t *seg, uint16_t *wu)
{
	for (unsigned b = 0; b < NB; ++b) {
		E[b] = 0;
		lm[b] = -LOG_MAX;
	}
	P->pa_sum = 0;

	// Each harmonic lies between two mel edges, seg and seg + 1: it is on the rising side of band seg (weight wu, Q8)
	// and the falling side of band seg - 1 (weight 256 - wu). seg = 255 marks a harmonic outside 100 Hz to 3.8 kHz,
	// which no band sees. lm[b] is the loudest harmonic band b sees
	unsigned sg = 0;
	for (unsigned l = 1; l <= L; ++l) {
		uint32_t f = l * f0;
		P->pa_sum += exp2neg_q20(2 * (a[l] - amax));
		while (sg < NB + 1 && f >= tab_mel_q24[sg + 1])
			++sg;
		if (f < tab_mel_q24[0] || sg >= NB + 1) {
			seg[l] = 255;
			continue;
		}
		seg[l] = (uint8_t)sg;
		wu[l] = (uint16_t)(((f - tab_mel_q24[sg]) >> 4) * 256u / ((tab_mel_q24[sg + 1] - tab_mel_q24[sg]) >> 4));
		if (sg < NB && a[l] > lm[sg])
			lm[sg] = a[l];
		if (sg > 0 && a[l] > lm[sg - 1])
			lm[sg - 1] = a[l];
	}

	// Band energies relative to each band's own loudest harmonic (Q27), so that a band 60 dB below the frame's peak
	// still has a precise shape: Q20 relative to the frame peak, tried first, rounded such bands to nothing and lost a
	// third of the gain. The floor, fixed relative to the frame peak, is rescaled into each band's units
	for (unsigned l = 1; l <= L; ++l) {
		unsigned g = seg[l];
		if (g == 255)
			continue;
		if (g < NB)
			E[g] += (exp2neg_q20(2 * (a[l] - lm[g])) * wu[l]) >> 1;
		if (g > 0)
			E[g - 1] += (exp2neg_q20(2 * (a[l] - lm[g - 1])) * (256u - wu[l])) >> 1;
	}
	int32_t V = 0;
	for (unsigned b = 0; b < NB; ++b) {
		if (E[b] == 0) {
			sv[b] = 0;
			v10[b] = 0;
			fl[b] = 0;
			continue;
		}
		int32_t up = 2 * (amax - lm[b]);			// >= 0, Q12
		int32_t lf = ilog2_q12(FLOOR_Q27) + up;
		if (lf >= (30 << 12)) {
			fl[b] = 1u << 30;
		} else {
			uint32_t m = exp2neg_q15((lf & 4095) - 4096) << 1;	// 2^frac, Q15
			int ip = lf >> 12;
			fl[b] = ip >= 15 ? m << (ip - 15) : m >> (15 - ip);
		}
		// Band weights: sv = sqrt(v_b), Q15, and v in Q10 for the weighted mean
		int32_t lg = ilog2_q12(E[b]) - (27 << 12) - up;
		if (lg > 0)
			lg = 0;
		sv[b] = (int32_t)exp2neg_q15((lg * BAND_EXP_Q12) >> 12);
		v10[b] = (sv[b] * sv[b]) >> 20;
		V += v10[b];
	}
	if (V == 0)
		V = 1;

	// Raw band responses: zc (Q13) per block coefficient and the target (Q26 accumulated, then Q12). k = 0 is the block
	// mean, k >= 1 the decoder's 2 cos terms
	int32_t mean_r = 0;
	for (unsigned b = 0; b < NB; ++b)
		tr[b] = ssum[b] = 0;
	for (unsigned l = 1; l <= L; ++l)
		mean_r += clamp32(a[l] - c[l], (16 << 12) - 1);
	mean_r = rdiv(mean_r, (int32_t)L);
	// Accumulated straight into zc: a band's shares sum to at most one and |2 cos| <= 2, so every partial sum stays
	// within +-2^14 (Q13)
	for (unsigned i = 0; i < 4; ++i)
		for (unsigned k = 0; k < 6; ++k)
			for (unsigned b = 0; b < NB; ++b)
				P->zc[i][k][b] = 0;
	unsigned pos = 1;
	for (unsigned i = 0; i < 4; ++i) {
		unsigned J = Jv[i], nk = J < 6 ? J : 6;
		const int16_t *cs = &tab_cos_q14[tab_cos_off[J]];
		for (unsigned j = 0; j < J; ++j, cs += nk - 1, ++pos) {
			unsigned sgl = seg[pos];
			if (sgl == 255)
				continue;
			for (int side = 0; side < 2; ++side) {
				unsigned b = side ? sgl : sgl - 1u;
				if ((side && sgl >= NB) || (!side && sgl == 0))
					continue;
				uint32_t wp = (exp2neg_q20(2 * (a[pos] - lm[b])) * (side ? wu[pos] : 256u - wu[pos])) >> 1;
				uint32_t den = E[b] + fl[b];
				int sh = 0;
				while ((den >> sh) >= (1u << 16))
					++sh;
				int32_t sq = (int32_t)(((wp >> sh) << 14) / ((den >> sh) ? (den >> sh) : 1));	// share, Q14
				tr[b] += sq * clamp32(a[pos] - c[pos] - mean_r, (16 << 12) - 1);
				ssum[b] += sq;
				P->zc[i][0][b] = (int16_t)(P->zc[i][0][b] + (sq >> 1));
				for (unsigned k = 1; k < nk; ++k)
					P->zc[i][k][b] = (int16_t)(P->zc[i][k][b] + ((sq * cs[k - 1]) >> 14));
			}
		}
	}

	// The decoder takes the mean over harmonics off T, so a block mean C0_i moves every harmonic by -J_i/L as well as
	// its own block; the cosine terms sum to zero over a block and need nothing. With the floor, a weak band's shares
	// sum to less than one, so this does not cancel and must be modeled (ssum[b] is band b's share total, Q14). The
	// target's own mean comes off the residual above for the same reason. Leaving both out made the fixed-point measure
	// disagree with an exact evaluation by a factor of 1.7 on average, and cost 0.3 dB of MCD
	for (unsigned i = 0; i < 4; ++i)
		for (unsigned b = 0; b < NB; ++b)
			P->zc[i][0][b] = (int16_t)(P->zc[i][0][b] - ((ssum[b] * Jv[i] / (int32_t)L) >> 1));

	// The weighting: subtract the v-weighted mean over bands, scale by sqrt v
	for (unsigned i = 0; i < 4; ++i) {
		for (unsigned k = 0; k < 6; ++k) {
			int32_t m = 0;
			for (unsigned b = 0; b < NB; ++b)
				m += v10[b] * P->zc[i][k][b];
			m /= V;
			for (unsigned b = 0; b < NB; ++b)
				P->zc[i][k][b] = (int16_t)clamp32((sv[b] * (P->zc[i][k][b] - m)) >> 15, 32767);
		}
	}
	{
		int32_t m = 0;
		for (unsigned b = 0; b < NB; ++b) {
			tr[b] >>= 14;
			m += v10[b] * (tr[b] >> 6);
		}
		m = (m / V) * 64;
		for (unsigned b = 0; b < NB; ++b)
			P->yr[b] = (sv[b] * ((tr[b] - m) >> 2)) >> 13;
	}

	// h'Gh for every higher-order codeword, G the Gram matrix of the block's columns, Q8
	for (unsigned i = 0; i < 4; ++i) {
		unsigned J = Jv[i], nk = J < 6 ? J : 6, n = nk > 2 ? nk - 2 : 0;
		int32_t G[4][4];
		for (unsigned x = 0; x < n; ++x) {
			for (unsigned y = x; y < n; ++y) {
				int32_t g = 0;
				for (unsigned b = 0; b < NB; ++b)
					g += (P->zc[i][x + 2][b] * P->zc[i][y + 2][b]) >> 6;
				G[x][y] = G[y][x] = clamp32(g >> 12, 1 << 15);
			}
		}
		for (unsigned cw = 0; cw < 16; ++cw) {
			const int8_t *h = &tab_hoc_q7[(i * 16 + cw) * 4];
			int32_t q = 0;
			for (unsigned x = 0; x < n; ++x) {
				int32_t v = 0;
				for (unsigned y = 0; y < n; ++y)
					v += G[x][y] * h[y];
				q += h[x] * v;
			}
			P->hq[i][cw] = q;
		}
	}
}

// Adds (sign +1) or removes (-1) higher-order codeword cw of block i from the band residual y (Q12): h (Q7) x zc (Q13)
// >> 8. The sum is shifted before the sign is applied, so a removal cancels its addition exactly. y is kept within +-16
// log2 units, which bounds every product that reads it
static void perc_hoc(const dstar_ambe_perc_t *P, unsigned i, unsigned n, unsigned cw, int sign, int32_t *y)
{
	const int8_t *h = &tab_hoc_q7[(i * 16 + cw) * 4];
	int32_t v[NB];
	for (unsigned b = 0; b < NB; ++b)
		v[b] = 0;
	for (unsigned k = 0; k < n; ++k) {
		const int16_t *z = P->zc[i][k + 2];
		int32_t hk = h[k];
		for (unsigned b = 0; b < NB; ++b)
			v[b] += hk * z[b];
	}
	if (sign > 0) {
		for (unsigned b = 0; b < NB; ++b)
			y[b] += v[b] >> 8;
	} else {
		for (unsigned b = 0; b < NB; ++b)
			y[b] -= v[b] >> 8;
	}
}

// The measure's error for band residual y
static uint32_t perc_norm(const int32_t *y)
{
	uint32_t e = 0;
	for (unsigned b = 0; b < NB; ++b) {
		int32_t v = clamp32(y[b] >> 5, 8191);
		e += (uint32_t)(v * v);
	}
	return e;
}

// The band residual of PRBA pair (b3, b4), into y: the pair's block means and slopes added to base (minus the target,
// plus the starting higher-order vectors). Returns its error
static uint32_t perc_pair(const dstar_ambe_perc_t *P, unsigned b3, unsigned b4, const int32_t *base, int32_t *y)
{
	int32_t R[8], c0[4], c1[4];
	prba_to_R(&tab_prba24_q7[b3 * 3], &tab_prba58_q7[b4 * 4], R);
	for (unsigned i = 0; i < 4; ++i) {
		c0[i] = (R[2 * i] + R[2 * i + 1] + 1) >> 1;
		c1[i] = ((R[2 * i] - R[2 * i + 1]) * 11585 + (1 << 14)) >> 15;
	}
	for (unsigned b = 0; b < NB; ++b) {
		int32_t v = base[b];
		for (unsigned i = 0; i < 4; ++i)
			v += ((c0[i] * P->zc[i][0][b]) >> 13) + ((c1[i] * P->zc[i][1][b]) >> 13);
		y[b] = v;
	}
	return perc_norm(y);
}

// Re-chooses each block's higher-order vector against residual y (which includes the current choice hb), one block at
// a time; updates y and hb, returns the new error
static uint32_t perc_refine(const dstar_ambe_perc_t *P, const uint8_t *Jv, int32_t *y, unsigned hb[4])
{
	for (unsigned i = 0; i < 4; ++i) {
		unsigned J = Jv[i], nk = J < 6 ? J : 6, n = nk > 2 ? nk - 2 : 0, step = (i == 3) ? 2 : 1;
		if (n == 0)
			continue;
		perc_hoc(P, i, n, hb[i], -1, y);
		int32_t q[4], yc[NB];
		for (unsigned b = 0; b < NB; ++b)
			yc[b] = clamp32(y[b], 1 << 16);
		for (unsigned k = 0; k < n; ++k) {
			const int16_t *z = P->zc[i][k + 2];
			int32_t v = 0;
			for (unsigned b = 0; b < NB; ++b)
				v += (yc[b] * z[b]) >> 10;
			q[k] = clamp32(v, 1 << 21);			// Q15
		}
		int32_t bs = INT32_MAX;
		unsigned bc = hb[i];
		for (unsigned cw = 0; cw < 16; cw += step) {
			const int8_t *h = &tab_hoc_q7[(i * 16 + cw) * 4];
			int32_t sc = P->hq[i][cw];
			for (unsigned k = 0; k < n; ++k)
				sc += 2 * h[k] * q[k];
			if (sc < bs) {
				bs = sc;
				bc = cw;
			}
		}
		hb[i] = bc;
		perc_hoc(P, i, n, bc, +1, y);
	}
	return perc_norm(y);
}

// The gain, chosen after the shape. The decoder fixes the frame's mean log magnitude from the gain alone, so the shape
// choice can't move it; but a coarse shape flattens peaks, and with the mean held the frame loses power (0.3 to 1.4 dB
// on the test speech). Correcting half of that loss in log terms gave the best STOI (0.8606 -> 0.8632; the full
// correction gave 0.8592, overshooting on frames whose shape was already right). The prediction memory then sees the
// corrected gain, as the decoder's does. Returns b2. pa_sum is the target's power relative to its loudest harmonic,
// Q20. The trial decode runs on a copy of the prediction memory, in t
static uint16_t gain_match(const dstar_ambe_state_t *s, dstar_ambe_scratch_t *t, const uint16_t b[DSTAR_AMBE_FIELDS],
			   unsigned L, int32_t amax, uint32_t pa_sum)
{
	dstar_ambe_params_t *o = &t->dec;
	t->trial = *s;
	dstar_ambe_dequantize(&t->trial, t, b, o);
	int32_t omax = -LOG_MAX;
	for (unsigned l = 1; l <= L; ++l) {
		if (o->log2_amp[l] > omax)
			omax = o->log2_amp[l];
	}
	uint32_t po = 0;
	for (unsigned l = 1; l <= L; ++l)
		po += exp2neg_q20(2 * (o->log2_amp[l] - omax));
	if (!po)
		return b[2];
	// log2(pa / po) in Q12; x 0.5 (power to magnitude) x 0.5 (half the loss) x 16 (Q16)
	int32_t lr = ilog2_q12(pa_sum) - ilog2_q12(po) + 2 * (amax - omax);
	int32_t want = (int32_t)tab_dg_q16[b[2]] + lr * 4, bd = INT32_MAX;
	uint16_t best = b[2];
	for (unsigned k = 0; k < 64; ++k) {
		int32_t d = (int32_t)tab_dg_q16[k] - want;
		if (d < 0)
			d = -d;
		if (d < bd) {
			bd = d;
			best = (uint16_t)k;
		}
	}
	return best;
}

void dstar_ambe_quantize(dstar_ambe_state_t *s, dstar_ambe_scratch_t *t, const dstar_ambe_params_t *in,
			 uint16_t b[DSTAR_AMBE_FIELDS], dstar_ambe_params_t *dec)
{
	unsigned Lin = in->L < 1 ? 1 : (in->L > DSTAR_AMBE_MAX_L ? DSTAR_AMBE_MAX_L : in->L);
	uint32_t f0in = in->f0_q24;
	if (f0in < tab_f0_q24[125] / 2)
		f0in = tab_f0_q24[125] / 2;
	if (f0in > tab_f0_q24[0] * 2)
		f0in = tab_f0_q24[0] * 2;

	// Pitch: nearest in log frequency. f0 falls as b0 rises, so b0 is the number of geometric midpoints above f0in.
	unsigned lo = 0, hi = 125;
	while (lo < hi) {
		unsigned m = (lo + hi) / 2;
		if (tab_f0_mid_q24[m] > f0in)
			lo = m + 1;
		else
			hi = m;
	}
	unsigned b0 = lo, L = tab_L[b0];
	uint32_t f0 = tab_f0_q24[b0];

	// Resample the magnitudes and voicing onto the quantized harmonic grid: harmonic l now sits at input harmonic l f0
	// / f0in. Linear in log.
	int32_t *a = t->u.q.a;
	uint8_t *v = t->u.q.v;
	int32_t amax = -LOG_MAX, sumA = 0;
	for (unsigned l = 1; l <= L; ++l) {
		uint32_t num = l * f0, idx = num / f0in, rem = num - idx * f0in;
		int32_t fr = (int32_t)((rem << 10) / f0in);
		int32_t x0 = in_at(in, Lin, idx), x1 = in_at(in, Lin, idx + 1);
		a[l] = x0 + (((x1 - x0) * fr + 512) >> 10);
		unsigned near = idx + (fr >= 512);
		if (near < 1)
			near = 1;
		if (near > Lin)
			near = Lin;
		v[l] = in->voiced[near] ? 1 : 0;
		if (a[l] > amax)
			amax = a[l];
		sumA += a[l];
	}

	// Voicing: each codeword fixes four band pairs. Cost of a codeword is the amplitude it labels wrongly, so loud
	// harmonics decide.
	uint32_t wv[8] = {0}, wu[8] = {0};
	for (unsigned l = 1; l <= L; ++l) {
		unsigned band = (l * f0) >> 20;
		uint32_t w = exp2neg_q15(a[l] - amax);
		if (v[l])
			wv[band] += w;
		else
			wu[band] += w;
	}
	unsigned b1 = 0;
	uint32_t best1 = UINT32_MAX;
	for (unsigned k = 0; k < 16; ++k) {
		uint32_t cost = 0;
		for (unsigned j = 0; j < 8; ++j)
			cost += ((tab_vuv[k] >> j) & 1u) ? wu[j] : wv[j];
		if (cost < best1) {
			best1 = cost;
			b1 = k;
		}
	}

	// Gain: the decoded frame mean is gamma - log2(L)/2 exactly. A multiply, not a shift: the mean is negative for a
	// quiet frame, and a left shift of a negative number is undefined in C (the sanitizer stopped there)
	int32_t target = (rdiv(sumA, (int32_t)L) + tab_half_log2L_q12[L]) * 16;
	int32_t delta = target - (s->gamma_q16 >> 1);
	unsigned b2 = 0;
	int32_t best2 = INT32_MAX;
	for (unsigned k = 0; k < 64; ++k) {
		int32_t d = (int32_t)tab_dg_q16[k] - delta;
		if (d < 0)
			d = -d;
		if (d < best2) {
			best2 = d;
			b2 = k;
		}
	}

	// Prediction residual and its block DCT (forward transform of the decoder's inverse: C_k = (1/J) sum_j r_j cos(pi
	// (k-1)(j-1/2)/J)).
	int32_t *c = t->u.q.c;
	predict(s, L, c);
	const uint8_t *Jv = &tab_J[(L - 9) * 4];
	uint16_t wj[4];
	block_weights(Jv, f0, wj);
	int32_t Ct[4][6], Rt[8];
	unsigned pos = 1;
	for (unsigned i = 0; i < 4; ++i) {
		unsigned J = Jv[i], nk = J < 6 ? J : 6;
		int32_t acc[6] = {0};
		const int16_t *cs = &tab_cos_q14[tab_cos_off[J]];
		for (unsigned j = 0; j < J; ++j, cs += nk - 1) {
			int32_t r = clamp32(a[pos] - c[pos], (16 << 12) - 1);
			++pos;
			acc[0] += r;
			for (unsigned k = 1; k < nk; ++k)
				acc[k] += (r * cs[k - 1] + 8192) >> 14;
		}
		for (unsigned k = 0; k < nk; ++k)
			Ct[i][k] = rdiv(acc[k], (int32_t)J);
		int32_t d = (Ct[i][1] * 23170 + 8192) >> 14;	// sqrt 2
		Rt[2 * i] = clamp32(Ct[i][0] + d, RT_MAX);
		Rt[2 * i + 1] = clamp32(Ct[i][0] - d, RT_MAX);
	}

	// The decoder removes the frame mean, so a common offset in the target costs nothing. Taking the J-weighted mean
	// out keeps the target, and so g below, small enough to stay inside its clamp.
	{
		int32_t sum = 0;
		for (unsigned i = 0; i < 4; ++i)
			sum += (int32_t)Jv[i] * (Rt[2 * i] + Rt[2 * i + 1]);
		int32_t mu = rdiv(sum, (int32_t)(2 * L));
		for (unsigned i = 0; i < 8; ++i)
			Rt[i] -= mu;
	}

	// PRBA, stage 1: search G2..G4 against the target with its G5..G8 part taken out by plain projection (the 8-point
	// basis is orthogonal), so each PRBA24 candidate is judged as if PRBA58 were perfect. Keep the best SURVIVORS by
	// true spectral error.
	int32_t t1[8];
	{
		int32_t G[4];
		for (unsigned m = 0; m < 4; ++m) {
			int32_t acc = 0;
			for (unsigned i = 0; i < 8; ++i)
				acc += (Rt[i] * tab_B_q14[i * 7 + 3 + m] + (1 << 17)) >> 18;
			G[m] = acc;	// Q12
		}
		for (unsigned i = 0; i < 8; ++i) {
			const int16_t *Bi = &tab_B_q14[i * 7 + 3];
			int32_t p = (G[0] * Bi[0] + G[1] * Bi[1] + G[2] * Bi[2] + G[3] * Bi[3] + (1 << 13)) >> 14;
			t1[i] = Rt[i] - p;
		}
	}
	int32_t (*A)[7] = t->u.q.w.prba.A, g[7], g1[3];
	prba_form(Jv, wj, L, Rt, t1, t->u.q.w.prba.Qx, A, g, g1);
	unsigned sv_idx[SURVIVORS];
	int32_t sv_err[SURVIVORS];
	for (unsigned k = 0; k < SURVIVORS; ++k) {
		sv_idx[k] = 0;
		sv_err[k] = INT32_MAX;
	}
	for (unsigned k = 0; k < 512; ++k) {
		const int16_t *q = &tab_prba24_q7[k * 3];
		int32_t cand[3] = { q[0], q[1], q[2] };
		int32_t err = prba_score(A, g1, 0, 3, cand);
		if (err >= sv_err[SURVIVORS - 1])
			continue;
		unsigned p = SURVIVORS - 1;
		while (p > 0 && sv_err[p - 1] > err) {
			sv_err[p] = sv_err[p - 1];
			sv_idx[p] = sv_idx[p - 1];
			--p;
		}
		sv_err[p] = err;
		sv_idx[p] = k;
	}

	// Stage 2: for each survivor, the exact joint error over PRBA58: the survivor's own score plus the PRBA58 score
	// against what it leaves, g58 - C'c24 with C the PRBA24 x PRBA58 block of A. The best PAIRS pairs, in order, go on
	// to the perceptual measure below
	unsigned b3 = sv_idx[0], b4 = 0;
	int32_t best = INT32_MAX;
	uint16_t pk3[PAIRS], pk4[PAIRS];
	int32_t pke[PAIRS];
	for (unsigned i = 0; i < PAIRS; ++i) {
		pke[i] = INT32_MAX;
		pk3[i] = pk4[i] = 0;
	}
	for (unsigned s2 = 0; s2 < SURVIVORS; ++s2) {
		const int16_t *q = &tab_prba24_q7[sv_idx[s2] * 3];
		int32_t c24[3] = { q[0], q[1], q[2] }, h[4];
		int32_t base = prba_score(A, g, 0, 3, c24);
		for (unsigned m = 0; m < 4; ++m)
			h[m] = g[3 + m] - ((A[0][3 + m] * c24[0] + A[1][3 + m] * c24[1] + A[2][3 + m] * c24[2]) >> 10);
		for (unsigned k = 0; k < 128; ++k) {
			const int8_t *q58 = &tab_prba58_q7[k * 4];
			int32_t cand[4] = { q58[0], q58[1], q58[2], q58[3] };
			int32_t err = base + prba_score(A, h, 3, 4, cand);
			if (err < best) {
				best = err;
				b3 = sv_idx[s2];
				b4 = k;
			}
			if (err < pke[PAIRS - 1]) {
				unsigned p = PAIRS - 1;
				while (p > 0 && pke[p - 1] > err) {
					pke[p] = pke[p - 1];
					pk3[p] = pk3[p - 1];
					pk4[p] = pk4[p - 1];
					--p;
				}
				pke[p] = err;
				pk3[p] = (uint16_t)sv_idx[s2];
				pk4[p] = (uint16_t)k;
			}
		}
	}

	// HOC: coefficients 3..min(J,6) of each block, plain squared error: within one block every coefficient carries the
	// same weight. This is the starting point; the perceptual pass below re-chooses them for its best pairs. (Weighting
	// harmonics by frequency and loudness here, per harmonic, did not improve MCD; the band-energy measure below does.)
	// The last block's codebook is restricted to even indices: b8's low bit is not transmitted.
	unsigned hb[4];
	for (unsigned i = 0; i < 4; ++i) {
		unsigned J = Jv[i], nk = J < 6 ? J : 6, n = nk - 2, step = (i == 3) ? 2 : 1;
		hb[i] = 0;
		if (n == 0)
			continue;
		int32_t bh = INT32_MAX;
		for (unsigned k = 0; k < 16; k += step) {
			const int8_t *h = &tab_hoc_q7[(i * 16 + k) * 4];
			int32_t err = 0;
			for (unsigned q = 0; q < n; ++q) {
				int32_t d = clamp32(Ct[i][q + 2] - (int32_t)h[q] * 32, 16383);
				err += d * d;
			}
			if (err < bh) {
				bh = err;
				hb[i] = k;
			}
		}
	}

	// The final choice under the perceptual measure (perc_setup). Every pair starts from the plain search's
	// higher-order vectors, so their part of the residual is common; each pair is scored with them, and only the best
	// REFINE get theirs re-chosen. The quadratic form is dead by now: the setup's working values take its place
	dstar_ambe_perc_t *P = &t->u.q.P;
	perc_setup(P, a, amax, c, L, f0, Jv, t->u.q.w.perc.E, t->u.q.w.perc.fl, t->u.q.w.perc.lm, t->u.q.w.perc.sv,
		   t->u.q.w.perc.v10, t->u.q.w.perc.tr, t->u.q.w.perc.ssum, t->u.q.w.perc.seg, t->u.q.w.perc.wu);
	{
		int32_t base[NB], y[NB];
		for (unsigned bnd = 0; bnd < NB; ++bnd)
			base[bnd] = -P->yr[bnd];
		for (unsigned i = 0; i < 4; ++i) {
			unsigned nk = Jv[i] < 6 ? Jv[i] : 6;
			if (nk > 2)
				perc_hoc(P, i, nk - 2, hb[i], +1, base);
		}
		uint32_t pe[PAIRS];
		unsigned np = 0;
		for (; np < PAIRS && pke[np] != INT32_MAX; ++np)
			pe[np] = perc_pair(P, pk3[np], pk4[np], base, y);
		uint32_t be = UINT32_MAX;
		unsigned h0[4] = { hb[0], hb[1], hb[2], hb[3] };
		for (unsigned r = 0; r < REFINE && r < np; ++r) {
			unsigned k = 0;
			for (unsigned j = 1; j < np; ++j) {
				if (pe[j] < pe[k])
					k = j;
			}
			pe[k] = UINT32_MAX;
			unsigned h[4] = { h0[0], h0[1], h0[2], h0[3] };
			perc_pair(P, pk3[k], pk4[k], base, y);
			uint32_t e = perc_refine(P, Jv, y, h);
			if (e < be) {
				be = e;
				b3 = pk3[k];
				b4 = pk4[k];
				for (unsigned i = 0; i < 4; ++i)
					hb[i] = h[i];
			}
		}
	}
	uint32_t pa_sum = P->pa_sum;

	b[0] = (uint16_t)b0;
	b[1] = (uint16_t)b1;
	b[2] = (uint16_t)b2;
	b[3] = (uint16_t)b3;
	b[4] = (uint16_t)b4;
	for (unsigned i = 0; i < 4; ++i)
		b[5 + i] = (uint16_t)hb[i];
	if (pa_sum)
		b[2] = gain_match(s, t, b, L, amax, pa_sum);

	// The far end's decode, which also advances the prediction memory exactly as it will there. Everything above in
	// *t is dead by now, so the decode's own scratch may overlap it
	dstar_ambe_dequantize(s, t, b, dec ? dec : &t->dec);
}
