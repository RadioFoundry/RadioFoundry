// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fixed-point helpers of the MBE core (mbe_dsp.h): 2^x, log2, interpolated cosine, and a real FFT built on a radix-2
// complex FFT with Q15 twiddles from the quarter-wave table.
#include "mbe_dsp.h"
#include "../dsp/fxp.h"

int32_t mbe_cos_i(uint32_t ph)
{
	uint32_t i = ph >> 22;
	int32_t c0 = mbe_sin_idx(i + 256u), c1 = mbe_sin_idx(i + 257u);

	return c0 + fxp_mulshr(c1 - c0, (int32_t)(ph & 0x3FFFFFu), 22);
}

int32_t mbe_exp2_q11(int32_t x)
{
	int32_t ip = x >> 11;				// floor
	uint32_t fr = (uint32_t)x & 2047u;		// 0..2047
	uint32_t i = fr >> 5, f = fr & 31u;
	uint32_t m = mbe_exp2_tab[i] + (((uint32_t)(mbe_exp2_tab[i + 1] - mbe_exp2_tab[i]) * f) >> 5);	// Q14, 1..2

	if (ip >= 30)
		return 1 << 30;
	if (ip >= 14)
		return (int32_t)(m << (ip - 14));
	if (ip < -1)
		return 0;
	return (int32_t)(m >> (14 - ip));
}

static int clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }

int32_t mbe_log2_q11(uint32_t x)
{
	int e;
	uint32_t m, fr, i, f, l;

	if (!x)
		return -(64 << 11);
	e = 31 - clz32(x);				// x in [2^e, 2^(e+1))
	m = e >= 16 ? x >> (e - 16) : x << (16 - e);	// Q16, 1..2
	fr = m & 0xFFFFu;
	i = fr >> 10;
	f = fr & 1023u;
	l = mbe_log2_tab[i] + (((uint32_t)(mbe_log2_tab[i + 1] - mbe_log2_tab[i]) * f) >> 10);	// Q15
	return (e << 11) + (int32_t)(l >> 4);
}

int32_t mbe_log2_q11_64(uint64_t x)
{
	uint32_t h = (uint32_t)(x >> 32), l = (uint32_t)x;

	if (h) {
		int s = 32 - clz32(h);			// 1..32: x >> s fits 32 bits, no variable 64-bit shift
		return mbe_log2_q11(s == 32 ? h : (h << (32 - s)) | (l >> s)) + (s << 11);
	}
	return mbe_log2_q11(l);
}

static inline __attribute__((always_inline)) int32_t mulq15(int32_t a, int32_t b) { return fxp_mulshr(a, b, 15); }

// Complex FFT of N points, radix 2, decimation in time, no scaling; the inverse (e^{+i}) when inv. The k = 0 and
// k = half/2 butterflies of each stage need no multiply.
static void cfft(int32_t *z, int inv)
{
	const int n = MBE_FFT_N;

	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;

		for (; j & bit; bit >>= 1)
			j ^= bit;
		j |= bit;
		if (i < j) {
			int32_t t = z[2 * i];

			z[2 * i] = z[2 * j];
			z[2 * j] = t;
			t = z[2 * i + 1];
			z[2 * i + 1] = z[2 * j + 1];
			z[2 * j + 1] = t;
		}
	}
	for (int len = 2, step = 512; len <= n; len <<= 1, step >>= 1) {
		int half = len >> 1;

		for (int i = 0; i < n; i += len) {
			int32_t *a = z + 2 * i, *b = z + 2 * (i + half);
			int32_t tr = b[0], ti = b[1];

			b[0] = a[0] - tr;
			b[1] = a[1] - ti;
			a[0] += tr;
			a[1] += ti;
		}
		if (half < 2)
			continue;
		for (int i = half >> 1; i < n; i += len) {
			int32_t *a = z + 2 * i, *b = z + 2 * (i + half);
			int32_t tr = inv ? -b[1] : b[1], ti = inv ? b[0] : -b[0];

			b[0] = a[0] - tr;
			b[1] = a[1] - ti;
			a[0] += tr;
			a[1] += ti;
		}
		for (int k = 1; k < half; k++) {
			int32_t c, s;

			if (2 * k == half)
				continue;
			c = mbe_sin_idx((uint32_t)(k * step + 256));
			s = mbe_sin_idx((uint32_t)(k * step));
			if (!inv)
				s = -s;
			for (int i = k; i < n; i += len) {
				int32_t *a = z + 2 * i, *b = z + 2 * (i + half);
				int32_t tr = mulq15(b[0], c) - mulq15(b[1], s);
				int32_t ti = mulq15(b[0], s) + mulq15(b[1], c);

				b[0] = a[0] - tr;
				b[1] = a[1] - ti;
				a[0] += tr;
				a[1] += ti;
			}
		}
	}
}

void mbe_irfft(int32_t *buf)
{
	// Fold the half spectrum X[0..N] into N complex points Z_k = (X_k + conj X_{N-k}) + i e^{i pi k/N} (X_k - conj X_{N-k}),
	// then z = IFFT(Z) gives x[2m] = Re z_m and x[2m+1] = Im z_m.
	const int n = MBE_FFT_N;
	int32_t x0 = buf[0], xn = buf[1];

	buf[0] = x0 + xn;
	buf[1] = x0 - xn;
	for (int k = 1; k <= n / 2; k++) {
		int32_t *p = buf + 2 * k, *q = buf + 2 * (n - k);
		int32_t ar = p[0], ai = p[1], br = q[0], bi = q[1];
		int32_t sr = ar + br, si = ai - bi;		// X_k + conj X_{N-k}
		int32_t dr = ar - br, di = ai + bi;		// X_k - conj X_{N-k}
		int32_t c = mbe_sin_idx((uint32_t)(k * 512 / n + 256)), s = mbe_sin_idx((uint32_t)(k * 512 / n));
		int32_t er = mulq15(dr, c) - mulq15(di, s), ei = mulq15(dr, s) + mulq15(di, c);	// e^{i pi k/N} * d

		p[0] = sr - ei;
		p[1] = si + er;
		if (k != n - k) {
			q[0] = sr + ei;
			q[1] = -si + er;
		}
	}
	cfft(buf, 1);
}

void mbe_rfft(int32_t *buf)
{
	// z_m = x[2m] + i x[2m+1], Z = FFT(z); X_k = (Z_k + conj Z_{N-k}) / 2 - i e^{-i pi k/N} (Z_k - conj Z_{N-k}) / 2
	const int n = MBE_FFT_N;
	int32_t z0r, z0i;

	cfft(buf, 0);
	z0r = buf[0];
	z0i = buf[1];
	buf[0] = z0r + z0i;				// X_0 and X_N, both real
	buf[1] = z0r - z0i;
	for (int k = 1; k <= n / 2; k++) {
		int32_t *p = buf + 2 * k, *q = buf + 2 * (n - k);
		int32_t ar = p[0], ai = p[1], br = q[0], bi = q[1];
		int32_t sr = (ar + br) >> 1, si = (ai - bi) >> 1;	// (Z_k + conj Z_{N-k}) / 2
		int32_t dr = (ar - br) >> 1, di = (ai + bi) >> 1;	// (Z_k - conj Z_{N-k}) / 2
		int32_t c = mbe_sin_idx((uint32_t)(k * 512 / n + 256)), s = -mbe_sin_idx((uint32_t)(k * 512 / n));
		int32_t er = mulq15(dr, c) - mulq15(di, s), ei = mulq15(dr, s) + mulq15(di, c);	// e^{-i pi k/N} * d

		p[0] = sr + ei;
		p[1] = si - er;
		if (k != n - k) {
			q[0] = sr - ei;
			q[1] = -si - er;
		}
	}
}
