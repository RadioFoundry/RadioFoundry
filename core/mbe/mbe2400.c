// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// 2400 bit/s MBE front end (mbe2400.h, docs/spec/mbe-2400.md), in integers.
//
// The dequantizer (spec 2): the pitch code gives the fundamental and L; the voicing code four 1 kHz bands; the gain
// gamma = Dg[b2] + gamma' / 2; two codebook vectors (G2..G4, G5..G8) give, through an 8-point DCT, the mean and the
// first cosine term of four blocks of harmonics, and four more codebooks their next four terms; each block's inverse
// DCT gives the shape T, and log2 M_l = T_l - mean(T) + P_l + gamma - log2(L) / 2, P the last frame's log amplitudes
// times 0.65, resampled to this frame's harmonics, mean removed (mbe_predict).
//
// The quantizer inverts it step by step, each codebook by nearest neighbor, and runs its own frame through the
// dequantizer so that its prediction state is exactly the receiver's. Log amplitudes are log2 Q16 inside a frame and
// Q11 outside, as in the full-rate front end.
#include <string.h>
#include "mbe2400.h"
#include "mbe2400_q.h"
#include "mbe_dsp.h"
#include "mbe_est.h"
#include "../dsp/fxp.h"

#define RHO		21299		// 0.65, Q15
#define OUT_GAIN	4252		// log2 Q11: the model amplitude to PCM units, 7 less 4.4 dB
#define SQRT2_Q15	46341
#define ISQRT8_Q15	11585		// 1 / (2 sqrt 2)
#define B0_UNVOICED	90		// the pitch code of a frame with no pitch: 108 Hz, 36 harmonics
#define A_FLOOR		(16 * 2048)	// log2 Q11: the quantizer holds every harmonic within 96 dB of the frame's loudest

static int32_t cos_turn(uint32_t unit, int num) { return mbe_cos_i(unit * (uint32_t)num); }

uint32_t mbe2400_w0(int b0)
{
	// f0 = 2^(-4.311767578125 - 0.021336 (b0 - 1)) of the sample rate: the exponent of 2^32 f0, Q20, to Q11
	return (uint32_t)mbe_exp2_q11((29033216 - 22372 * (b0 - 1) + 256) >> 9);
}

int mbe2400_pitch_harmonics(int b0) { return mbe2400_L[b0]; }

// the code nearest a fundamental's log2 (Q11, of mbe_params_t.w0), any value
static int code_of_log(int32_t lw)
{
	int32_t n = 29033216 - lw * 512;

	// code k at n = 22372 (k - 1): code 0 only past half a step above code 1
	return n < -11186 ? 0 : (n + 11186) / 22372 + 1;
}

// the code nearest a period in quarter samples: w0 = 2^34 / period4
static int code_of(int period4) { return code_of_log(34 * 2048 - mbe_log2_q11((uint32_t)period4)); }
static const mbe_grid_t grid = { code_of, mbe2400_w0, MBE2400_PITCHES, 78 };

void mbe2400_state_reset(mbe2400_state_t *s)
{
	memset(s, 0, sizeof *s);
	s->q.L = 30;
}

// block i's length, for L harmonics
static int block_len(int L, int i)
{
	const uint8_t *J = &mbe2400_J[(L - 9) * 3];

	return i < 3 ? J[i] : L - J[0] - J[1] - J[2];
}

// cos(pi (m - 1)(i - 1/2) / 8), Q15: the 8-point transform between G2..G8 and R1..R8
static int32_t cos8(int m, int i) { return cos_turn(1u << 27, (m - 1) * (2 * i - 1)); }

int mbe2400_unpack(mbe2400_state_t *s, const uint16_t b[MBE2400_FIELDS], mbe_params_t *p)
{
	int b0 = b[0] & 127, L, pos = 1;
	int32_t G[9], R[9], T[MBE_MAXL + 1], pr[MBE_MAXL + 1], sumT = 0, gamma, big;
	const int8_t *g24 = &mbe2400_prba24[(b[3] & 511) * 3], *g58 = &mbe2400_prba58[(b[4] & 127) * 4];

	p->flags = MBE_UV_DENSITY | MBE_PHASE_REGEN;
	if (b0 >= MBE2400_PITCHES) {
		// another kind of frame: played as silence, and nothing predicted from what was before it
		mbe2400_state_reset(s);
		p->L = 0;
		return MBE2400_FRAME_SILENCE;
	}
	L = mbe2400_L[b0];
	p->L = (int16_t)L;
	p->w0 = mbe2400_w0(b0);
	// voicing: harmonic l is in 1 kHz band (l f0) / 1000 Hz, b1's highest bit the lowest band
	for (int l = 1; l <= L; l++)
		p->v[l - 1] = (uint8_t)((b[1] >> (3 - (int)(((uint32_t)l * p->w0) >> 29))) & 1);
	gamma = (int32_t)mbe2400_dg[b[2] & 63] * 8 + (s->gamma >> 1);

	// G2..G8, Q16, to R1..R8 = sum 2 G_m cos
	for (int m = 2; m <= 4; m++)
		G[m] = g24[m - 2] * 4096;
	for (int m = 5; m <= 8; m++)
		G[m] = g58[m - 5] * 512;
	for (int i = 1; i <= 8; i++) {
		int32_t sm = 0;

		for (int m = 2; m <= 8; m++)
			sm += fxp_mulshr(G[m], cos8(m, i), 15);
		R[i] = 2 * sm;
	}
	// each block: its first two terms from a pair of R, the next four from its codebook (b8's lowest bit is not sent),
	// then its inverse DCT T_j = C1 + 2 sum C_k cos(pi (k - 1)(j - 1/2) / J)
	for (int i = 0; i < 4; i++) {
		int J = block_len(L, i), nk = J < 6 ? J : 6;
		int32_t C[7];
		uint32_t unit = (1u << 30) / (uint32_t)J;
		const int8_t *h = &mbe2400_hoc[(i * 16 + (b[5 + i] & (i == 3 ? 14 : 15))) * 4];

		C[1] = (R[2 * i + 1] + R[2 * i + 2]) >> 1;
		C[2] = fxp_mulshr(R[2 * i + 1] - R[2 * i + 2], ISQRT8_Q15, 15);
		for (int k = 3; k <= nk; k++)
			C[k] = h[k - 3] * 512;
		for (int j = 1; j <= J; j++, pos++) {
			int32_t sm = 0;

			for (int k = 2; k <= nk; k++)
				sm += fxp_mulshr(C[k], cos_turn(unit, (k - 1) * (2 * j - 1)), 15);
			T[pos] = C[1] + 2 * sm;
			sumT += T[pos];
		}
	}
	mbe_predict(&s->q, L, RHO, pr);
	big = gamma - mbe_log2_q11((uint32_t)L) * 16 - sumT / L;
	for (int l = 1; l <= L; l++)
		p->a[l - 1] = (int16_t)fxp_clamp((T[l] + pr[l] + big + 16) >> 5, -32767, 32767);
	s->gamma = gamma;
	s->q.L = (int16_t)L;
	for (int l = 1; l <= L; l++)
		s->q.lg[l] = p->a[l - 1];
	s->q.lg[0] = s->q.lg[1];
	return MBE2400_FRAME_OK;
}

// the nearest of n vectors of dim values (in the codebook's Q) to x (the same Q) by their first use values; every
// step-th vector from the first
static int nearest(const int8_t *cb, int n, int dim, int use, int step, const int32_t *x)
{
	int32_t best = INT32_MAX;
	int pick = 0;

	for (int i = 0; i < n; i += step) {
		int32_t e = 0;

		for (int d = 0; d < use; d++) {
			int32_t v = x[d] - cb[i * dim + d];

			e += v * v;
		}
		if (e < best) {
			best = e;
			pick = i;
		}
	}
	return pick;
}

// a Q16 value in the Q of a codebook (2^q), held where its squared differences stay in range
static int32_t to_cb(int32_t v, int q) { return fxp_clamp((v + (1 << (15 - q))) >> (16 - q), -4096, 4096); }

// the gain code whose level is nearest want (log2 Q16)
static uint16_t gain_code(int32_t want)
{
	int32_t best = INT32_MAX;
	uint16_t pick = 0;

	for (int k = 0; k < 64; k++) {
		int32_t d = (int32_t)mbe2400_dg[k] * 8 - want;

		if (d < 0)
			d = -d;
		if (d < best) {
			best = d;
			pick = (uint16_t)k;
		}
	}
	return pick;
}

// log2 of a frame's energy, Q11: the sum of its harmonics' squared amplitudes
static int32_t energy(const mbe_params_t *p, int L)
{
	int32_t amax = -32768;
	uint32_t e = 0;

	for (int l = 0; l < L; l++)
		if (p->a[l] > amax)
			amax = p->a[l];
	for (int l = 0; l < L; l++) {
		int32_t m = mbe_exp2_q11(p->a[l] - amax + 12 * 2048);	// Q12, at most 2^12

		e += (uint32_t)(m * m);					// the loudest adds 2^24; 56 of them stay under 2^30
	}
	return 2 * amax + mbe_log2_q11(e | 1u);
}

// The fields for b0 (in range) and p. noinline: inlined, its arrays (600 B) stay in mbe2400_pack's frame under the
// dequantizer's (550 B), and the encoder's stack is their sum instead of the larger
static void __attribute__((noinline)) quantize(mbe2400_state_t *s, int b0, const mbe_params_t *p,
					       uint16_t b[MBE2400_FIELDS])
{
	int L = mbe2400_L[b0], pos = 1, nv[4] = { 0, 0, 0, 0 };
	int32_t x[MBE_MAXL + 1], pr[MBE_MAXL + 1], R[9], C[4][7], amax = -32768, sum = 0, mean;

	b[0] = (uint16_t)b0;

	// voicing: each 1 kHz band by the count of its voiced harmonics against its unvoiced ones
	{
		uint32_t w0 = mbe2400_w0(b0);

		for (int l = 1; l <= L; l++)
			nv[((uint32_t)l * w0) >> 29] += p->v[l - 1] ? 1 : -1;
		b[1] = 0;
		for (int k = 0; k < 4; k++)
			if (nv[k] > 0)
				b[1] |= (uint16_t)(8 >> k);
	}

	// the target, Q16, no harmonic far under the loudest
	for (int l = 0; l < L; l++)
		if (p->a[l] > amax)
			amax = p->a[l];
	for (int l = 1; l <= L; l++) {
		x[l] = (p->a[l - 1] < amax - A_FLOOR ? amax - A_FLOOR : p->a[l - 1]) * 32;
		sum += x[l];
	}
	mean = sum / L;

	// gain: the frame's mean log amplitude is gamma - log2(L) / 2
	b[2] = gain_code(mean + mbe_log2_q11((uint32_t)L) * 16 - (s->gamma >> 1));

	// the shape: the residual after the prediction, each block's DCT (C_k = 1/J sum r_j cos), and from the first two
	// terms of each block the pair R_2i-1 = C1 + sqrt 2 C2, R_2i = C1 - sqrt 2 C2. The mean is the gain's
	mbe_predict(&s->q, L, RHO, pr);
	for (int l = 1; l <= L; l++)
		x[l] -= pr[l] + mean;
	for (int i = 0; i < 4; i++) {
		int J = block_len(L, i), nk = J < 6 ? J : 6;
		uint32_t unit = (1u << 30) / (uint32_t)J;

		for (int k = 1; k <= nk; k++) {
			int32_t sm = 0;

			for (int j = 0; j < J; j++)
				sm += k == 1 ? x[pos + j] : fxp_mulshr(x[pos + j], cos_turn(unit, (k - 1) * (2 * j + 1)), 15);
			C[i][k] = sm / J;
		}
		pos += J;
		R[2 * i + 1] = C[i][1] + fxp_mulshr(C[i][2], SQRT2_Q15, 15);
		R[2 * i + 2] = C[i][1] - fxp_mulshr(C[i][2], SQRT2_Q15, 15);
	}
	// G_m = 1/8 sum R_i cos (the transform is orthogonal: the nearest G is the nearest R), each codebook by itself
	{
		int32_t g[7];

		for (int m = 2; m <= 8; m++) {
			int32_t sm = 0;

			for (int i = 1; i <= 8; i++)
				sm += fxp_mulshr(R[i], cos8(m, i), 15);
			g[m - 2] = to_cb(sm / 8, m <= 4 ? 4 : 7);
		}
		b[3] = (uint16_t)nearest(mbe2400_prba24, 512, 3, 3, 1, g);
		b[4] = (uint16_t)nearest(mbe2400_prba58, 128, 4, 4, 1, g + 3);
	}
	// each block's terms 3..6 (those it has: the distance leaves the others out); the last block's codebook by its even
	// entries (b8's lowest bit is not sent)
	for (int i = 0; i < 4; i++) {
		int J = block_len(L, i), n = (J < 6 ? J : 6) - 2;
		int32_t h[4];

		for (int k = 0; k < n; k++)
			h[k] = to_cb(C[i][k + 3], 7);
		b[5 + i] = (uint16_t)(n > 0 ? nearest(&mbe2400_hoc[i * 64], 16, 4, n, i == 3 ? 2 : 1, h) : 0);
	}
}

void mbe2400_pack(mbe2400_state_t *s, int b0, mbe_params_t *p, uint16_t b[MBE2400_FIELDS])
{
	mbe2400_state_t t = *s;
	int32_t want;
	int L;

	b0 = (int)fxp_clamp(b0, 0, MBE2400_PITCHES - 1);
	L = mbe2400_L[b0];
	want = energy(p, L);
	quantize(s, b0, p, b);
	// The gain code again, for the frame's energy. The first one sets the mean of the log amplitudes, and the shape
	// codebooks cannot follow a spectrum of a few strong harmonics over deep gaps (a tone, a whistle, a hum): they
	// fill the gaps and cut the peaks, the mean stays right and the frame plays up to 8 dB low, at a level that moves
	// from frame to frame. The gain shifts every log amplitude alike, so one trial through the dequantizer tells by
	// how much the receiver's frame misses the energy. Speech moves by about 1 dB
	(void)mbe2400_unpack(&t, b, p);
	b[2] = gain_code((int32_t)mbe2400_dg[b[2]] * 8 + (want - energy(p, L)) * 16);
	// advance the state as the receiver's dequantizer will, and hand back what it reconstructs
	(void)mbe2400_unpack(s, b, p);
}

// --- the decoder ---

// fading repeats: 3 at most, each 1 dB under the last
static const mbe_conceal_t conceal = { 3, 340 };

void mbe2400_dec_init(const mbe2400_mem_t *m)
{
	mbe2400_state_reset(m->s);
	mbe_synth_init(&m->mbe);
}

int mbe2400_dec_frame(const mbe2400_mem_t *m, const uint8_t *in, uint8_t *errs, int16_t pcm[MBE_FRAME_LEN])
{
	mbe_params_t *p = m->p;
	uint16_t b[MBE2400_FIELDS];
	uint8_t e = 0xFF;
	int st = -1, run;

	if (in) {
		e = mbe2400_frame_unpack(in, b);
		if (!MBE2400_ERRS_BAD(e))
			st = mbe2400_unpack(m->s, b, p);
	}
	if (errs)
		*errs = e;
	if (st == MBE2400_FRAME_SILENCE) {
		memset(p, 0, sizeof *p);
		p->w0 = m->mbe.prev->w0;
		return mbe_synth_conceal(&m->mbe, &conceal, p, 0, pcm);
	}
	if (st == MBE2400_FRAME_OK) {
		for (int l = 0; l < p->L; l++)
			p->a[l] = (int16_t)fxp_clamp(p->a[l] + OUT_GAIN, -32767, 32767);
		return mbe_synth_conceal(&m->mbe, &conceal, p, 0, pcm);
	}
	run = mbe_synth_conceal(&m->mbe, &conceal, NULL, 1, pcm);
	if (run > conceal.repeat_max)
		mbe2400_state_reset(m->s);
	return run;
}

// --- the encoder ---

void mbe2400_enc_init(const mbe2400_enc_mem_t *m)
{
	mbe2400_state_reset(m->s);
	mbe_est_init(m->est);
}

void mbe2400_enc_frame(const mbe2400_enc_mem_t *m, const int16_t pcm[MBE_FRAME_LEN], uint8_t out[MBE2400_BYTES])
{
	mbe_params_t *p = m->p;
	mbe_est_t e;
	uint16_t b[MBE2400_FIELDS];
	int b0;

	mbe_est_frame(m->est, pcm, m->buf, &e);
	if (e.zero) {
		// nothing but zeros: the silence frame, which the receiver decodes (and predicts from) like any other
		memcpy(out, mbe2400_silence, MBE2400_BYTES);
		mbe2400_frame_unpack(out, b);
		(void)mbe2400_unpack(m->s, b, p);
		m->est->vprev = 0;
		return;
	}
	b0 = mbe_est_pitch(m->est, m->buf, &e, &grid);
	if (b0 < 0)
		b0 = B0_UNVOICED;
	memset(p, 0, sizeof *p);
	p->flags = MBE_UV_DENSITY;
	p->w0 = mbe2400_w0(b0);
	p->L = (int16_t)mbe2400_L[b0];
	mbe_est_params_hz(m->est, m->buf, &e, p, 1000, 4);
	for (int l = 0; l < p->L; l++)		// PCM units to the model's
		p->a[l] = (int16_t)fxp_clamp(p->a[l] - OUT_GAIN, -8 * 2048, 32767);
	mbe2400_pack(m->s, b0, p, b);
	mbe2400_frame_pack(b, out);
}
