// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MBE synthesis (mbe.h): concealment, spectral amplitude enhancement, harmonic oscillators and band-shaped noise, in
// fixed point. Everything the core keeps or uses as scratch is placed by the caller
// (mbe_mem_t); 32x32 -> 64 bit products go through core/dsp/fxp.h. Reference: TIA-102.BABA.
#include <string.h>
#include "mbe.h"
#include "mbe_dsp.h"
#include "../dsp/fxp.h"

#define N MBE_FRAME_LEN
#define SEG 40
_Static_assert(MBE_BUF_WORDS == 2 * MBE_FFT_N, "the noise FFT works in place on 2 * MBE_FFT_N words");
_Static_assert(N % SEG == 0, "segments");

// gains, log2 in Q11
#define G_NOISE		1751		// level of an unvoiced band's noise against the harmonic amplitude it stands for
// spectral amplitude enhancement: weights from the envelope's autocorrelation (TIA-102.BABA), limited to 0.5 .. 1.2
#define ENH_LO		(-2048)
#define ENH_HI		539
#define ENH_EXP		512		// the weight's exponent on the envelope term, 0.25 in Q11
#define ENH_SKIP	8		// harmonics l with ENH_SKIP * l <= L keep weight 1
#define LOG2_2PI	5430
#define LOG2_096PI	3262
#define LOG2_BN		(-2144)		// noise: log2 sqrt(sum of the 320 window's squares / 512)
#define QA		4		// fraction bits of the synthesis sum
#define W0_INIT		0x04000000u	// the fundamental before any frame (125 Hz)
#define W0_MIN		0x00400000u	// the range the synthesis is made for: 7.8 Hz ..
#define W0_MAX		0x1FFFFFFFu	// .. 1 kHz, the phase step stays under 2^29

// Spectral amplitude enhancement in the log domain:
// W_l = sqrt(M_l) (0.96 pi (R0^2 + R1^2 - 2 R0 R1 cos(w0 l)) / (w0 R0 (R0^2 - R1^2)))^0.25, limited, 1 for the lowest
// harmonics; then the energy is restored.
void mbe_enhance(int16_t *out, const int16_t *a, int L, uint32_t w0)
{
	int32_t amax = -32768, R0 = 0, R1 = 0;	// R0, R1 under 56 * 2^24
	int64_t R00, D2, r0r1, s2, R0p = 0;

	for (int l = 0; l < L; l++)
		if (a[l] > amax)
			amax = a[l];
	for (int l = 0; l < L; l++) {
		int32_t m = mbe_exp2_q11(a[l] - amax + 12 * 2048);	// Q12, at most 2^12
		int32_t m2 = m * m;					// Q24

		R0 += m2;
		R1 += fxp_mulshr(m2, mbe_cos(w0 * (uint32_t)(l + 1)), 15);
	}
	for (int l = 0; l < L; l++)
		out[l] = a[l];
	R00 = fxp_mul64(R0, R0);
	D2 = R00 - fxp_mul64(R1, R1);
	if (R0 <= 0 || D2 <= (R00 >> 30))
		return;
	{
		int32_t lden = mbe_log2_q11(w0) - 32 * 2048 + LOG2_2PI + mbe_log2_q11_64((uint64_t)R0) + mbe_log2_q11_64((uint64_t)D2);
		int32_t base = 24 * 2048 + LOG2_096PI - lden, lg, rh, rl;

		r0r1 = fxp_mul64(R0, R1) >> 15;
		s2 = R00 + fxp_mul64(R1, R1);
		rh = (int32_t)(r0r1 >> 16);
		rl = (int32_t)((uint32_t)r0r1 & 0xFFFFu);	// r0r1 = rh 2^16 + rl, |r0r1| < 2^46
		for (int l = 0; l < L; l++) {
			int32_t lw = 0, m;

			if (a[l] == MBE_A_SILENT)
				continue;
			if (ENH_SKIP * (l + 1) > L) {
				int32_t c = mbe_cos(w0 * (uint32_t)(l + 1)), lx;
				int64_t num = s2 - 2 * (int64_t)(((uint64_t)fxp_mul64(rh, c) << 16) + (uint64_t)(int64_t)(rl * c));

				if (num < 1)
					num = 1;
				lx = mbe_log2_q11_64((uint64_t)num) + base;
				lw = ((a[l] - amax) >> 1) + fxp_mulshr(lx, ENH_EXP, 11);
				lw = fxp_clamp(lw, ENH_LO, ENH_HI);
			}
			out[l] = (int16_t)fxp_clamp(a[l] + lw, -32767, 32767);
			m = mbe_exp2_q11(out[l] - amax + 12 * 2048);
			R0p = fxp_mac64(R0p, m, m);
		}
		if (R0p <= 0)
			return;
		lg = (mbe_log2_q11_64((uint64_t)R0) - mbe_log2_q11_64((uint64_t)R0p)) >> 1;
		for (int l = 0; l < L; l++)
			if (a[l] != MBE_A_SILENT)
				out[l] = (int16_t)fxp_clamp(out[l] + lg, -32767, 32767);
	}
}

// Each harmonic's phase offset from l times the fundamental's phase (TIA-102.BABA's voiced synthesis): none for the
// lowest quarter of the harmonics; above, a random one, uniform over a turn, scaled by the share of the frame's
// harmonics that are unvoiced. An all-voiced frame is coherent; the more of it is noise, the more the rest is dispersed.
static void set_phases(mbe_frame_t *f, uint32_t seed)
{
	int L = f->L, uv = 0;
	uint32_t rng = seed | 1u;

	for (int l = 0; l < L; l++)
		uv += f->v[l] != 1;
	mbe_rand(&rng);
	for (int l = 1; l <= L; l++) {
		int32_t r = (int16_t)(mbe_rand(&rng) >> 16);	// a turn is 2^16

		f->th[l - 1] = 4 * l <= L ? 0 : (uint16_t)(r * uv / L);
	}
}

// one sinusoid added into acc[0..N-1]: phase ph, step inc changing by dinc per sample, amplitude from the knots k[0..4]
// (Q4) linearly over four 40-sample pieces
static void osc(int32_t *acc, uint32_t ph, uint32_t inc, uint32_t dinc, const int32_t *k)
{
	for (int j = 0; j < N / SEG; j++) {
		int32_t a = k[j] * 256, da = fxp_mulshr((k[j + 1] - k[j]) * 256, 53687091, 31);	// / SEG
		int32_t *o = acc + j * SEG;

		for (int n = 0; n < SEG; n++) {
			a += da;
			ph += inc;
			inc += dinc;
			o[n] += fxp_mulshr(a >> 8, mbe_cos(ph), 15);
		}
	}
}

// log2 amplitude (Q11) to linear, Q4, at most 2^20
static int32_t amp_lin(int32_t a) { int32_t v = mbe_exp2_q11(a + (QA << 11)); return v > (1 << 20) ? (1 << 20) : v; }
static inline int32_t shq(int32_t v, int qs) { return qs >= QA ? v >> (qs - QA) : v * (1 << (QA - qs)); }

// The fade across a pitch jump (a frame that is not continuous): the old harmonics' and the new ones' amplitude at
// samples 0, 40, 80, 120, 160, Q15 (a Hann crossfade over the frame).
static const int16_t JF[2][5] = {
	{ 32767, 27968, 16383, 4798, 0 },
	{ 0, 4799, 16384, 27969, 32767 },
};

// synthesis of one frame from the previous frame's amplitudes to this one's
static void synth(const mbe_mem_t *m, const mbe_frame_t *f, int16_t *pcm)
{
	mbe_state_t *s = m->s;
	mbe_frame_t *p = m->prev;
	int L0 = p->L, L1 = f->L, any = 0, qs = 0;
	uint32_t W0 = p->w0, W1 = f->w0, rng;
	int32_t *buf = m->buf, *acc = buf, D;
	int32_t x1, y1;

	// Noise part (TIA-102.BABA's unvoiced synthesis): 320 samples of white noise under a Hann window centered on the
	// frame's end, its spectrum set band by band to the unvoiced harmonics' amplitudes (voiced bands zeroed), back to
	// time and overlap-added. Seeded per frame, so a frame that changes (a bit error) doesn't shift the noise after it.
	rng = (s->rng += 0x9E3779B9u) | 1u;
	mbe_rand(&rng);
	mbe_rand(&rng);
	for (int l = 0; l < L1; l++)
		if (f->v[l] != 1)
			any = 1;
	if (any) {
		uint32_t bwq = W1 >> 8;		// half a harmonic spacing in FFT bins, Q16
		int32_t lmax = -32768;
		int done = 1;

		for (int l = 1; l <= L1; l++)
			if (f->v[l - 1] != 1 && f->a[l - 1] > lmax)
				lmax = f->a[l - 1];
		qs = fxp_clamp((15 * 2048 - (lmax + G_NOISE)) >> 11, 0, 12);	// spectrum built 2^qs times larger, for low levels
		for (int n = 0; n < N; n++) {
			// Hann window of 320: (1 - cos(pi n / 160)) / 2, Q15
			int32_t w = (32768 - mbe_cos_i((uint32_t)n * 13421773u)) >> 1;	// half a turn over 160 samples

			if (w > 32767)
				w = 32767;
			buf[n] = ((int32_t)mbe_rand(&rng) >> 20) * w >> 15;
			buf[2 * N - 1 - n] = ((int32_t)mbe_rand(&rng) >> 20) * w >> 15;
		}
		memset(buf + 2 * N, 0, (2 * MBE_FFT_N - 2 * N) * sizeof buf[0]);
		mbe_rfft(buf);
		buf[0] = buf[1] = 0;
		for (int l = 1; l <= L1; l++) {
			int lo, hi;

			if (f->v[l - 1] == 1) {
				hi = (int)(((uint32_t)(2 * l + 1) * bwq + 65535u) >> 16);
				if (hi > MBE_FFT_N)
					hi = MBE_FFT_N;
				for (; done < hi; done++)
					buf[2 * done] = buf[2 * done + 1] = 0;
				continue;
			}
			lo = (int)(((uint32_t)(2 * l - 1) * bwq + 65535u) >> 16);
			hi = (int)(((uint32_t)(2 * l + 1) * bwq + 65535u) >> 16);
			if (lo < 1)
				lo = 1;
			if (hi > MBE_FFT_N)
				hi = MBE_FFT_N;
			if (hi <= lo)
				continue;
			{
				int32_t la = f->a[l - 1] + G_NOISE - (mbe_log2_q11((uint32_t)(hi - lo)) >> 1) - 1024 + (qs << 11);
				int64_t e = 0;
				int32_t lr, g;

				for (; done < lo; done++)
					buf[2 * done] = buf[2 * done + 1] = 0;
				for (int b = lo; b < hi; b++)
					e = fxp_mac64(fxp_mac64(e, buf[2 * b], buf[2 * b]), buf[2 * b + 1], buf[2 * b + 1]);
				// the gain that takes the band's rms (2^lr) to the amplitude's (2^la)
				lr = (mbe_log2_q11_64((uint64_t)e + 1) - mbe_log2_q11((uint32_t)(hi - lo))) >> 1;
				g = mbe_exp2_q11(fxp_clamp(la + LOG2_BN - lr + 15 * 2048, 0, 30 * 2048));
				for (int b = lo; b < hi; b++) {
					buf[2 * b] = fxp_mulshr(buf[2 * b], g, 15);
					buf[2 * b + 1] = fxp_mulshr(buf[2 * b + 1], g, 15);
				}
				done = hi;
			}
		}
		for (; done < MBE_FFT_N; done++)
			buf[2 * done] = buf[2 * done + 1] = 0;
		mbe_irfft(buf);
	} else {
		memset(buf, 0, 2 * N * sizeof buf[0]);
	}
	// the noise's first half plus the last frame's second half, Q4, is the base the harmonics are added to
	for (int n = 0; n < N; n++) {
		int32_t t = shq(buf[N + n], qs);

		acc[n] = shq(buf[n], qs) + (int32_t)s->utail[n] * (1 << QA);
		s->utail[n] = (int16_t)fxp_clamp((t + (1 << (QA - 1))) >> QA, -32768, 32767);
	}
	// voiced part
	D = (int32_t)(W1 - W0);
	if (L0 > 0 && L1 > 0 && (D < 0 ? -D : D) < (int32_t)(W1 / 10)) {	// continuous pitch: within a tenth (TIA-102.BABA)
		int32_t dd = D / N;
		int Lm = L0 > L1 ? L0 : L1;

		for (int l = 1; l <= Lm; l++) {
			int h0 = l <= L0 && p->v[l - 1] == 1, h1 = l <= L1 && f->v[l - 1] == 1;
			int32_t k[5];
			uint32_t th0, th1, ph, dinc, inc;

			if (!h0 && !h1)
				continue;
			if (h0 && h1) {
				int32_t A0 = amp_lin(p->a[l - 1]), A1 = amp_lin(f->a[l - 1]);

				// the amplitude moves in a straight line over the frame (TIA-102.BABA)
				for (int j = 0; j <= 4; j++)
					k[j] = A0 + (A1 - A0) * j / 4;
			} else {
				int32_t A = amp_lin(h1 ? f->a[l - 1] : p->a[l - 1]);

				for (int j = 0; j <= 4; j++)
					k[j] = h1 ? A * j / 4 : A * (4 - j) / 4;
			}
			th0 = l <= L0 ? (uint32_t)p->th[l - 1] << 16 : (uint32_t)f->th[l - 1] << 16;
			th1 = l <= L1 ? (uint32_t)f->th[l - 1] << 16 : th0;
			ph = (uint32_t)l * s->psi + th0;
			dinc = (uint32_t)l * (uint32_t)dd;
			// the first sample adds W0 + dd; the offset moves from th0 to th1 over the frame
			inc = (uint32_t)l * W0 + (uint32_t)((int32_t)(th1 - th0) / N);
			osc(acc, ph, inc + dinc, dinc, k);
		}
		s->psi += W0 * N + (uint32_t)fxp_mulshr(D, N + 1, 1);
	} else {
		uint32_t psiN = s->psi + W0 * (N / 2) + W1 * (N / 2);

		// the old harmonics fade out at their pitch, the new ones fade in, phase-aligned at the frame's end
		for (int pass = 0; pass < 2; pass++) {
			const mbe_frame_t *q = pass ? f : p;
			uint32_t W = pass ? W1 : W0;

			for (int l = 1; l <= q->L; l++) {
				int32_t A, k[5];
				uint32_t ph;

				if (q->v[l - 1] != 1)
					continue;
				A = amp_lin(q->a[l - 1]);
				for (int j = 0; j <= 4; j++)
					k[j] = fxp_mulshr(A, JF[pass][j], 15);	// linear between the knots
				ph = (uint32_t)l * (pass ? psiN - W * N : s->psi) + ((uint32_t)q->th[l - 1] << 16);
				osc(acc, ph, (uint32_t)l * W, 0, k);
			}
		}
		s->psi = psiN;
	}
	// Output: a DC blocker (pole at 1 - 1/128), state in Q12. The sum is limited first so nothing overflows.
	x1 = s->dc[0];
	y1 = s->dc[1];
	for (int n = 0; n < N; n++) {
		int32_t x = fxp_clamp(acc[n], -(1 << 20), 1 << 20);

		y1 = (x - x1) * 256 + y1 - (y1 >> 7);
		x1 = x;
		pcm[n] = (int16_t)fxp_clamp((y1 + (1 << 11)) >> 12, -32768, 32767);
	}
	s->dc[0] = x1;
	s->dc[1] = y1;
	*p = *f;
}

void mbe_synth_init(const mbe_mem_t *m)
{
	memset(m->s, 0, sizeof *m->s);
	memset(m->prev, 0, sizeof *m->prev);
	m->s->rng = 0x2545F491u;
	m->prev->w0 = W0_INIT;
}

int mbe_synth_frame(const mbe_mem_t *m, const mbe_params_t *in, int bad, int16_t pcm[MBE_FRAME_LEN])
{
	mbe_state_t *s = m->s;
	mbe_frame_t *f = m->cur;
	const mbe_params_t *src = in;

	if (!in)
		bad = 1;
	if (bad) {
		if (s->run <= MBE_REPEAT_MAX)
			s->run++;
		src = s->have_last && s->run <= MBE_REPEAT_MAX ? &s->last : NULL;
	} else {
		s->run = 0;
		s->last = *in;
		s->have_last = 1;
	}
	memset(f, 0, sizeof *f);
	if (!src || src->L <= 0) {
		f->w0 = m->prev->w0;
	} else {
		int L = src->L > MBE_MAXL ? MBE_MAXL : src->L;

		f->L = (int16_t)L;
		f->w0 = src->w0 < W0_MIN ? W0_MIN : src->w0 > W0_MAX ? W0_MAX : src->w0;
		if (src->flags & MBE_NO_ENHANCE)
			memcpy(f->a, src->a, (size_t)L * sizeof f->a[0]);
		else
			mbe_enhance(f->a, src->a, L, f->w0);
		memcpy(f->v, src->v, (size_t)L);
		set_phases(f, s->rng ^ 0x5BD1E995u);
	}
	synth(m, f, pcm);
	return s->run;
}
