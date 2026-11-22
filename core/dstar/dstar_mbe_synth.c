// SPDX-License-Identifier: MIT AND ISC
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 mbe contributors
// SPDX-FileCopyrightText: 2010 mbelib Author (the synthesizer it ports, ISC license, see LICENSES/ISC.txt)
/*
 * Fixed-point MBE synthesizer (dstar_mbe_synth.h), ported from mbelib (szechyjs/mbelib 9a04ed5, mbelib.c and
 * ambe3600x2400.c):
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
 * The structure, equations and constants are mbelib's; the arithmetic is new. Where this file departs from mbelib it
 * says so and why.
 *
 * Number formats
 *   phases      uint32, 2^32 = one cycle. f0_q24 << 8 is the phase step of the fundamental a sample, exactly
 *   log2Ml      mbelib's log2 magnitudes, Q12
 *   log2A       log2 of a harmonic's PCM amplitude, Q12: mbelib's output is 7 x sum(Ml cos), so A = 7 Ml and
 *               log2A = log2Ml + log2 7
 *   amplitudes  for one frame the amplitudes of both overlapping frames are converted to int16 with one shared
 *               exponent e (A16 = A 2^e, largest < 2^14), so every product in the sample loop is 16 x 16 bits and the
 *               accumulator needs no 64-bit arithmetic
 */
#include "dstar/dstar_mbe_synth.h"
#include "dstar/dstar_mbe_fixed.h"

#define N		DSTAR_MBE_SYNTH_FRAME
#define LOG_ZERO	(-16777216)	// log2 of a zero amplitude
#define LOG2_7		11499		// log2(7), Q12: mbelib's output gain
#define LOG2_UNVC	(-9376)		// log2(0.2046), Q12: the unvoiced scale
#define LOG2_2PI	10861		// log2(2 pi), Q12
#define LOG2_096PI	6523		// log2(0.96 pi), Q12
#define LOG2_1P2	1077		// log2(1.2), Q12
#define LOG2_UVGAIN	1786		// log2(uvsine x qfactor) = log2(1.3591409 e x ln(3) / 3), Q12
#define LOG_MAX		(16 << 12)	// amplitudes clamped below 2^16 PCM

// xorshift32 (Marsaglia 2003): 0..2^31-1, as rand(). mbelib calls libc rand(); any uniform source will do, and this
// one is two lines and has no multiply
static uint32_t synth_rand(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x >> 1;
}

// mbe_initMbeParms. The noise generator keeps running across resets, as libc's rand() does under mbelib
static void reset(dstar_mbe_synth_t *s)
{
	s->f0_q24 = 250408;	// mbelib: w0 = 0.09378 rad
	s->L = 30;
	s->repeat = 0;
	s->have_last = 0;
	for (unsigned l = 0; l <= DSTAR_AMBE_MAX_L; ++l) {
		s->voiced[l] = 0;
		s->log2A[l] = LOG_ZERO;
		s->psi[l] = 0x40000000u;	// pi / 2
		s->phi[l] = 0;
	}
	for (unsigned i = 0; i < DSTAR_MBE_SYNTH_NTAIL; ++i)
		s->ntail[i] = 0;
}

void dstar_mbe_synth_init(dstar_mbe_synth_t *s)
{
	s->rng = 0x2545f491u;
	reset(s);
}

// log2(w0) for w0 = 2 pi f0, Q12
static int32_t log2_w0(uint32_t f0_q24)
{
	return LOG2_2PI + dstar_mbe_log2_q12(f0_q24) - (24 << 12);
}

// mbe_spectralAmpEnhance in the log domain. lm[1..L] are log2Ml and are replaced by the enhanced values.
//
// mbelib: R0 = sum M^2, R1 = sum M^2 cos(w0 l),
//   W_l = sqrt(M_l) [0.96 pi (R0^2 + R1^2 - 2 R0 R1 cos(w0 l)) / (w0 R0 (R0^2 - R1^2))]^(1/4),
//   M_l *= clamp(W_l, 0.5, 1.2) for 8 l > L, then all M rescaled so that sum M^2 is unchanged.
// With rho = R1 / R0 the bracket is (1 + rho^2 - 2 rho cos) / (w0 R0 (1 - rho^2)), which needs no squares of R0 and so
// fits 32 bits. The magnitudes are normalized to the largest before going linear; the result is scale-invariant, so
// nothing is lost
static void enhance(int32_t *lm, unsigned L, uint32_t f0_q24)
{
	int32_t mx = lm[1];
	for (unsigned l = 2; l <= L; ++l) {
		if (lm[l] > mx)
			mx = lm[l];
	}
	uint32_t step = f0_q24 << 8;
	int32_t r0 = 0, r1 = 0;
	for (unsigned l = 1; l <= L; ++l) {
		int32_t m = (int32_t)dstar_mbe_pow2_q12(lm[l] - mx + (14 << 12));	// Q14, <= 1
		int32_t m2 = (m * m) >> 14;
		r0 += m2;
		r1 += (m2 * dstar_mbe_cos(step * l)) >> 15;
	}
	if (r0 <= 0)
		return;
	// rho in Q15. |r1| <= r0 < 2^21; scale both under 2^16 first
	int sh = 0;
	while ((r0 >> sh) >= (1 << 16))
		++sh;
	int32_t rho = ((r1 >> sh) * 32768) / ((r0 >> sh) ? (r0 >> sh) : 1);
	int32_t rho2 = (rho * rho) >> 15;
	int32_t one_m = 32768 - rho2;
	if (one_m < 1)
		one_m = 1;
	int32_t log_r0 = dstar_mbe_log2_q12((uint32_t)r0) - (14 << 12) + 2 * mx;	// log2 sum Ml^2
	int32_t k = LOG2_096PI - log2_w0(f0_q24) - log_r0 - (dstar_mbe_log2_q12((uint32_t)one_m) - (15 << 12));
	int32_t s2 = 0;
	for (unsigned l = 1; l <= L; ++l) {
		if (8 * l > L) {
			int32_t c = dstar_mbe_cos(step * l);
			int32_t t = 32768 + rho2 - ((2 * rho * c) >> 15);	// 1 + rho^2 - 2 rho c, Q15
			if (t < 1)
				t = 1;
			int32_t lw = lm[l] / 2 + (k + dstar_mbe_log2_q12((uint32_t)t) - (15 << 12)) / 4;
			if (lw > LOG2_1P2)
				lw = LOG2_1P2;
			if (lw < -4096)
				lw = -4096;
			lm[l] += lw;
		}
		int32_t m = (int32_t)dstar_mbe_pow2_q12(lm[l] - mx + (14 << 12));	// Q14, <= 1.2
		s2 += (m * m) >> 14;
	}
	// gamma = sqrt(R0 / sum M'^2), in log2
	int32_t g = (dstar_mbe_log2_q12((uint32_t)r0) - dstar_mbe_log2_q12((uint32_t)(s2 ? s2 : 1))) / 2;
	for (unsigned l = 1; l <= L; ++l)
		lm[l] += g;
}

// Amplitude at the shared exponent e, for log2 value la (Q12)
static int32_t amp16(int32_t la, int e)
{
	return la <= LOG_ZERO ? 0 : (int32_t)dstar_mbe_pow2_q12(la + e * 4096);
}

// mbelib's Ws: the current frame fades in over n = 55..105 in steps of 0.02 and the previous frame fades out as
// 1 - Ws[n]. Q15
#define FADE0		55
#define FADE1		105

// One frame's sinusoid in one band (an unvoiced band is noise_frame's, so its a16 is only used for the exponent)
typedef struct {
	int32_t a16;		// amplitude at the shared exponent; 0 = silent
	uint32_t ph0;		// phase at n = 0
	uint32_t step[1];	// phase step a sample
} comp_t;

// A voiced component alone, over the samples where its weight is non-zero: A cos(ph0 + step n), at full weight outside
// the fade
static void v_add(int32_t *acc, const comp_t *c, int fade_in)
{
	const int32_t a16 = c->a16;
	const uint32_t step = c->step[0];
	if (a16 == 0)
		return;
	int n = fade_in ? FADE0 + 1 : 0;
	uint32_t ph = c->ph0 + step * (uint32_t)n;
	if (!fade_in) {
		for (; n <= FADE0; ++n, ph += step)
			acc[n] += (dstar_mbe_cos(ph) * a16) >> 12;
	}
	for (; n < FADE1; ++n, ph += step) {
		int32_t w = dstar_mbe_ws_ramp_q15[n - FADE0];
		acc[n] += (dstar_mbe_cos(ph) * ((a16 * (fade_in ? w : 32768 - w)) >> 15)) >> 12;
	}
	if (fade_in) {
		for (; n < N; ++n, ph += step)
			acc[n] += (dstar_mbe_cos(ph) * a16) >> 12;
	}
}

// --- Changes measured on real DVSI frames (18 Nov 2026) ---------------------------------------------------------------
//
// Each was measured alone and together: STOI and MCD over the research's 12 round trips (flite and LibriSpeech, clean,
// at 1/3/5 % bit errors and in fades), and on 357 voice frames off air from an IC-9700 (a DVSI chip; no reference
// exists for those) the clipping and the 50/100 Hz modulation of the output envelope, which is how cross-fade and
// phase artifacts at the frame rate show. Clean channel, before -> after: STOI 0.865 -> 0.876, MCD 21.70 -> 21.51 dB,
// frame-rate modulation 0.183 -> 0.104 (mbelib 0.138), clipped samples 291 -> 0. STOI also rose at every error rate
// tested. Rejected: changing the noise's cross-fade length or level, and any change to mbelib's enhancer (reference
// metrics punish every post-filter, so they cannot judge one).

// A voiced harmonic whose pitch barely moved: one sinusoid, its amplitude linear from the previous frame's to the
// current's, its phase from PHI_prev at n = 0 to PHI_cur at n = N with the frequency moving linearly between (mbelib's
// commented-out eq 134-138, from the IMBE description). IMBE does this below the 8th harmonic only; every harmonic
// measured better (MCD -0.15 dB, STOI +0.003)
static void vi_add(int32_t *acc, int32_t pa16, int32_t ca16, uint32_t pphi, uint32_t cphi, uint32_t pf, uint32_t cf,
		   unsigned l)
{
	uint32_t ps = (pf << 8) * l, cs = (cf << 8) * l;
	int32_t dphi = (int32_t)(cphi - pphi - (ps + cs) * (uint32_t)(N / 2));	// wrapped to +-half a cycle
	int32_t d2 = (int32_t)(cs - ps);
	int32_t dd = d2 / N;	// the frequency's change a sample
	uint32_t inc = ps + (uint32_t)(dphi / N) + (uint32_t)(d2 / (2 * N));
	uint32_t ph = pphi;
	int32_t a = pa16 * 256, da = (ca16 - pa16) * 256 / N;
	for (int n = 0; n < N; ++n) {
		acc[n] += (dstar_mbe_cos(ph) * (a >> 8)) >> 12;
		ph += inc;
		inc += (uint32_t)dd;
		a += da;
	}
}

// 32 x Q15 -> 32 bits without a 64-bit product (none on this core), for |x| < 2^25
static inline int32_t mulq15(int32_t x, int32_t t)
{
	return (x >> 15) * t + (((x & 0x7fff) * t) >> 15);
}

// In-place inverse FFT, 256 points, unscaled (radix 2, decimation in time)
static void ifft256(int32_t *re, int32_t *im)
{
	const unsigned n = DSTAR_MBE_SYNTH_NFFT;
	for (unsigned i = 1, j = 0; i < n; ++i) {
		unsigned bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j |= bit;
		if (i < j) {
			int32_t t = re[i];
			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	for (unsigned len = 2; len <= n; len <<= 1) {
		unsigned half = len >> 1;
		uint32_t dph = 0xFFFFFFFFu / len + 1u;	// 2^32 / len
		for (unsigned k = 0; k < half; ++k) {
			int32_t wr = dstar_mbe_cos(dph * k), wi = dstar_mbe_sin(dph * k);
			for (unsigned i = k; i < n; i += len) {
				unsigned j = i + half;
				int32_t tr = mulq15(re[j], wr) - mulq15(im[j], wi);
				int32_t ti = mulq15(re[j], wi) + mulq15(im[j], wr);
				re[j] = re[i] - tr;
				im[j] = im[i] - ti;
				re[i] += tr;
				im[i] += ti;
			}
		}
	}
}

// Unvoiced bands as noise. mbelib's three fixed sines per band sound tonal ("watery"); this is a random-phase spectrum
// over the unvoiced bands' bins (bin k, at k/256 of the sample rate, belongs to the nearest harmonic), magnitudes
// interpolated in log between neighboring unvoiced band centers, an inverse FFT, and a window that rises over n =
// 55..105 of this frame (where the voiced cross-fade is) and falls over 55..105 of the next, so successive independent
// segments are power-complementary. A band's power is the multisine's (three sines of amplitude a: 3 a^2 / 2), so the
// voiced/unvoiced balance is unchanged. STOI +0.009. la[l]: log2 PCM amplitudes, Q12, unvoiced scale applied
//
// Unvoiced bands at DVSI's level (19 Nov 2026). Against an IC-9700's own decode of the same 681 DVSI frames (an ID-51,
// BER 0; the 9700's output recorded over LAN and aligned to a sample), mbelib's unvoiced scale played the noise too
// loud, and more so the lower the frame's pitch: all-unvoiced frames +6.6 dB against voiced ones, DVSI's background
// frames (b0 120, 69 Hz) +13 dB, frames DVSI codes all unvoiced at a voice pitch +10 dB. DVSI frames of known speech
// (an IC-9700 coding a ground-truth clip, no Icom receiver involved) said the same against the source: +8.7 dB at
// b0 34, +11.5 dB at a voice pitch. That is what a spectral density read as a band amplitude does: mbelib's band
// power goes as M^2 / w0, a density's as M^2 w0. So the unvoiced amplitude is mbelib's times w0 / w0(b0 34) (render),
// and the noise 6 dB quieter (UV_NGAIN); then the ground truth's unvoiced frames sit 0.8-1.6 dB from the source's and
// the 9700's 0.4 dB from ours. The analyzer codes the same law (dstar_mbe_analysis.c), so our own frames round trip
// with the law cancelled and the noise 1.5 dB quieter, which measured better (12 inputs, MCD 20.75 -> 20.28 dB, STOI
// 0.8638 -> 0.8654). On the 9700's frames, against its decode: STOI 0.763 -> 0.795, log-spectral distance 12.8 ->
// 10.3 dB; on the ground truth STOI 0.646 -> 0.686
#define UV_LW0_REF	(-9684)		// log2(w0) at b0 34 (247.3 Hz), Q12
#define UV_NGAIN	(-4096)		// -6 dB, log2 amplitude Q12

// A noise floor under voiced harmonics from 1 kHz up (19 Nov 2026), 17 dB below each harmonic's power. Our voiced
// bands were too clean: against an IC-9700's decode of the same DVSI frames, the harmonics stood 4.5 dB (1-2 kHz) and
// 2.5 dB (2-3.5 kHz) further above the floor between them than the 9700's (40 ms windows, steady-pitch frames), and
// 5 dB further than the source's on DVSI frames of known speech: the buzz of a vocoder. With the floor 2.1 and 0.7 dB.
// -15 dB matched the 9700 at 1-2 kHz but cost our own round trip 0.15 dB of MCD, -17 dB costs 0.08 (STOI +0.0007);
// a slope with frequency or a start at 1.5 kHz did no better. Cross-fading the upper harmonics instead of
// interpolating them (mbelib's way) overshot: less peaky than the 9700's above 2 kHz, frame-rate modulation up. The
// floor rides in noise_frame's inverse FFT, which then runs on fully voiced frames too
#define VN_FLOOR	(-11565)	// -17 dB in power, log2 amplitude Q12
#define VN_FROM_HZ	1000u
static inline int vn_on(uint32_t l, uint32_t f0)
{
	return ((f0 >> 8) * l * 8000u >> 16) >= VN_FROM_HZ;
}

#define NX0		55		// the noise's fade-in starts here
#define NXF		50		// and lasts this long
#define NSEG		(N + NXF)	// a frame's noise segment, its fades included
static void noise_frame(dstar_mbe_synth_t *s, dstar_mbe_synth_scratch_t *t, const int32_t *la, const uint8_t *v,
			unsigned L, uint32_t f0)
{
	int32_t *re = t->re, *im = t->im, *out = t->nz;
	uint8_t *band = t->band, *cnt = t->ncnt;
	int32_t *lm = t->nlm;
	for (unsigned l = 0; l <= L + 1; ++l)
		cnt[l] = 0;
	int any = 0;
	for (unsigned k = 0; k < DSTAR_MBE_SYNTH_NFFT / 2; ++k) {
		uint32_t l = (((uint32_t)k << 16) + (f0 >> 1)) / f0;	// the nearest harmonic
		band[k] = (l >= 1 && l <= L && (!v[l] || vn_on(l, f0))) ? (uint8_t)l : 0;
		if (band[k]) {
			cnt[l]++;
			any = 1;
		}
	}
	for (unsigned n = 0; n < N; ++n)
		out[n] = n < DSTAR_MBE_SYNTH_NTAIL ? s->ntail[n] : 0;
	if (!any) {
		for (unsigned i = 0; i < DSTAR_MBE_SYNTH_NTAIL; ++i)
			s->ntail[i] = 0;
		return;
	}
	// a bin's log2 magnitude: unvoiced la + UVGAIN + UV_NGAIN + (log2 3 - log2 n_l) / 2; under a voiced harmonic,
	// la + VN_FLOOR - log2(n_l) / 2, so the band's noise power is the floor times the harmonic's A^2 / 2
	int32_t top = LOG_ZERO;
	for (unsigned l = 1; l <= L; ++l) {
		if (!cnt[l] || la[l] <= LOG_ZERO) {
			lm[l] = LOG_ZERO;
			continue;
		}
		if (v[l])
			lm[l] = la[l] + VN_FLOOR - dstar_mbe_log2_q12(cnt[l]) / 2;
		else
			lm[l] = la[l] + LOG2_UVGAIN + UV_NGAIN + (6492 - dstar_mbe_log2_q12(cnt[l])) / 2;
		if (lm[l] > top)
			top = lm[l];
	}
	int en = 18 - ((top + 4095) >> 12);	// the largest bin just under 2^18, so the sum of 127 stays under 2^25
	for (unsigned k = 0; k < DSTAR_MBE_SYNTH_NFFT; ++k) {
		re[k] = 0;
		im[k] = 0;
	}
	for (unsigned k = 1; k < DSTAR_MBE_SYNTH_NFFT / 2; ++k) {
		unsigned l = band[k];
		if (!l || lm[l] <= LOG_ZERO)
			continue;
		int32_t lk = lm[l];
		// the bin's place relative to harmonic l, in 1/256 of a band: -128..128; toward the neighbor on that side
		int32_t pos = (int32_t)((((uint32_t)k << 16) % f0) * 256u / f0);
		if (pos > 128)
			pos -= 256;
		unsigned m = pos >= 0 ? l + 1 : l - 1;
		if (m >= 1 && m <= L && lm[m] > LOG_ZERO)
			lk = lm[l] + (((lm[m] - lm[l]) >> 4) * (pos < 0 ? -pos : pos) >> 4);
		int32_t x = (int32_t)dstar_mbe_pow2_q12(lk + en * 4096);
		uint32_t ph = synth_rand(&s->rng) << 1;
		re[k] = (x * (dstar_mbe_cos(ph) >> 3)) >> 12;	// x < 2^18, cos >> 3 < 2^12
		im[k] = (x * (dstar_mbe_sin(ph) >> 3)) >> 12;
	}
	ifft256(re, im);
	const uint32_t wstep = 0x40000000u / NXF;
	for (unsigned j = 0; j < NSEG; ++j) {
		int32_t w = j < NXF ? dstar_mbe_sin(wstep * j) : j < N ? 32767 : dstar_mbe_cos(wstep * (j - N));
		int32_t y = mulq15(re[j], w);
		y = en >= 0 ? (y + (en ? (1 << (en - 1)) : 0)) >> en : y * (1 << -en);
		if (j < N - NX0)
			out[NX0 + j] += y;
		else
			s->ntail[j - (N - NX0)] = y;
	}
}

// Voiced phase. mbelib re-randomizes the phase of the upper three quarters of the harmonics every frame, in proportion
// to the unvoiced band count; across the cross-fade that is heard as roughness. Instead: the minimum phase a smooth
// envelope implies, a truncated discrete Hilbert transform of the log magnitudes at the harmonics,
// theta_l = gamma / pi sum_k (lnM_{l+k} - lnM_{l-k}) / k, k = 1..19, gamma 0.44 (best of 0 to 1; any randomness kept was
// worse). Regenerating MBE phase from the magnitudes in this form is published (US 5,701,390, expired). With the
// interpolation and the noise it gave the best MCD and halved the frame-rate modulation. lm: log2 magnitudes, Q12
//
// Plus a fixed dispersion, 12/1000 of a cycle times l^2 (19 Nov 2026), the same every frame so it costs no continuity.
// The minimum phase lines the harmonics up into one sharp pulse a period, which is heard as buzz: against an IC-9700's
// decode of the same DVSI frames our voiced frames' crest factor (300-3000 Hz) was 9.7 dB to its 8.4, and on DVSI
// frames of known speech 12.0 dB against the source's own 10.1. The quadratic phase is a chirp: harmonic l is delayed
// by about (2l + 1) x 0.012 periods, so the pulse spreads over about half a period by the 20th harmonic. 3, 6, 12, 25
// and 40 measured; 12 brings the known speech to the source's crest (9.8 dB; the 9700's 9.0, whose own audio filters
// disperse it further), with ground-truth STOI 0.686 -> 0.696 and the rest unchanged (our round trip MCD +0.03 dB)
#define PDISP_Q32	51539608u	// 0.012 x 2^32
static void env_phase(const int32_t *lm, unsigned L, uint32_t *theta)
{
	static const uint16_t inv_q15[20] = { 0, 32768, 16384, 10923, 8192, 6554, 5461, 4681, 4096, 3641, 3277, 2979,
					      2731, 2521, 2341, 2185, 2048, 1928, 1820, 1725 };
	const uint32_t C = 16200;	// 0.44 ln 2 / (2 pi^2) x 2^32 / 4096: Q12 log2 sum -> phase
	for (unsigned l = 1; l <= L; ++l) {
		int32_t sum = 0;
		for (unsigned k = 1; k < 20; ++k) {
			unsigned hi = l + k > L ? L : l + k, lo = l > k ? l - k : 1;
			int32_t d = lm[hi] - lm[lo];
			if (d > (24 << 12))
				d = 24 << 12;
			if (d < -(24 << 12))
				d = -(24 << 12);
			sum += ((d >> 2) * (int32_t)inv_q15[k]) >> 13;
		}
		theta[l] = (uint32_t)sum * C + (uint32_t)(l * l) * PDISP_Q32;	// wraps modulo one cycle, as a phase should
	}
}

// Real frames peak above full scale on loud vowels (291 clipped samples in 7 s of an IC-9700's); linear to -2.5 dBFS,
// then K + F x / (x + F), which approaches full scale smoothly, instead of the hard clip's audible corners
static int32_t soft_limit(int32_t v)
{
	const int32_t K = 24576, F = 32760 - 24576;
	int32_t a = v < 0 ? -v : v;
	if (a <= K)
		return v;
	int32_t y = K + F - (F * F) / (a - K + F);
	return v < 0 ? -y : y;
}

// DVSI's decoders put back spectral depth that its analysis leaves out: an Icom ID-51 played our full-depth frames
// audibly but unintelligibly until our analyzer compressed the shape to 60 % with power kept (dstar_mbe_analysis.c). This
// puts depth back the same way, so our frames decode as DVSI's decoders decode them, and DVSI's as DVSI meant them: each
// frame's log magnitudes about their mean scaled by 1/0.73, then shifted so the frame's power is unchanged.
//
// 1/0.73, not the analyzer's exact inverse 1/0.6 (used until 19 Nov 2026): measured on both kinds of frame this decoder
// plays (docs/spec/dstar-codec.md 4), 1/0.6 -> 1/0.73 took our own frames (12 speech inputs) from MCD 22.96 to 20.75 dB
// and STOI 0.861 to 0.864, and an IC-9700's DVSI frames of known speech from STOI 0.610 to 0.647 (MCD 66.5 to 62.2 dB).
// 0.73 is DVSI's own harmonic spread against our analysis at full depth, measured on those frames. Less expansion helps
// DVSI's frames further (none: STOI 0.694) but flattens ours (none: MCD 33.6, STOI 0.833); 1/0.68 is our frames' best
// alone, 1/0.75 starts to cost them. A smaller factor also scales the shape's quantization and channel errors less
#define EXPAND_Q12	5611		// 1 / 0.73
static void expand_shape(dstar_ambe_params_t *q)
{
	if (q->L == 0)
		return;
	int32_t sum = 0, mx = q->log2_amp[1];
	for (unsigned l = 1; l <= q->L; ++l) {
		sum += q->log2_amp[l];
		if (q->log2_amp[l] > mx)
			mx = q->log2_amp[l];
	}
	int32_t m = sum / (int32_t)q->L;
	int32_t mx1 = m + (((mx - m) >> 4) * EXPAND_Q12 >> 8);
	uint32_t p0 = 0, p1 = 0;
	for (unsigned l = 1; l <= q->L; ++l) {
		int32_t x = q->log2_amp[l], y = m + (((x - m) >> 4) * EXPAND_Q12 >> 8);
		p0 += dstar_mbe_pow2_q12(2 * (x - mx) + (12 << 12));
		p1 += dstar_mbe_pow2_q12(2 * (y - mx1) + (12 << 12));
		q->log2_amp[l] = y;
	}
	// power kept: half the log2 ratio of the two sums, each taken relative to its own maximum
	int32_t g = (dstar_mbe_log2_q12(p0 ? p0 : 1) - dstar_mbe_log2_q12(p1 ? p1 : 1)) / 2 + mx - mx1;
	for (unsigned l = 1; l <= q->L; ++l)
		q->log2_amp[l] += g;
}

static void make_comp(comp_t *c, int32_t a16, uint32_t f0_q24, unsigned l, uint32_t phi, int is_cur)
{
	c->a16 = a16;
	uint32_t step = (f0_q24 << 8) * l;
	c->step[0] = step;
	// mbelib: previous frame cos(pw0 l n + PHI_prev), current frame cos(cw0 l (n - N) + PHI_cur)
	c->ph0 = is_cur ? phi - step * N : phi;
}

// The output level (19 Nov 2026): 4.4 dB down, to an IC-9700's. Over the same ID-51 over (681 DVSI frames), ours was
// 4.4 dB louder than the 9700's decode over active speech, after the noise changes above (4.7 before), and the operator heard
// it "slightly louder". mbelib's output gain of 7 was never matched to a DVSI decoder's; this matches one. The 9700's
// LAN audio level is the reference; its own AF gain is not in that path. Our own round trip plays 4.4 dB under its input
#define OUT_GAIN_Q15	19744		// 2^(-2994 / 4096): -4.4 dB

// mbe_synthesizeSpeech for the current frame (enhanced log2Ml in t->lm) against the stored previous frame; the current
// frame is then stored
static void synthesize(dstar_mbe_synth_t *s, dstar_mbe_synth_scratch_t *t, const dstar_ambe_params_t *p, int16_t *pcm)
{
	const unsigned cL = p->L, pL = s->L;
	const uint32_t cf = p->f0_q24, pf = s->f0_q24;
	const int32_t *lm = t->lm;
	int32_t *cA = t->cA, *acc = t->acc;
	uint8_t *cV = t->cV;
	uint32_t *pphi = t->pphi;
	unsigned maxl = cL > pL ? cL : pL;

	for (unsigned l = 1; l <= cL; ++l) {
		cV[l] = p->voiced[l] ? 1 : 0;
		int32_t a = lm[l] + LOG2_7;
		cA[l] = a > LOG_MAX ? LOG_MAX : a;
	}
	// mbelib eq 128/129: the shorter frame's missing harmonics are zero and voiced, so they fade out (or in) as plain
	// sinusoids
	for (unsigned l = cL + 1; l <= maxl; ++l) {
		cA[l] = LOG_ZERO;
		cV[l] = 1;
	}
	for (unsigned l = pL + 1; l <= maxl; ++l) {
		s->log2A[l] = LOG_ZERO;
		s->voiced[l] = 1;
	}

	// mbelib eq 139/140: phase continuity; then mbelib adds a random offset, this the envelope's phase (env_phase)
	uint32_t half = (pf + cf) << 8;	// (pw0 + cw0) as a phase step
	for (unsigned l = 1; l <= DSTAR_AMBE_MAX_L; ++l) {
		pphi[l] = s->phi[l];
		s->psi[l] += half * (l * (N / 2));
		s->phi[l] = s->psi[l] + (l <= cL ? t->theta[l] : 0);
	}

	// The shared exponent: the largest amplitude (unvoiced ones including the multisine gain) lands in [2^12, 2^13],
	// which bounds every product below and the accumulator below 2^27 even with 56 bands at full scale
	int32_t top = LOG_ZERO;
	for (unsigned l = 1; l <= maxl; ++l) {
		int32_t a = cA[l] + (cV[l] ? 0 : LOG2_UVGAIN), b = s->log2A[l] + (s->voiced[l] ? 0 : LOG2_UVGAIN);
		if (a > top)
			top = a;
		if (b > top)
			top = b;
	}
	int e = top <= LOG_ZERO / 2 ? 0 : 13 - ((top + 4095) >> 12);
	if (e > 24)
		e = 24;
	for (int n = 0; n < N; ++n)
		acc[n] = 0;

	for (unsigned l = 1; l <= maxl; ++l) {
		int pv = s->voiced[l], cv = cV[l];
		comp_t pc, cc;
		make_comp(&pc, pv ? amp16(s->log2A[l], e) : 0, pf, l, pphi[l], 0);
		make_comp(&cc, cv ? amp16(cA[l], e) : 0, cf, l, s->phi[l], 1);
		if (pv && cv) {
			uint32_t df = cf > pf ? cf - pf : pf - cf;
			if (df < cf / 10)	// pitch moved under 10 %: interpolate (vi_add)
				vi_add(acc, pc.a16, cc.a16, pphi[l], s->phi[l], pf, cf, l);
			else {			// eq 133
				v_add(acc, &pc, 0);
				v_add(acc, &cc, 1);
			}
			continue;
		}
		// an unvoiced side is noise_frame's; the voiced side fades out or in
		if (pv)
			v_add(acc, &pc, 0);
		if (cv)
			v_add(acc, &cc, 1);
	}
	noise_frame(s, t, cA, cV, cL, cf);

	// mbe_floattoshort: the gain 7 is already in A; the noise added; the output level (OUT_GAIN_Q15); then limited
	// (soft_limit) and truncated toward zero, as the (short) cast does
	int sh = e + 3;
	for (int n = 0; n < N; ++n) {
		int32_t v = acc[n];
		if (sh > 0)
			v = v >= 0 ? v >> sh : -((-v) >> sh);
		else
			v *= 1 << -sh;
		v = soft_limit(mulq15(v + t->nz[n], OUT_GAIN_Q15));
		if (v > 32760)
			v = 32760;
		if (v < -32760)
			v = -32760;
		pcm[n] = (int16_t)v;
	}

	s->f0_q24 = cf;
	s->L = (uint8_t)cL;
	for (unsigned l = 1; l <= DSTAR_AMBE_MAX_L; ++l) {
		s->voiced[l] = l <= cL ? cV[l] : 1;
		s->log2A[l] = l <= cL ? cA[l] : LOG_ZERO;
	}
}

static void silence(int16_t *pcm)
{
	for (int n = 0; n < N; ++n)
		pcm[n] = 0;
}

// The parameters -> mbelib's log2Ml, enhanced, then the unvoiced scale (the order is one of the two departures from
// mbelib, dstar_mbe_synth.h). mbelib forms Ml = exp(0.693 log2Ml): 0.693 is ln 2 rounded, so its magnitudes are
// 2^(0.99979 log2Ml); the factor is kept to match it
static void render(dstar_mbe_synth_t *s, dstar_mbe_synth_scratch_t *t, const dstar_ambe_params_t *p0, int16_t *pcm)
{
	int32_t *lm = t->lm;
	dstar_ambe_params_t *p = &t->ex;
	*p = *p0;
	expand_shape(p);
	for (unsigned l = 1; l <= p->L; ++l) {
		int32_t x = p->log2_amp[l];
		x -= (x * 55) >> 18;
		lm[l] = x;
	}
	enhance(lm, p->L, p->f0_q24);
	env_phase(lm, p->L, t->theta);
	int32_t lw = log2_w0(p->f0_q24);
	int32_t uv = LOG2_UNVC - lw / 2 + (lw - UV_LW0_REF);	// mbelib's 0.2046 / sqrt(w0), then DVSI's density law
	for (unsigned l = 1; l <= p->L; ++l) {
		if (!p->voiced[l])
			lm[l] += uv;
	}
	synthesize(s, t, p, pcm);
}

void dstar_mbe_synth_mute(dstar_mbe_synth_t *s, int16_t pcm[DSTAR_MBE_SYNTH_FRAME])
{
	silence(pcm);
	reset(s);
}

void dstar_mbe_synth_frame(dstar_mbe_synth_t *s, dstar_mbe_synth_scratch_t *t, const dstar_ambe_params_t *p,
			   int16_t pcm[DSTAR_MBE_SYNTH_FRAME])
{
	if (p->L == 0 || p->L > DSTAR_AMBE_MAX_L) {
		dstar_mbe_synth_mute(s, pcm);
		return;
	}
	s->repeat = 0;
	s->last = *p;
	s->have_last = 1;
	render(s, t, p, pcm);
}

// Concealment. mbelib repeats the last good frame at full level three times and then cuts to silence: a repeated voiced
// frame is a held buzz, and the cut is a hard gap. Here each repeat is a little quieter, and after the repeats the last
// spectrum continues as unvoiced noise (comfort noise) fading to a floor, so a run of lost frames sounds like a dropout
// in a noisy channel rather than a buzz and a click; only a long run mutes and resets. Repetition with fading and noise
// substitution are the textbook packet-loss remedies (Perkins, Hodson and Hardman, IEEE Network 12(5), 1998); the
// constants are the best of the research's sweep at 1, 3 and 5 % bit errors, measured with the codec's bad-frame rule
// (dstar_codec.c; the figures are in docs/spec/dstar-codec.md 7)
#define BAD_REPEATS	3			// frames repeated before the noise
#define BAD_ATT_Q12	680			// 1 dB quieter a repeated frame, log2 amplitude Q12
#define CN_FLOOR_Q12	(-5 * 4096)		// the noise's floor, 30 dB below the last good frame (the mute comes first, at
						// 28 dB; kept as the research swept it, should CN_FRAMES grow)
#define CN_FRAMES	25			// 0.5 s of noise, then silence

void dstar_mbe_synth_bad(dstar_mbe_synth_t *s, dstar_mbe_synth_scratch_t *t, int16_t pcm[DSTAR_MBE_SYNTH_FRAME])
{
	if (!s->have_last || s->repeat >= BAD_REPEATS + CN_FRAMES) {
		dstar_mbe_synth_mute(s, pcm);
		return;
	}
	unsigned r = ++s->repeat;
	dstar_ambe_params_t *p = &t->rep;
	*p = s->last;
	int32_t att = -(int32_t)(BAD_ATT_Q12 * r);
	int cn = r > BAD_REPEATS;
	if (cn && att < CN_FLOOR_Q12)
		att = CN_FLOOR_Q12;
	for (unsigned l = 1; l <= p->L; ++l) {
		p->log2_amp[l] += att;
		if (cn)
			p->voiced[l] = 0;
	}
	render(s, t, p, pcm);
}
