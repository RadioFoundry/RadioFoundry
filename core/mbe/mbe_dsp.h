// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fixed-point helpers of the MBE core: sine and cosine of a 32-bit phase, 2^x and log2 in Q11, a noise source and the
// real FFT the unvoiced synthesis uses. No codec knowledge, no float, no libm.
#ifndef MBE_DSP_H
#define MBE_DSP_H

#include <stdint.h>

extern const int16_t mbe_sin_tab[257];		// sin(pi/2 * i / 256), Q15, one quarter wave
extern const uint16_t mbe_exp2_tab[65];		// 2^(14 + i/64)
extern const uint16_t mbe_log2_tab[65];		// 32768 * log2(1 + i/64)

// sin(2 pi i / 1024), Q15, from the quarter wave
static inline int32_t mbe_sin_idx(uint32_t i)
{
	uint32_t q = i & 255u;
	int32_t v = mbe_sin_tab[(i & 256u) ? 256u - q : q];

	return (i & 512u) ? -v : v;
}
// cos of a phase, 2^32 = one turn, Q15, nearest of 1024 steps (the oscillators' per-sample lookup)
static inline int32_t mbe_cos(uint32_t ph) { return mbe_sin_idx(((ph + 0x200000u) >> 22) + 256u); }
// the same with linear interpolation, for the dequantizers and windows (a few hundred calls per frame)
int32_t mbe_cos_i(uint32_t ph);

// 2^(x / 2048) for x in Q11, as an integer (saturates at 2^30, rounds down to 0 below 1/2)
int32_t mbe_exp2_q11(int32_t x);
// log2(x) in Q11 for x > 0 (x = 0 gives -64 * 2048)
int32_t mbe_log2_q11(uint32_t x);
int32_t mbe_log2_q11_64(uint64_t x);

// 32-bit xorshift noise
static inline uint32_t mbe_rand(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

// Inverse real FFT of size 2 * MBE_FFT_N: the half spectrum X[0..N] (re, im interleaved; X[0] and X[N] real, X[N].re in
// buf[1]) to x[0..2N-1], in place on 2N int32 values, no scaling:
// x[n] = Re(X0) + Re(XN)(-1)^n + 2 sum_k Re(X_k e^{i pi k n / N}).
#define MBE_FFT_LOG 8
#define MBE_FFT_N (1 << MBE_FFT_LOG)
void mbe_irfft(int32_t *buf);
// forward real FFT with the inverse's layout: x[0..2N-1] to X[0..N]; X_k = sum x[n] e^{-i pi k n / N}
void mbe_rfft(int32_t *buf);

#endif
