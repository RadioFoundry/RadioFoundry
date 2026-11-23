// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MBE core's parameter estimator (mbe_est.h), in integers.
//
// A frame: the input is high-passed (a DC blocker) and joins the last 61 samples under a 221-sample Hamming window,
// centered at index 0 of a 512-point real FFT so that a steady sinusoid's spectrum is the window's transform W, real,
// at the sinusoid's frequency. The spectrum is kept as 13-bit pairs with its power beside it.
//
// The pitch: a normalized autocorrelation of the signal low-passed to about 1 kHz and taken at 4 kHz, over the last
// 128 of those samples, for periods of 18 to 124 samples. The shortest period within a little of the best one wins (a
// multiple of the true period scores as well as the period itself). The front end then tries its own pitch values
// near it against the spectrum (mbe_est_mismatch).
//
// A harmonic at bin c owns the bins half a harmonic spacing to each side. Its energy E is their power; the part a
// sinusoid at c explains is |sum S W|^2 / sum W^2 over those bins. A band is voiced when its harmonics leave less
// than a threshold of their energy unexplained. A voiced harmonic's amplitude is that of the sinusoid with energy E
// in the bins, an unvoiced one's the level the synthesis turns into noise of the band's power.
#include <string.h>
#include "mbe_est.h"
#include "mbe_dsp.h"
#include "../dsp/fxp.h"

typedef int16_t __attribute__((may_alias)) i16a;
typedef uint32_t __attribute__((may_alias)) u32a;

#define HALF		110			// the window is samples -HALF..HALF around its center
#define FFT_LEN		(2 * MBE_FFT_N)		// 512
#define BINS		(MBE_FFT_N - 1)		// bins 1..255 are kept
#define COMB_AT		120			// scratch words while the window is filled: the frame's samples ...
#define NL_AT		250			// ... and the pitch search's copy (both in the part of the FFT input that is zero)
#define LP_TAPS		15
#define LP_NEW		(MBE_FRAME_LEN / 2)
#define AC_LEN		128			// samples the autocorrelation sums over
#define LAG_MIN		9
#define LAG_MAX		62
#define W_STEPS		76			// table entries: 1/16 bin, to the first null
#define PITCH_NEAR	311			// log2 Q11 (a factor 0.9): a shorter period this close to the best one wins
#define PITCH_KEEP	660			// log2 Q11 (a factor 0.8): a period near the last frame's this close to the best one wins

// constants of the amplitude scale, log2 Q11 (sum w = 118.88, sum w^2 = 87.4344 for the window)
#define C_VOICED	(-12070)		// 1 - log2(sum w): a peak of the spectrum to the sinusoid's amplitude
#define C_UNVOICED	(-16289)		// -(log2(256 sum w^2) / 2 + log2 1.657): bin power to the unvoiced level
// voicing thresholds, Q15 (see band_threshold)
#define TH_SCALE	19661			// 0.6 of what noise would leave unexplained
#define TH_TILT		9830			// less 0.3 of that at the top band
#define TH_KEEP		8192			// a band voiced in the last frame gets a quarter more
#define W_NOISE_BINS	207749			// 3.17 bins, Q16: the bins of noise one fitted sinusoid explains

// the window's transform at 1/16 bin steps of the 512-point FFT, Q15, W(0) = 1:
// W(f) = sum_k w(k) cos(2 pi f k / 512) / sum w, w(k) = 0.54 + 0.46 cos(2 pi k / 220), k = -110..110
static const int16_t w_tab[W_STEPS] = {
	32767, 32748, 32692, 32598, 32467, 32300, 32096, 31856, 31582, 31273, 30931, 30556, 30150, 29714, 29250, 28757,
	28238, 27694, 27127, 26538, 25928, 25300, 24655, 23995, 23321, 22635, 21938, 21234, 20522, 19806, 19086, 18364,
	17643, 16922, 16206, 15493, 14787, 14089, 13399, 12720, 12052, 11397, 10756, 10129, 9518,  8924,  8347,  7789,
	7249,  6728,  6227,  5746,  5286,  4846,  4428,  4030,  3653,  3296,  2961,  2645,  2350,  2075,  1819,  1582,
	1363,  1162,  979,   812,   661,   526,   405,   298,   203,   122,   51,    -8,
};
// the pitch search's low-pass: 15 taps, 1.25 kHz at 8 kHz (-32 dB from 2 kHz), Q15
static const int16_t lp_tab[LP_TAPS] = { 66, -83, -515, -803, 433, 3962, 8232, 10184, 8232, 3962, 433, -803, -515, -83, 66 };

static int bits_of(uint32_t x) { return x ? 32 - __builtin_clz(x) : 0; }

void mbe_est_init(mbe_est_state_t *s) { memset(s, 0, sizeof *s); }

// The pitch period from s->lp, quarter samples, and how periodic the signal is there. nl: scratch for MBE_EST_LP
static void pitch(const mbe_est_state_t *s, i16a *nl, mbe_est_t *e)
{
	int16_t sc[LAG_MAX - LAG_MIN + 1];
	const i16a *x = nl + (MBE_EST_LP - AC_LEN);
	int32_t m = 0, e0, et, best_peak = -32768;
	int sh, pick = -1, p4;

	for (int i = 0; i < MBE_EST_LP; i++) {
		int32_t v = s->lp[i] < 0 ? -s->lp[i] : s->lp[i];

		if (v > m)
			m = v;
	}
	e->period4 = 0;
	e->periodic = -32768;
	if (!m)
		return;
	sh = bits_of((uint32_t)m) - 11;			// to 11 bits: 128 products of two fit 32 bits
	for (int i = 0; i < MBE_EST_LP; i++)
		nl[i] = (int16_t)(sh > 0 ? s->lp[i] >> sh : s->lp[i] * (1 << -sh));	// a multiply: no left shift of a negative
	e0 = fxp_dot16(x, x, AC_LEN);
	et = fxp_dot16(x - LAG_MIN, x - LAG_MIN, AC_LEN);
	for (int lag = LAG_MIN; lag <= LAG_MAX; lag++) {
		int32_t num = fxp_dot16(x, x - lag, AC_LEN), v = -32768;

		if (num > 0 && e0 > 0 && et > 0)
			v = fxp_clamp(mbe_log2_q11((uint32_t)num) - ((mbe_log2_q11((uint32_t)e0) + mbe_log2_q11((uint32_t)et)) >> 1),
				      -32768, 0);
		sc[lag - LAG_MIN] = (int16_t)v;
		// the window one sample further back
		et += x[-lag - 1] * x[-lag - 1] - x[AC_LEN - 1 - lag] * x[AC_LEN - 1 - lag];
	}
	// a peak has a lower score on both sides: the first and last lags are only there to give the next ones a neighbor
	// (a low-passed signal is always alike at the shortest lags, peak or not)
	for (int i = 1; i < LAG_MAX - LAG_MIN; i++)
		if (sc[i] > best_peak && sc[i] >= sc[i - 1] && sc[i] >= sc[i + 1])
			best_peak = sc[i];
	// the last frame's pitch holds when a peak within a fifth of it is nearly as good as the best: a frame at the
	// edge of a sound often scores a half or a double period a little higher
	if (s->period4) {
		int32_t near = -32768;

		for (int i = 1; i < LAG_MAX - LAG_MIN; i++) {
			int p8 = 8 * (i + LAG_MIN);

			if (5 * p8 < 4 * s->period4 || 4 * p8 > 5 * s->period4)
				continue;
			if (sc[i] > near && sc[i] >= sc[i - 1] && sc[i] >= sc[i + 1]) {
				near = sc[i];
				pick = i;
			}
		}
		if (near < best_peak - PITCH_KEEP)
			pick = -1;
	}
	for (int i = 1; pick < 0 && i < LAG_MAX - LAG_MIN; i++)
		if (sc[i] >= best_peak - PITCH_NEAR && sc[i] >= sc[i - 1] && sc[i] >= sc[i + 1])
			pick = i;
	if (pick < 0 || best_peak <= -32768)
		return;
	p4 = 8 * (pick + LAG_MIN);
	{	// the peak between the samples, by a parabola through three
		int32_t sm = sc[pick - 1], s0 = sc[pick], sp = sc[pick + 1], den = sm - 2 * s0 + sp;

		if (den < 0 && sm > -32768 && sp > -32768)
			p4 += (4 * (sm - sp)) / den;
	}
	e->period4 = (int16_t)p4;
	e->periodic = sc[pick];
}

void mbe_est_frame(mbe_est_state_t *s, const int16_t pcm[MBE_FRAME_LEN], int32_t *buf, mbe_est_t *e)
{
	i16a *comb = (i16a *)(buf + COMB_AT), *sp = (i16a *)buf;
	u32a *pw = (u32a *)(buf + BINS);
	int32_t y = s->hp_y, m = 0;
	int x1 = s->hp_x, sh, s2;
	uint32_t ph = 0;

	// high-pass: y[n] = x[n] - x[n-1] + 0.99 y[n-1]. What is taken off y is rounded away from zero, so that y runs
	// out to zero after the input does (rounded down, a small positive y stays for good: a DC level of a few units)
	memcpy(comb, s->hist, sizeof s->hist);
	for (int i = 0; i < MBE_FRAME_LEN; i++) {
		y = (pcm[i] - x1) * 16 + y - ((y * 328 + (y > 0 ? 32767 : 0)) >> 15);
		x1 = pcm[i];
		comb[MBE_EST_HIST + i] = fxp_sat16(y >> 4);
	}
	s->hp_y = y;
	s->hp_x = (int16_t)x1;
	memcpy(s->hist, comb + MBE_FRAME_LEN, sizeof s->hist);

	// the pitch search's signal: low-pass, every second sample
	memmove(s->lp, s->lp + LP_NEW, (MBE_EST_LP - LP_NEW) * sizeof s->lp[0]);
	for (int i = 0; i < LP_NEW; i++) {
		const i16a *x = comb + MBE_EST_HIST + 2 * i + 1 - (LP_TAPS - 1);
		int32_t acc = 0;

		for (int t = 0; t < LP_TAPS; t++)
			acc += x[t] * lp_tab[t];
		s->lp[MBE_EST_LP - LP_NEW + i] = fxp_sat16(acc >> 15);
	}
	pitch(s, (i16a *)(buf + NL_AT), e);
	e->exp = 0;
	e->zero = 0;

	// the window, the samples scaled up to 14 bits first
	for (int i = 0; i < MBE_EST_WIN; i++) {
		int32_t v = comb[i] < 0 ? -comb[i] : comb[i];

		if (v > m)
			m = v;
	}
	if (!m) {
		memset(buf, 0, MBE_BUF_WORDS * sizeof buf[0]);
		e->period4 = 0;
		e->zero = 1;
		return;
	}
	sh = 14 - bits_of((uint32_t)m);
	for (int k = 0; k <= HALF; k++, ph += 19522579u) {	// 2^32 / 220
		int32_t w = 17695 + ((15073 * mbe_cos_i(ph)) >> 15);
		int32_t a = comb[HALF + k], b = comb[HALF - k];

		a = sh >= 0 ? a * (1 << sh) : a >> -sh;	// a multiply: a left shift of a negative value is undefined
		b = sh >= 0 ? b * (1 << sh) : b >> -sh;
		buf[k] = (a * w) >> 15;
		if (k)
			buf[FFT_LEN - k] = (b * w) >> 15;
	}
	memset(buf + HALF + 1, 0, (FFT_LEN - 2 * HALF - 1) * sizeof buf[0]);
	mbe_rfft(buf);

	// bins 1..255 as 13-bit pairs at the front, their power after them
	m = 0;
	for (int i = 2; i < FFT_LEN; i++) {
		int32_t v = buf[i] < 0 ? -buf[i] : buf[i];

		if (v > m)
			m = v;
	}
	s2 = bits_of((uint32_t)m) - 13;
	if (s2 < 0)
		s2 = 0;
	for (int k = 1; k <= BINS; k++) {
		int32_t re = buf[2 * k], im = buf[2 * k + 1];

		if (s2) {
			re = (re + (1 << (s2 - 1))) >> s2;
			im = (im + (1 << (s2 - 1))) >> s2;
		}
		sp[2 * (k - 1)] = (int16_t)fxp_clamp(re, -8191, 8191);
		sp[2 * (k - 1) + 1] = (int16_t)fxp_clamp(im, -8191, 8191);
	}
	for (int k = BINS; k >= 1; k--)			// from the top: the powers end where the pairs begin
		pw[k - 1] = (uint32_t)(sp[2 * (k - 1)] * sp[2 * (k - 1)] + sp[2 * (k - 1) + 1] * sp[2 * (k - 1) + 1]);
	e->exp = (int16_t)(sh - s2);
}

typedef struct {
	uint32_t E;		// the power of the harmonic's bins
	uint32_t fit;		// the part a sinusoid at the harmonic explains (never more than E)
	uint32_t ww;		// sum W^2 over the bins, Q15
} harm_t;

// harmonic l of a fundamental at c1 bins (Q16)
static void harmonic(const int32_t *buf, uint32_t c1, int l, harm_t *h)
{
	const i16a *sp = (const i16a *)buf;
	const u32a *pw = (const u32a *)(buf + BINS);
	uint32_t cl = (uint32_t)l * c1, half = c1 >> 1;
	int lo = (int)((cl - half + 0xFFFFu) >> 16), hi = (int)((cl + half + 0xFFFFu) >> 16);
	int32_t xr = 0, xi = 0;
	uint32_t E = 0, ww = 0, x2;

	if (lo < 1)
		lo = 1;
	if (hi > BINS + 1)
		hi = BINS + 1;
	for (int m = lo; m < hi; m++) {
		int32_t d = (int32_t)((uint32_t)m << 16) - (int32_t)cl;
		int idx;

		E += pw[m - 1];
		if (d < 0)
			d = -d;
		idx = (d + 2048) >> 12;
		if (idx < W_STEPS) {
			int32_t w = w_tab[idx];

			xr += (sp[2 * (m - 1)] * w) >> 4;
			xi += (sp[2 * (m - 1) + 1] * w) >> 4;
			ww += (uint32_t)(w * w) >> 15;
		}
	}
	h->E = E;
	h->ww = ww;
	h->fit = 0;
	if (!ww || !E)
		return;
	xr = (xr + 1024) >> 11;
	xi = (xi + 1024) >> 11;
	x2 = (uint32_t)(xr * xr + xi * xi);
	// x2 / (ww / 2^15), with x2 scaled up as far as it goes first
	{
		int up = __builtin_clz(x2 | 1u) - 1, down;
		uint32_t q;

		if (up < 0)
			up = 0;
		q = (x2 << up) / ((ww >> 3) | 1u);		// x2 2^up / (WW 2^12)
		down = up - 12;
		q = down >= 0 ? q >> down : q << -down;
		h->fit = q > E ? E : q;
	}
}

int mbe_est_mismatch(const int32_t *buf, uint32_t w0)
{
	uint32_t c1 = w0 >> 7, se = 0, sr = 0;	// bins of the 512-point FFT, Q16

	if (c1 < (2u << 16))
		return 32768;
	for (int l = 1; l <= MBE_MAXL && (uint32_t)l * c1 < (205u << 16); l++) {
		harm_t h;

		harmonic(buf, c1, l, &h);
		se += h.E >> 6;
		sr += (h.E - h.fit) >> 6;
	}
	while (se >= 65536u) {
		se >>= 1;
		sr >>= 1;
	}
	return se ? (int)((sr << 15) / se) : 32768;
}

// What a band must leave unexplained less of to be voiced, Q15. Noise alone leaves 1 - 3.17 / (bins per harmonic)
// unexplained (one fitted sinusoid always explains about 3.17 bins of it), so the threshold is a share of that: less
// toward the top bands, where speech is less regular, and more for a band that was voiced in the last frame
static int32_t band_threshold(uint32_t c1, int k, int K, int was)
{
	int32_t noise = 32768 - (int32_t)(((uint32_t)W_NOISE_BINS << 13) / ((c1 >> 2) | 1u));
	int32_t th;

	if (noise < 0)
		noise = 0;
	th = (noise * TH_SCALE) >> 15;
	th -= (((th * TH_TILT) >> 15) * k) / (K > 1 ? K - 1 : 1);
	if (was)
		th += (th * TH_KEEP) >> 15;
	return th;
}

void mbe_est_params(mbe_est_state_t *s, const int32_t *buf, const mbe_est_t *e, mbe_params_t *p, int per, int K)
{
	uint32_t c1 = p->w0 >> 7;
	int16_t du[MBE_MAXL];			// each harmonic's unvoiced level less its voiced one
	uint16_t voiced = 0;
	int L = p->L, l = 1;
	int32_t ex = (int32_t)e->exp * 2048;

	for (int k = 0; k < K; k++) {
		int last = k == K - 1 ? L : (k + 1) * per, v;
		uint32_t se = 0, sr = 0;

		for (int i = l; i <= last; i++) {
			harm_t h;
			int32_t lge, av, au;

			harmonic(buf, c1, i, &h);
			se += h.E >> 5;
			sr += (h.E - h.fit) >> 5;
			if (!h.E || !h.ww) {
				p->a[i - 1] = MBE_A_SILENT;
				du[i - 1] = 0;
				continue;
			}
			lge = mbe_log2_q11(h.E);
			av = ((lge - (mbe_log2_q11(h.ww) - 15 * 2048)) >> 1) + C_VOICED - ex;
			au = (lge >> 1) + C_UNVOICED - ex;
			p->a[i - 1] = (int16_t)fxp_clamp(av, -32767, 32767);
			du[i - 1] = (int16_t)fxp_clamp(au - av, -32767, 32767);
		}
		while (se >= 65536u) {
			se >>= 1;
			sr >>= 1;
		}
		v = se && e->period4 && (int32_t)((sr << 15) / se) < band_threshold(c1, k, K, (s->vprev >> k) & 1);
		if (v)
			voiced |= (uint16_t)(1u << k);
		for (int i = l; i <= last; i++) {
			p->v[i - 1] = (uint8_t)v;
			if (!v && p->a[i - 1] != MBE_A_SILENT)
				p->a[i - 1] = (int16_t)fxp_clamp(p->a[i - 1] + du[i - 1], -32767, 32767);
		}
		l = last + 1;
	}
	s->vprev = voiced;
	s->period4 = e->period4;
}
