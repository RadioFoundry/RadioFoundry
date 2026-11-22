// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 mbe contributors
/*
 * Fixed-point MBE analysis for the D-STAR AMBE 3600x2400 codec (dstar_mbe_analysis.h).
 *
 * Written clean room from Griffin & Lim, "Multiband Excitation Vocoder" (IEEE Trans. ASSP 36(8), 1988), Hardwick's MIT
 * thesis (1988), the expired DVSI patents on MBE analysis and pitch tracking, and general DSP texts; the output
 * conventions are mbelib's (ISC) and the quantizer's. No other codec's source was consulted (docs/spec/dstar-codec.md,
 * Provenance).
 *
 * Per call (one 20 ms frame):
 *   1. DC-block the new samples into a 640-sample history.
 *   2. The pitch error E(P) of the newest frame on a half-sample grid, 20 to 127 samples, from a 1 kHz low-passed copy.
 *   3. Track the pitch of the frame two frames older from E of it, of the two frames after it (look-ahead) and the two
 *      before it (look-back), with a check for pitch doubling.
 *   4. A 256-point FFT of that frame; refine the pitch to 1/8 sample by harmonic fitting.
 *   5. Voicing per 1 kHz pair of bands (the D-STAR voicing field's resolution) from the fit's residual against a
 *      measured noise baseline, scaled by the low band's periodicity and its loudness against the upper bands.
 *   6. Snap f0 to the D-STAR grid, measure band energies there, convert to mbelib's log2 magnitude scale (the
 *      derivation is at step 6b).
 */
#include "dstar/dstar_mbe_analysis.h"
#include "dstar/dstar_mbe_fixed.h"

#include <stddef.h>

#define PW_LEN		301				// pitch window
#define PW_HALF		150
#define LPF_HALF	10				// pitch-path low-pass, 21 taps
#define PW_CENTER	(DSTAR_MBE_ANA_BUF - 1 - PW_HALF - LPF_HALF)	// newest frame: 479
#define SW_HALF		110				// spectral window 221
#define CENTER		(PW_CENTER - 2 * DSTAR_MBE_FRAME)	// analyzed frame: 159
#define P2_MIN		40				// pitch grid, half samples: 20.0 .. 127.0
#define E_ONE		4096				// E = 1.0: no periodicity

// Output scale constants, Q12 log2 (dstar_mbe_analysis.h)
#define LOG2_7		11499		// mbelib's output gain 7
#define LOG2_INV_UNVC	9376		// -log2(0.2046)
#define LOG2_KUV	14158		// log2(Kuv), Kuv^2 = 1.5 (7 x 1.352951)^2 x 0.895875
#define LOG2_2PI	10861
#define LOG_FLOOR	(-12 * 4096)	// band power floor, log2 PCM^2

// Unvoiced bands are coded in DVSI's convention, the one dstar_mbe_synth.c plays (see "Unvoiced bands at DVSI's
// level" there): as a spectral density, the band's amplitude times sqrt(w0) taken off once more, about the pitch of
// b0 34 where every all-unvoiced frame goes, plus 4.5 dB. mbelib's convention, which this coded until 19 Nov 2026, is a
// band amplitude; DVSI's decoders therefore heard our noise at a voice pitch about 6 dB quieter than meant, and the
// decoder, now playing DVSI's frames at DVSI's level, would play ours as much too quiet without this. On our own round
// trip the pair cancels the law, and the gain against the decoder's -6 dB was chosen on it (12 inputs, +3 / +4.5 /
// +6 dB: MCD 20.31 / 20.40 / 20.93 dB, STOI 0.8647 / 0.8662 / 0.8641; before 20.75 / 0.8638); white noise comes back
// 1.5 dB quieter
#define UV_LW0_REF	(-9684)		// log2(w0) at b0 34 (247.3 Hz), Q12
#define UV_GAIN		3072		// +4.5 dB, log2 amplitude Q12

// Tracker constants, on E summed over three frames (Q12). The structure (look-back against look-ahead, a preference for
// sub-multiples) is the patented tracker's; the values are the ones that structure is usually given, and moving
// CE_B_ACCEPT between 0.3 and 0.7 changed nothing measurable on the test speech
#define CE_B_ACCEPT	(E_ONE * 48 / 100)	// look-back taken outright below this
#define SUB_A_ABS	(E_ONE * 85 / 100)	// sub-multiple accepted if CE <= 0.85
#define SUB_A_REL	17			//   and CE <= 1.7 x CE(P_F) (tenths)
#define SUB_B_ABS	(E_ONE * 40 / 100)	// or CE <= 0.40
#define SUB_B_REL	35			//   and CE <= 3.5 x CE(P_F)
#define SUB_C_ABS	(E_ONE * 5 / 100)	// or CE <= 0.05
// The ratios compare against CE(P_F) floored at 0.1. A short period falls between half-sample grid points, so its
// sampled error is a few hundredths even for a perfect tone, while one of its multiples can land on an exact integer
// lag with an error near zero; without the floor that multiple wins every ratio test. (A 300 Hz tone: E = 0.041 at
// 26.5, 0.000 at 80.0.)
#define SUB_REL_FLOOR	(E_ONE * 10 / 100)

// Voicing. The fit residual of a band, as a fraction of its energy, is near 0 for a harmonic and, for noise, near the
// share of the band that one fitted window shape cannot explain. That share depends strongly on how many bins a
// harmonic spans (B = 256 / P), and the five-pitch search below lowers it further where a 1 % pitch change moves a
// harmonic by a bin or more (upper bands, low pitch). A fixed threshold therefore calls low-pitched noise voiced. The
// threshold is instead a fraction alpha of the statistic's measured mean on white noise, per 1 kHz pair, tabulated
// every 8 samples of pitch (by a tool of the research that measured it) and interpolated. (An earlier closed form,
// 1 - 2.1 / (B + 0.8), fitted single harmonics but not the searched pair statistic, and voiced 86 % of the upper bands
// of the fricative test.) alpha falls with frequency because real voiced harmonics fit worse higher up (jitter and
// pitch movement within the window scale with harmonic number) while speech above 2 kHz is more often noise
#define NOISE_ROWS	14
static const int16_t noise_base_q12[NOISE_ROWS][4] = {
	{ 3369, 2211, 3316, 3043 },	// P =  20.0
	{ 3078, 2286, 3040, 2775 },	// P =  28.0
	{ 2838, 2201, 2708, 2460 },	// P =  36.0
	{ 2582, 2128, 2396, 2087 },	// P =  44.0
	{ 2267, 1860, 2107, 1898 },	// P =  52.0
	{ 1967, 1674, 1862, 1704 },	// P =  60.0
	{ 1800, 1468, 1555, 1421 },	// P =  68.0
	{ 1548, 1262, 1331, 1216 },	// P =  76.0
	{ 1281, 1092, 1134, 1097 },	// P =  84.0
	{ 1130,  950,  993,  958 },	// P =  92.0
	{  986,  839,  864,  840 },	// P = 100.0
	{  838,  731,  766,  741 },	// P = 108.0
	{  730,  658,  689,  658 },	// P = 116.0
	{  652,  576,  599,  578 },	// P = 124.0
};
static const int16_t alpha_q12[4] = { 2704, 2590, 2365, 2141 };	// per 1 kHz pair: 0.66 .. 0.52
#define ALPHA_HYST	410	// +0.10 if the band was voiced last frame
// The time-domain error at the tracked pitch says how periodic the low band is. White noise gives E = 0.57..0.84
// (median 0.76: the minimum of a noisy function over 215 candidates); on the test speech, frames called voiced have a
// median of 0.09. alpha is scaled from full at E_LO down to zero at E_HI, so a frame with little periodicity below
// 1 kHz needs an almost perfect harmonic fit in a band to call it voiced. The values were swept on the test speech
// under the constraint that the noise and mix tests keep a margin (0.9 % of white-noise harmonics voiced, limit 5 %)
#define E_LO		(E_ONE * 55 / 100)
#define E_HI		(E_ONE * 90 / 100)
#define REFINE_BINS	96	// harmonic fit for pitch refinement up to 3 kHz
#define PAIR_EPS_Q12	20	// per-pair pitch step, 0.5 % (Q12)
#define PAIR_STEPS	2	// steps each side

void dstar_mbe_ana_init(dstar_mbe_ana_t *s)
{
	for (int i = 0; i < DSTAR_MBE_ANA_BUF; ++i)
		s->buf[i] = 0;
	s->hp_x1 = s->hp_y1 = 0;
	for (int f = 0; f < 3; ++f) {
		for (int i = 0; i < DSTAR_MBE_NPITCH; ++i)
			s->E[f][i] = E_ONE;
	}
	s->ring = 0;
	for (int k = 0; k < 8; ++k)
		s->prev_v[k] = 0;
	s->prev_p2 = 200;
	s->full_depth = 0;
	s->flat = 0;
	s->prev_e1 = s->prev_e2 = E_ONE;
	s->p8 = 800;
	s->e_tracked = E_ONE;
}

// --- 2. pitch error ---
// A normalized difference summed over the multiples of P,
//     E(P) = sum_k sum_n (x(n) - x(n+kP))^2 / sum_k sum_n (x(n)^2 + x(n+kP)^2),
// k >= 1, over the n where both samples fall inside the window. It is 0 for a periodic signal and about 1 for noise,
// like Griffin and Lim's period-fit error, but each lag is normalized by the energy of the two segments it compares (as
// YIN normalizes its difference function; de Cheveigne & Kawahara 2002), so a frame whose energy changes inside the
// 37 ms window keeps a low error at its pitch. Griffin and Lim's form, normalized by the whole (tapered) window's
// energy, gave E > 1 at the right pitch on 10 to 15 % of voiced frames of the test speech (a quiet frame between loud
// ones), and those frames were then synthesized as noise. The difference form compares sample with sample, so it needs
// an untapered window: under a taper even a periodic signal leaves a residual (v^2(n) - v^2(n+P)) s(n).
static void pitch_error(const int16_t *raw, dstar_mbe_ana_scratch_t *t, uint16_t *E)
{
	// Low-pass the window first (see the filter table): raw[0] is LPF_HALF samples before the window's first sample.
	// The filtered samples go to the FFT area, which is free until the spectrum
	int16_t *w = (int16_t *)t->u.f.im;
	int32_t m = 0;
	for (int n = 0; n < PW_LEN; ++n) {
		int32_t acc = 0;
		for (int k = 0; k < 2 * LPF_HALF + 1; ++k)
			acc += raw[n + k] * dstar_mbe_pitch_lpf_q15[k];
		acc = (acc + 16384) >> 15;
		w[n] = (int16_t)(acc > 32767 ? 32767 : (acc < -32768 ? -32768 : acc));
		int32_t a = acc < 0 ? -acc : acc;
		if (a > m)
			m = a;
	}
	if (m < 4) {	// digital silence: no pitch information
		for (int i = 0; i < DSTAR_MBE_NPITCH; ++i)
			E[i] = E_ONE;
		return;
	}
	// Samples to 10 bits, so 301 products and 2 r(0) fit int32
	const int bits = 9;
	int b = 31 - dstar_mbe_clz((uint32_t)m);
	for (int n = 0; n < PW_LEN; ++n)
		t->x[n] = (int16_t)(b >= bits ? w[n] >> (b - bits) : w[n] * (1 << (bits - b)));
	// Autocorrelation: the bulk of the analyzer's work (about 40 k multiply-adds). Lags 1..19 are never read (the
	// shortest period is 20 samples, and d below needs only r(0)); four products an iteration halve the loop overhead
	int32_t *r = t->u.a.r;
	for (int lag = 1; lag < P2_MIN / 2; ++lag)
		r[lag] = 0;
	for (int lag = 0; lag < PW_LEN; lag = lag ? lag + 1 : P2_MIN / 2) {
		int32_t a0 = 0, a1 = 0;
		const int16_t *p = t->x, *q = t->x + lag;
		int n = PW_LEN - lag;
		for (; n >= 4; n -= 4, p += 4, q += 4) {
			a0 += p[0] * q[0] + p[1] * q[1];
			a1 += p[2] * q[2] + p[3] * q[3];
		}
		for (; n > 0; --n)
			a0 += *p++ * *q++;
		r[lag] = a0 + a1;
	}
	// d(t) = the energy of the two segments that lag t overlaps, e(0..N-1-t) + e(t..N-1)
	int32_t *dd = t->u.a.d;
	int32_t head = 0, tail = 0;
	for (int lag = 0; lag < PW_LEN; ++lag) {
		dd[lag] = 2 * r[0] - head - tail;
		head += t->x[lag] * t->x[lag];
		tail += t->x[PW_LEN - 1 - lag] * t->x[PW_LEN - 1 - lag];
	}
	// r(0) < 2^28.4 here; bring it under 2^23 so up to 15 terms of d stay below 2^28.5
	int sh2 = (31 - dstar_mbe_clz((uint32_t)(r[0] > 0 ? r[0] : 1))) - 22;
	if (sh2 > 0) {
		for (int i = 0; i < PW_LEN; ++i) {
			r[i] >>= sh2;
			dd[i] >>= sh2;
		}
	}
	if (r[0] <= 0) {
		for (int i = 0; i < DSTAR_MBE_NPITCH; ++i)
			E[i] = E_ONE;
		return;
	}
	for (int i = 0; i < DSTAR_MBE_NPITCH; ++i) {
		int32_t p2 = P2_MIN + i, num = 0, den = 0;
		for (int32_t kp = p2; kp <= 2 * (PW_LEN - 2); kp += p2) {
			// Half-integer kP takes the mean of the two neighboring lags
			int32_t lag = kp >> 1, rr, de;
			if (kp & 1) {
				rr = (r[lag] + r[lag + 1]) >> 1;
				de = (dd[lag] + dd[lag + 1]) >> 1;
			} else {
				rr = r[lag];
				de = dd[lag];
			}
			num += de - 2 * rr;
			den += de;
		}
		if (num < 0)
			num = 0;
		while (den >= (1 << 18)) {
			num >>= 1;
			den >>= 1;
		}
		int32_t e = den > 0 ? (num << 12) / den : E_ONE;
		E[i] = (uint16_t)(e > 2 * E_ONE ? 2 * E_ONE : e);
	}
}

// --- 3. tracking ---

static int lo_of(int i)
{
	int lo = (4 * (P2_MIN + i) + 4) / 5 - P2_MIN;
	return lo < 0 ? 0 : lo;
}

static int hi_of(int i)
{
	int hi = (6 * (P2_MIN + i)) / 5 - P2_MIN;
	return hi > DSTAR_MBE_NPITCH - 1 ? DSTAR_MBE_NPITCH - 1 : hi;
}

// out[i] = min in[j] over j in [0.8 P_i, 1.2 P_i]. Both bounds rise with i, so a monotone queue gives every window
// minimum in one pass
static void window_min(const int32_t *in, int32_t *out, int16_t *q)
{
	int head = 0, tail = 0, next = 0;
	for (int i = 0; i < DSTAR_MBE_NPITCH; ++i) {
		int lo = lo_of(i), hi = hi_of(i);
		for (; next <= hi; ++next) {
			while (tail > head && in[q[tail - 1]] >= in[next])
				--tail;
			q[tail++] = (int16_t)next;
		}
		while (q[head] < lo)
			++head;
		out[i] = in[q[head]];
	}
}

static int track(dstar_mbe_ana_t *s, dstar_mbe_ana_scratch_t *t)
{
	const uint16_t *E0 = s->E[s->ring], *E1 = s->E[(s->ring + 1) % 3], *E2 = s->E[(s->ring + 2) % 3];
	int32_t *g = t->g, *ce = t->ce;
	int16_t *q = t->x;	// free after pitch_error

	// Look-back: the best E0 within +-20 % of the last pitch
	int pi = s->prev_p2 - P2_MIN, ib = lo_of(pi);
	for (int i = ib, hi = hi_of(pi); i <= hi; ++i) {
		if (E0[i] < E0[ib])
			ib = i;
	}
	int32_t ceb = E0[ib] + s->prev_e1 + s->prev_e2;

	// Look-ahead: CE(P0) = E0(P0) + min_P1 [E1(P1) + min_P2 E2(P2)], each step within +-20 %
	for (int i = 0; i < DSTAR_MBE_NPITCH; ++i)
		ce[i] = E2[i];
	window_min(ce, g, q);
	for (int i = 0; i < DSTAR_MBE_NPITCH; ++i)
		g[i] += E1[i];
	window_min(g, ce, q);
	for (int i = 0; i < DSTAR_MBE_NPITCH; ++i)
		ce[i] += E0[i];
	int iF = 0;
	for (int i = 1; i < DSTAR_MBE_NPITCH; ++i) {
		if (ce[i] < ce[iF])
			iF = i;
	}

	// Pitch doubling check: prefer the shortest sub-multiple P_F / n whose cumulative error is nearly as good
	int p2F = P2_MIN + iF;
	for (int n = p2F / P2_MIN; n >= 2; --n) {
		int c = (p2F + n / 2) / n - P2_MIN, best = c;
		if (c < 0)
			continue;
		for (int j = c - 1; j <= c + 1; ++j) {
			if (j >= 0 && j < DSTAR_MBE_NPITCH && ce[j] < ce[best])
				best = j;
		}
		int32_t a = ce[best], f = ce[iF] > SUB_REL_FLOOR ? ce[iF] : SUB_REL_FLOOR;
		if ((a <= SUB_A_ABS && 10 * a <= SUB_A_REL * f) || (a <= SUB_B_ABS && 10 * a <= SUB_B_REL * f) ||
		    a <= SUB_C_ABS) {
			iF = best;
			break;
		}
	}

	int ic = (ceb <= CE_B_ACCEPT || ceb <= ce[iF]) ? ib : iF;
	s->prev_e2 = s->prev_e1;
	s->prev_e1 = E0[ic];
	s->prev_p2 = (uint16_t)(P2_MIN + ic);
	s->e_tracked = E0[ic];
	return P2_MIN + ic;
}

// --- 4. spectrum ---

// In-place radix-2 decimation-in-time FFT, 256 points, block floating point: a stage halves its outputs only when its
// inputs reach 2^14, so quiet frames keep their precision. Inputs below 2^15.3 keep every twiddle product within
// int32. Returns the number of halvings (output = DFT / 2^return)
static int fft256(int32_t *re, int32_t *im)
{
	for (unsigned i = 0; i < 256; ++i) {
		unsigned j = ((i & 0x0fu) << 4) | ((i & 0xf0u) >> 4);
		j = ((j & 0x33u) << 2) | ((j & 0xccu) >> 2);
		j = ((j & 0x55u) << 1) | ((j & 0xaau) >> 1);
		if (j > i) {
			int32_t a = re[i];
			re[i] = re[j];
			re[j] = a;
			a = im[i];
			im[i] = im[j];
			im[j] = a;
		}
	}
	int shifts = 0;
	int32_t mx = 0;
	for (int i = 0; i < 256; ++i) {
		int32_t a = re[i] < 0 ? -re[i] : re[i], b = im[i] < 0 ? -im[i] : im[i];
		if (a > mx)
			mx = a;
		if (b > mx)
			mx = b;
	}
	for (unsigned half = 1, tw = 128; half < 256; half <<= 1, tw >>= 1) {
		int sc = mx >= (1 << 14);
		shifts += sc;
		mx = 0;
		for (unsigned k = 0; k < half; ++k) {
			uint32_t ph = (k * tw) << 24;	// 2 pi k tw / 256
			int32_t wr = dstar_mbe_cos(ph), wi = -dstar_mbe_sin(ph);
			for (unsigned i = k; i < 256; i += 2 * half) {
				unsigned j = i + half;
				int32_t tr = (re[j] * wr - im[j] * wi) >> 15;
				int32_t ti = (re[j] * wi + im[j] * wr) >> 15;
				int32_t ar = re[i], ai = im[i];
				int32_t a0 = (ar + tr) >> sc, a1 = (ai + ti) >> sc;
				int32_t b0 = (ar - tr) >> sc, b1 = (ai - ti) >> sc;
				re[i] = a0;
				im[i] = a1;
				re[j] = b0;
				im[j] = b1;
				int32_t m0 = a0 < 0 ? -a0 : a0, m1 = a1 < 0 ? -a1 : a1;
				int32_t m2 = b0 < 0 ? -b0 : b0, m3 = b1 < 0 ? -b1 : b1;
				if (m0 > mx)
					mx = m0;
				if (m1 > mx)
					mx = m1;
				if (m2 > mx)
					mx = m2;
				if (m3 > mx)
					mx = m3;
			}
		}
	}
	return shifts;
}

// The windowed, zero-phase FFT of the analyzed frame and its power spectrum. Returns ex with
// pw[m] = |DFT{s w}(m)|^2 2^(2 ex - 4), or -99 for a silent frame
static int spectrum(const int16_t *buf, dstar_mbe_ana_scratch_t *t)
{
	int32_t *re = t->u.f.re, *im = t->u.f.im;
	int32_t m = 0;
	for (int k = -SW_HALF; k <= SW_HALF; ++k) {
		int32_t a = buf[CENTER + k];
		if (a < 0)
			a = -a;
		if (a > m)
			m = a;
	}
	if (m == 0)
		return -99;
	int b = 31 - dstar_mbe_clz((uint32_t)m);
	int sh_in = 13 - b;	// peak just under 2^14
	for (int i = 0; i < 256; ++i)
		re[i] = im[i] = 0;
	for (int k = -SW_HALF; k <= SW_HALF; ++k) {
		int32_t sv = buf[CENTER + k];
		sv = sh_in >= 0 ? sv * (1 << sh_in) : sv >> -sh_in;
		re[k & 255] = (sv * dstar_mbe_spec_w_q15[k < 0 ? -k : k]) >> 15;
	}
	int ex = sh_in - fft256(re, im);
	// Bring the half spectrum to just under 2^13 so |X|^2 >> 4 < 2^22 and a sum over all 129 bins stays below 2^30
	int32_t mx = 1;
	for (int i = 0; i <= 128; ++i) {
		int32_t a = re[i] < 0 ? -re[i] : re[i], c = im[i] < 0 ? -im[i] : im[i];
		if (a > mx)
			mx = a;
		if (c > mx)
			mx = c;
	}
	int post = 12 - (31 - dstar_mbe_clz((uint32_t)mx));
	for (int i = 0; i <= 128; ++i) {
		if (post >= 0) {
			re[i] *= 1 << post;
			im[i] *= 1 << post;
		} else {
			re[i] >>= -post;
			im[i] >>= -post;
		}
		t->pw[i] = (re[i] * re[i] + im[i] * im[i]) >> 4;
	}
	return ex + post;
}

// --- harmonic fit ---
// Harmonic l of a fundamental of c bins (Q16) covers [(l - 1/2) c, (l + 1/2) c). Over its bins, the best fit of
// A W(m - lc) to X(m) leaves
//     residual = sum |X|^2 - |sum X W|^2 / sum W^2
// (Griffin & Lim eq. 19-21); W is real because the window is zero-phase. Returns the explained part, in pw units, and
// the band energy in *en
static int32_t harmonic_fit(const dstar_mbe_ana_scratch_t *t, uint32_t c, unsigned l, int32_t *en)
{
	const int32_t *re = t->u.f.re, *im = t->u.f.im;
	uint32_t lc = l * c, a = lc - c / 2, b = lc + c / 2;
	int32_t sr = 0, si = 0, gw = 0, e = 0;
	for (uint32_t mq = (a + 65535u) & ~65535u; mq < b; mq += 65536u) {
		unsigned m = mq >> 16;
		if (m > 128)
			break;
		int32_t d = (int32_t)(mq - lc);
		int32_t w = dstar_mbe_win_ft_q12[((d < 0 ? -d : d) + 512) >> 10];
		sr += re[m] * w;
		si += im[m] * w;
		gw += (w * w) >> 12;
		e += t->pw[m];
	}
	*en = e;
	if (gw <= 0)
		return 0;
	int32_t x = sr >> 12, y = si >> 12;	// |x|, |y| < 2^15 below 2^13 X
	if (x > 32767)
		x = 32767;
	if (x < -32767)
		x = -32767;
	if (y > 32767)
		y = 32767;
	if (y < -32767)
		y = -32767;
	uint32_t qq = (uint32_t)(x * x) + (uint32_t)(y * y);
	// explained = qq 2^8 / gw (units: pw = |X|^2 / 16, W in Q12, gw Q12)
	int hs = dstar_mbe_clz(qq) - 1;
	int32_t ex = hs >= 8 ? (int32_t)((qq << 8) / (uint32_t)gw) : (int32_t)(((qq << hs) / (uint32_t)gw) << (8 - hs));
	return ex > e ? e : ex;
}

// The explained energy below lim bins for a fundamental of c bins (Q16)
static int32_t fit_total(const dstar_mbe_ana_scratch_t *t, uint32_t c, unsigned lim)
{
	int32_t sum = 0, en;
	for (unsigned l = 1; (l * c + c / 2) >> 16 < lim; ++l)
		sum += harmonic_fit(t, c, l, &en);
	return sum;
}

// Refines a half-sample pitch to an eighth of a sample: +-1.125 samples in quarter steps, then +-1/8 around the best.
// Returns P in eighth samples
static uint32_t refine(const dstar_mbe_ana_scratch_t *t, int p2)
{
	uint32_t best = (uint32_t)(4 * p2);
	int32_t bv = -1;
	for (int k = -9; k <= 9; k += 2) {
		uint32_t p8 = (uint32_t)(4 * p2 + k);
		if (p8 < 8 * 19 || p8 > 8 * 128)
			continue;
		int32_t v = fit_total(t, (1u << 27) / p8, REFINE_BINS);
		if (v > bv) {
			bv = v;
			best = p8;
		}
	}
	uint32_t c0 = best;
	for (int k = -1; k <= 1; k += 2) {
		uint32_t p8 = c0 + (uint32_t)k;
		int32_t v = fit_total(t, (1u << 27) / p8, REFINE_BINS);
		if (v > bv) {
			bv = v;
			best = p8;
		}
	}
	return best;
}

// The nearest D-STAR grid index in log frequency (f0 falls as b0 rises)
static unsigned snap_b0(uint32_t f0)
{
	unsigned lo = 0, hi = 125;
	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		if (dstar_mbe_f0_mid_q24[mid] > f0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

// The energy of [a, b) bins (Q16) with the edge bins weighted by overlap; bin m covers [m - 1/2, m + 1/2)
static int32_t band_energy(const int32_t *pw, uint32_t a, uint32_t b)
{
	int32_t sum = 0;
	uint32_t m0 = (a + 32768u) >> 16, m1 = (b + 32768u) >> 16;
	for (uint32_t m = m0; m <= m1 && m <= 128; ++m) {
		uint32_t lo = m * 65536u - 32768u, hi = lo + 65536u;
		if (m == 0)
			lo = 0;
		uint32_t x0 = a > lo ? a : lo, x1 = b < hi ? b : hi;
		if (x1 <= x0)
			continue;
		uint32_t wq8 = (x1 - x0) >> 8;	// overlap, Q8 of a bin
		sum += (int32_t)(((uint32_t)pw[m] * wq8) >> 8);
	}
	return sum;
}

static void dc_block(dstar_mbe_ana_t *s, const int16_t *pcm, int16_t *dst)
{
	// y = x - x1 + (1 - 1/64) y1: a 20 Hz high-pass, y kept in Q8
	int32_t x1 = s->hp_x1, y = s->hp_y1;
	for (int n = 0; n < DSTAR_MBE_FRAME; ++n) {
		int32_t x = pcm[n];
		y += (x - x1) * 256 - (y >> 6);
		x1 = x;
		int32_t o = (y + 128) >> 8;
		dst[n] = (int16_t)(o > 32767 ? 32767 : (o < -32768 ? -32768 : o));
	}
	s->hp_x1 = x1;
	s->hp_y1 = y;
}

// Matching a DVSI encoder (18 Nov 2026, on-air evidence; docs/spec/dstar-codec.md 2).
//
// A real DVSI decoder (an Icom ID-51) played our frames audibly but unintelligibly although every bit was correctly
// coded, while it plays an IC-9700's DVSI frames fine. Decoded with mbelib's tables, DVSI frames have a much flatter
// spectrum than real speech through this analyzer: voiced harmonics at 1-3 kHz about 15 dB under the strongest,
// against 30-40 dB here. Compressing each frame's log magnitudes about their mean, with the frame's power kept, made
// the ID-51 intelligible: on real speech 50 % rated 4/5, 55 % a little better than none, 60 % 5/5, and the operator's
// own voice at 60 % "wasn't bad"; without the power kept it was intelligible but quiet. So DVSI's analysis is flatter
// than ours, and its decoders, which put the depth back, over-deepened ours. dstar_mbe_synth.c puts depth back (1/0.73).
//
// DVSI_SHAPE_Q12 is the compression per 500 Hz band (the voicing bands), 0.6 in all: the ground-truth comparison of
// 18 Nov (the same audio through an IC-9700 and through us, frame by frame over 21,000 harmonics) found the compression
// the same in every band.
//
// DVSI_EQ_Q12 is DVSI's spectral balance on top of it, log2 Q12 per 500 Hz band, added before the power is kept. The
// 18 Nov comparison measured it (-4.5 +2.2 +3.2 +2.9 +1.8 +3.0 -1.0 -8.0 dB) but it was left off: one radio couldn't
// tell DVSI's analysis (which DVSI decoders would undo) from the 9700's audio path (which they would not). On since
// 21 Nov (the research's LOUDNESS-21NOV.md), with the table re-measured that day from the same audio through this
// encoder and the 9700's, each normalised to its own full-band level: -5.2 +0.3 +1.8 +2.1 +1.4 +1.6 -1.7 -4.9 dB. Icom
// handhelds played us quietly because our level sat below 500 Hz, which a handheld's speaker does not reproduce; the
// 9700's frames carry 5 dB less there and 1.5-2 dB more at 1-3 kHz. The two measurements agree in shape; the later is
// the gentler and was taken against the encoder as it now is. With the power kept, the cut below 500 Hz moves level
// into the speech band, and it takes bass peaks out of the decoder's headroom now the leveler aims 6 dB higher
// (dstar_mbe_level.c). The cost, on our own round trip, where no decoder undoes it (the research's quality run, 12
// inputs, leveler off): STOI 0.866 -> 0.810, MCD 20.4 -> 37.3 dB; the 18 Nov table 0.798, the 500 Hz cut alone 0.820.
// An ID-51 and one of our own radios hearing the same over decide whether to keep all of it; the cut alone
// ({ -3538, 0, ... }) is the fallback. The research codec carries the same table, so the frames stay its bit for bit.
//
// Pitch conventions seen in every IC-9700 frame (347 of speech, 490 of whistle and hum): all-unvoiced frames always at
// b0 34, voiced frames never above b0 113. On their own they did not make our frames intelligible, but they cost
// nothing and keep us inside what DVSI decoders are known to receive
#define DVSI_B0_UNVOICED	34
#define DVSI_B0_MAX		119
static const int16_t dvsi_shape_q12[8] = { 2458, 2458, 2458, 2458, 2458, 2458, 2458, 2458 };
static const int16_t dvsi_eq_q12[8] = { -3538, 204, 1225, 1429, 953, 1089, -1157, -3334 };

// y: L + 1 words of working memory
static void dvsi_shape(dstar_ambe_params_t *p, int32_t *y, int flat)
{
	unsigned L = p->L;
	if (L == 0)
		return;
	int32_t sum = 0, mx = p->log2_amp[1], mx1 = INT32_MIN;
	for (unsigned l = 1; l <= L; ++l) {
		sum += p->log2_amp[l];
		if (p->log2_amp[l] > mx)
			mx = p->log2_amp[l];
	}
	int32_t m = sum / (int32_t)L;
	for (unsigned l = 1; l <= L; ++l) {
		unsigned k = (l * p->f0_q24) >> 20;
		k = k > 7 ? 7 : k;
		y[l] = m + (((p->log2_amp[l] - m) >> 4) * dvsi_shape_q12[k] >> 8) + (flat ? 0 : dvsi_eq_q12[k]);
		if (y[l] > mx1)
			mx1 = y[l];
	}
	// power kept: half the log2 ratio of sum 2^(2x) before and after, each sum taken relative to its own maximum
	uint32_t p0 = 0, p1 = 0;
	for (unsigned l = 1; l <= L; ++l) {
		p0 += dstar_mbe_pow2_q12(2 * (p->log2_amp[l] - mx) + (12 << 12));
		p1 += dstar_mbe_pow2_q12(2 * (y[l] - mx1) + (12 << 12));
	}
	int32_t g = (dstar_mbe_log2_q12(p0 ? p0 : 1) - dstar_mbe_log2_q12(p1 ? p1 : 1)) / 2 + mx - mx1;
	for (unsigned l = 1; l <= L; ++l)
		p->log2_amp[l] = y[l] + g;
}

void dstar_mbe_analyze(dstar_mbe_ana_t *s, dstar_mbe_ana_scratch_t *t, const int16_t pcm[DSTAR_MBE_FRAME],
		       dstar_ambe_params_t *out)
{
	// 1. history
	for (int i = 0; i < DSTAR_MBE_ANA_BUF - DSTAR_MBE_FRAME; ++i)
		s->buf[i] = s->buf[i + DSTAR_MBE_FRAME];
	dc_block(s, pcm, s->buf + DSTAR_MBE_ANA_BUF - DSTAR_MBE_FRAME);

	// 2. E of the newest frame; the ring advances so E[ring] is the frame analyzed
	s->ring = (uint8_t)((s->ring + 1) % 3);
	pitch_error(s->buf + PW_CENTER - PW_HALF - LPF_HALF, t, s->E[(s->ring + 2) % 3]);

	// 3. track
	int p2 = track(s, t);

	// 4. spectrum and refinement
	int ex = spectrum(s->buf, t);
	uint32_t p8 = ex == -99 ? (uint32_t)(4 * p2) : refine(t, p2);
	s->p8 = p8;
	uint32_t c_ref = (1u << 27) / p8;	// bins per harmonic, Q16
	uint32_t f0 = (1u << 27) / p8;		// cycles per sample Q24 = bins Q16

	// 6a. snap to the D-STAR grid: the magnitudes are measured at the harmonics the decoder will synthesize, and the
	// quantizer then finds the same b0
	const int dvsi = !s->full_depth;
	unsigned b0 = snap_b0(f0);
	if (dvsi && b0 > DVSI_B0_MAX)
		b0 = DVSI_B0_MAX;
	f0 = dstar_mbe_f0_q24[b0];
	unsigned L = dstar_mbe_L[b0];
	out->f0_q24 = f0;
	out->L = (uint8_t)L;

	if (ex == -99) {	// silence
		for (unsigned l = 1; l <= L; ++l) {
			out->voiced[l] = 0;
			out->log2_amp[l] = LOG_FLOOR / 2;
		}
		for (int k = 0; k < 8; ++k)
			s->prev_v[k] = 0;
		return;
	}

	// 5. voicing: the normalized residual of the fit at the refined pitch, gathered into the decoder's 500 Hz bands
	// (the band of harmonic l is floor(16 l f0))
	int32_t res[8] = { 0 }, tot[8] = { 0 };
	for (unsigned l = 1; (l * c_ref + c_ref / 2) >> 16 <= 128; ++l) {
		int32_t en, ex_l = harmonic_fit(t, c_ref, l, &en);
		unsigned k = (l * c_ref) >> 20;
		if (k > 7)
			break;
		res[k] += (en - ex_l) >> 4;
		tot[k] += en >> 4;
	}
	// Real voiced speech fits worse than a steady tone: the pitch moves during the 27 ms window and smears the upper
	// harmonics, by l times the drift. So each 1 kHz pair keeps the smallest residual over the refined pitch and four
	// neighbors 0.5 % apart. On white noise this extra freedom moves the residual very little (the noise and mix tests
	// still pass with the same thresholds); on the test speech it lowered MCD by 0.5 to 0.7 dB through more upper-band
	// voicing in vowels
	for (int j = -PAIR_STEPS; j <= PAIR_STEPS; ++j) {
		if (j == 0)
			continue;
		uint32_t cj = (uint32_t)((int32_t)c_ref + (((int32_t)c_ref >> 4) * PAIR_EPS_Q12 * j >> 8));
		int32_t r2[8] = { 0 };
		for (unsigned l = 1; (l * cj + cj / 2) >> 16 <= 128; ++l) {
			int32_t en, ex_l = harmonic_fit(t, cj, l, &en);
			unsigned k = (l * cj) >> 20;
			if (k > 7)
				break;
			r2[k] += (en - ex_l) >> 4;
		}
		for (int g = 0; g < 4; ++g) {
			if (r2[2 * g] + r2[2 * g + 1] < res[2 * g] + res[2 * g + 1]) {
				res[2 * g] = r2[2 * g];
				res[2 * g + 1] = r2[2 * g + 1];
			}
		}
	}
	// Decided per 1 kHz pair of bands: the D-STAR voicing field (b1) has one bit a pair, and pooling the pair halves
	// the variance of the noise residual, which is what limits the separation (its spread is about 0.23 of D_noise
	// per 500 Hz band at a 140 Hz pitch)
	uint8_t v[8];
	// The noise baseline at the refined pitch, per pair: rows every 16 half samples, linear in between
	int32_t pos = (int32_t)p8 / 4 - P2_MIN, row = pos >> 4, fr16 = pos & 15;
	if (row < 0) {
		row = 0;
		fr16 = 0;
	}
	if (row >= NOISE_ROWS - 1) {
		row = NOISE_ROWS - 2;
		fr16 = 16;
	}
	int32_t ge = s->e_tracked <= E_LO ? 4096 : s->e_tracked >= E_HI ? 0
		   : ((E_HI - s->e_tracked) << 12) / (E_HI - E_LO);
	for (int g = 0; g < 4; ++g) {
		int k = 2 * g;
		int32_t al = alpha_q12[g];
		al += s->prev_v[k] ? ALPHA_HYST : 0;
		al = (al * ge) >> 12;
		// Voiced speech falls with frequency (the glottal source about -12 dB an octave); an upper pair more than
		// 6 dB louder than 0..1 kHz is almost always frication. Its noise is also strongly colored, which makes the
		// residual statistic noisier than the white-noise baseline assumes, so the threshold is halved there. On the
		// test speech this cut voicing above 2 kHz in fricative frames from 42 % to 8 % for 0.0 to 0.15 dB of MCD
		if (g > 0 && tot[k] + tot[k + 1] > 4 * (tot[0] + tot[1]))
			al >>= 1;
		int32_t base = noise_base_q12[row][g] +
			       (((noise_base_q12[row + 1][g] - noise_base_q12[row][g]) * fr16) >> 4);
		int32_t th = (al * base) >> 12;
		int32_t rk = res[k] + res[k + 1], tk = tot[k] + tot[k + 1];
		if (tk <= 0) {
			v[k] = v[k + 1] = g > 0 ? v[k - 1] : 0;
			continue;
		}
		while (tk >= (1 << 19)) {	// rk <= tk: rk << 12 fits
			rk >>= 1;
			tk >>= 1;
		}
		int32_t d = (rk << 12) / tk;
		v[k] = v[k + 1] = d < th;
	}
	for (int k = 0; k < 8; ++k)
		s->prev_v[k] = v[k];

	// DVSI sends every all-unvoiced frame at one pitch index: measure such a frame on that grid. Voicing above was
	// decided per 500 Hz band, so it does not depend on the grid
	if (dvsi && !(v[0] | v[1] | v[2] | v[3] | v[4] | v[5] | v[6] | v[7])) {
		f0 = dstar_mbe_f0_q24[DVSI_B0_UNVOICED];
		L = dstar_mbe_L[DVSI_B0_UNVOICED];
		out->f0_q24 = f0;
		out->L = (uint8_t)L;
	}

	// 6b. magnitudes from band energy on the output grid. Band power, PCM^2: P = 2 E_true / (256 sum w^2), where
	// E_true = sum |DFT{s w}|^2 over the band = band_energy 2^(4 - 2 ex): a sinusoid of amplitude a puts
	// a^2 / 4 x 256 sum w^2 into its band (Parseval), so P = a^2 / 2, and noise of power P in the band gives the same
	uint32_t c = f0;	// output grid, bins Q16
	int32_t lw0 = LOG2_2PI + dstar_mbe_log2_q12(f0) - (24 << 12);
	int32_t base = (4 - 2 * ex) * 4096 + 4096 - DSTAR_MBE_SW_LOG2_NW2_Q12;
	for (unsigned l = 1; l <= L; ++l) {
		unsigned k = (l * f0) >> 20;
		out->voiced[l] = v[k > 7 ? 7 : k];
		// Band 1 starts at 0 Hz: the input is DC-blocked, and the lower skirt of the first harmonic's window response
		// (half-width 2.3 bins) would otherwise belong to no band; at 70 Hz that lost 1 dB
		int32_t e = band_energy(t->pw, l == 1 ? 0 : l * c - c / 2, l * c + c / 2);
		int32_t lp = e > 0 ? dstar_mbe_log2_q12((uint32_t)e) + base : LOG_FLOOR;
		if (lp < LOG_FLOOR)
			lp = LOG_FLOOR;
		int32_t la = out->voiced[l] ? (lp + 4096) / 2 - LOG2_7
					    : lp / 2 + lw0 / 2 + LOG2_INV_UNVC - LOG2_KUV - (lw0 - UV_LW0_REF) + UV_GAIN;
		out->log2_amp[l] = la + ((la * 55) >> 18);	// mbelib's exp(0.693 x)
	}
	if (dvsi)
		dvsi_shape(out, t->g, s->flat);	// t->g (the tracker's) is free by now
}
