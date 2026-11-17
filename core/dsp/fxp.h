// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fixed-point kernels for the signal code (the soft modem, the voice codecs). Each is defined by its plain C
// version (suffix _c, always there, the one the host tests run). A core with instructions for one gets its own
// body, which must give the plain version's result for every input:
//
//   Arm with the DSP extension (__ARM_FEATURE_DSP)   16 x 16 multiply-accumulates two at a time on packed pairs
//   C-SKY with the HI/LO multiply unit               32 x 32 -> 64 products without a library call. Only in a file
//     (FXP_CSKY_HILO defined for the file)           built with -Wa,-mdsp, and only in code that runs in one task:
//                                                    the context switch does not save HI and LO
//
// What the compilers do alone, so the code here and its callers stay plain C where that is as good: a 16 x 16
// product and its accumulate are one instruction on both cores from `acc += (int16_t)a * (int16_t)b`; a clamp to
// 16 bits is one on Arm. What they do not: pair two multiply-accumulates (Arm), or multiply 32 x 32 -> 64 on
// C-SKY, where it is a library call (use fxp_mulshr and friends below, or keep the product in 32 bits).
//
// Rules for a loop that runs per sample, from the same measurements: int or unsigned counters and indexes (a
// narrower one costs an extend on every step on C-SKY), no % on a ring index (a divide instruction there: wrap
// with a compare, or lay the history out twice so a window is always in one piece), no int64_t.
#ifndef RFOUNDRY_DSP_FXP_H
#define RFOUNDRY_DSP_FXP_H

#include <stdint.h>

#if defined(__ARM_FEATURE_DSP) && !defined(FXP_FORCE_PLAIN)
#define FXP_ARM_DSP	1
#include <arm_acle.h>
#endif
#if defined(__csky__) && defined(FXP_CSKY_HILO) && !defined(FXP_FORCE_PLAIN)
#define FXP_HILO	1
#endif

#define FXP_INLINE	static inline __attribute__((always_inline))
typedef uint32_t __attribute__((may_alias)) fxp_u32a;

// --- Plain versions ---

// One product as unsigned: each fits int32_t (at most 2^30), and the sums wrap in uint32_t as the DSP instructions do,
// where a wrap in int would be undefined
#define FXP_P(a, b)	((uint32_t)((int32_t)(a) * (int32_t)(b)))

// The sum of a[i] * b[i] over 8, wrapping at 32 bits (the caller scales so it does not)
FXP_INLINE int32_t fxp_dot16_8_c(const int16_t *a, const int16_t *b)
{
	return (int32_t)(FXP_P(a[0], b[0]) + FXP_P(a[1], b[1]) + FXP_P(a[2], b[2]) + FXP_P(a[3], b[3]) + FXP_P(a[4], b[4])
			 + FXP_P(a[5], b[5]) + FXP_P(a[6], b[6]) + FXP_P(a[7], b[7]));
}

// The sum over n, n even
FXP_INLINE int32_t fxp_dot16_c(const int16_t *a, const int16_t *b, unsigned n)
{
	uint32_t y = 0;

	for (unsigned i = 0; i < n; i += 2)
		y += FXP_P(a[i], b[i]) + FXP_P(a[i + 1], b[i + 1]);
	return (int32_t)y;
}

// (x * g) >> 16 exactly, in 32-bit steps, for g < 2^24 and |x * g| < 2^39 (a result under 2^23). The high part alone
// may pass 2^31 at the range's edge while the sum stays inside: add in uint32_t, where the wrap is defined
FXP_INLINE int32_t fxp_scale16(int32_t x, uint32_t g)
{
	return (int32_t)((uint32_t)(x >> 8) * g + (((uint32_t)(x & 0xFF) * g) >> 8)) >> 8;
}

FXP_INLINE int32_t fxp_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
FXP_INLINE int16_t fxp_sat16(int32_t x) { return (int16_t)fxp_clamp(x, -32768, 32767); }

// a * b, exact
FXP_INLINE int64_t fxp_mul64_c(int32_t a, int32_t b) { return (int64_t)a * b; }
// (a * b) >> s, the low 32 bits of the shifted product, 0 <= s < 32
FXP_INLINE int32_t fxp_mulshr_c(int32_t a, int32_t b, int s)
{
	return (int32_t)(uint32_t)((uint64_t)((int64_t)a * b) >> s);
}

// --- Per core ---

#if FXP_ARM_DSP
// Unaligned word loads: fine on every Arm core that has these instructions
FXP_INLINE int32_t fxp_dot16_8(const int16_t *a, const int16_t *b)
{
	const fxp_u32a *p = (const fxp_u32a *)(const void *)a, *q = (const fxp_u32a *)(const void *)b;
	int32_t y = (int32_t)__smuad((int16x2_t)p[0], (int16x2_t)q[0]);

	y = (int32_t)__smlad((int16x2_t)p[1], (int16x2_t)q[1], y);
	y = (int32_t)__smlad((int16x2_t)p[2], (int16x2_t)q[2], y);
	return (int32_t)__smlad((int16x2_t)p[3], (int16x2_t)q[3], y);
}

FXP_INLINE int32_t fxp_dot16(const int16_t *a, const int16_t *b, unsigned n)
{
	const fxp_u32a *p = (const fxp_u32a *)(const void *)a, *q = (const fxp_u32a *)(const void *)b;
	int32_t y = 0;

	for (unsigned i = 0; i < n / 2; i++)
		y = (int32_t)__smlad((int16x2_t)p[i], (int16x2_t)q[i], y);
	return y;
}
#else
FXP_INLINE int32_t fxp_dot16_8(const int16_t *a, const int16_t *b) { return fxp_dot16_8_c(a, b); }
FXP_INLINE int32_t fxp_dot16(const int16_t *a, const int16_t *b, unsigned n) { return fxp_dot16_c(a, b, n); }
#endif

#if FXP_HILO
// Each statement sets HI:LO with its own multiply and reads them back: nothing depends on what they held before,
// and neither is written from a register
FXP_INLINE int64_t fxp_mul64(int32_t a, int32_t b)
{
	uint32_t h, l;

	__asm__("muls %2, %3\n\tmfhi %0\n\tmflo %1" : "=&r"(h), "=&r"(l) : "r"(a), "r"(b));
	return (int64_t)(((uint64_t)h << 32) | l);
}

FXP_INLINE int32_t fxp_mulshr(int32_t a, int32_t b, int s)
{
	uint32_t h, l;

	__asm__("muls %2, %3\n\tmfhi %0\n\tmflo %1" : "=&r"(h), "=&r"(l) : "r"(a), "r"(b));
	return s == 0 ? (int32_t)l : (int32_t)((h << (32 - s)) | (l >> s));
}
#else
FXP_INLINE int64_t fxp_mul64(int32_t a, int32_t b) { return fxp_mul64_c(a, b); }
FXP_INLINE int32_t fxp_mulshr(int32_t a, int32_t b, int s) { return fxp_mulshr_c(a, b, s); }
#endif

// acc + a * b (wraps like a hardware accumulate)
FXP_INLINE int64_t fxp_mac64(int64_t acc, int32_t a, int32_t b)
{
	return (int64_t)((uint64_t)acc + (uint64_t)fxp_mul64(a, b));
}

#endif
