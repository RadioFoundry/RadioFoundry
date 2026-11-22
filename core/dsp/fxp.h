// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fixed-point kernels for the signal code (the soft modem, the voice codecs). Each is defined by its plain C
// version (suffix _c, always there, the one the host tests run). A core with instructions for one gets its own
// body, which must give the plain version's result for every input:
//
//   Arm with the DSP extension (__ARM_FEATURE_DSP)   16 x 16 multiply-accumulates two at a time on packed pairs; the
//                                                    rounded high word of a 32 x 32 product in one (SMMULR)
//   C-SKY with the HI/LO multiply unit               32 x 32 -> 64 products without a library call. Only in a file
//     (FXP_CSKY_HILO defined for the file)           built with -Wa,-mdsp, and only in code that runs in one task:
//                                                    the context switch does not save HI and LO
//   C-SKY without it (FXP_SPLIT16 on the host)       32 x 32 -> 64 products from 16-bit halves (the _h versions)
//
// What the compilers do alone, so the code here and its callers stay plain C where that is as good: a 16 x 16
// product and its accumulate are one instruction on both cores from `acc += (int16_t)a * (int16_t)b`; a clamp to
// 16 bits is one on Arm. What they do not: pair two multiply-accumulates (Arm), or multiply 32 x 32 -> 64 on
// C-SKY, where `(int64_t)a * b` is a library call (use fxp_mulshr and friends below, which make it from 16-bit
// halves there, fxp_mulshr16 for a 16-bit factor, or keep the product in 32 bits).
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
#elif (defined(__csky__) || defined(FXP_SPLIT16)) && !defined(FXP_FORCE_PLAIN)
#define FXP_HALVES	1
#endif

#define FXP_INLINE	static inline __attribute__((always_inline))
// A word of two packed 16-bit values, at any halfword: aligned(2), or GCC may pair two loads into an LDRD or LDM, which
// fault on an unaligned address where LDR does not (seen on the radio once a loop was unrolled)
typedef uint32_t __attribute__((may_alias, aligned(2))) fxp_u32a;

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
// Four neighboring outputs of a two-channel FIR filter, the channels summed: c and x hold them interleaved (c[2i] and
// x[2i] channel 0, c[2i + 1] and x[2i + 1] channel 1), so output k adds the sum over i < n of c[2i] x[2i + 2k] +
// c[2i + 1] x[2i + 2k + 1] to y[k], k = 0-3, each wrapping at 32 bits. n taps a channel; reads x[0..2n + 7]
FXP_INLINE void fxp_fir2ch4_c(const int16_t *c, const int16_t *x, unsigned n, int32_t *y)
{
	for (int k = 0; k < 4; k++) {
		uint32_t a = (uint32_t)y[k];

		for (unsigned i = 0; i < 2 * n; i++) {
			a += FXP_P(c[i], x[i + 2 * k]);
		}
		y[k] = (int32_t)a;
	}
}

// (a * b) >> s, the low 32 bits of the shifted product, 0 <= s < 32
FXP_INLINE int32_t fxp_mulshr_c(int32_t a, int32_t b, int s)
{
	return (int32_t)(uint32_t)((uint64_t)((int64_t)a * b) >> s);
}
// (a * b + 2^31) >> 32: the high word of the product, rounded. Two Q31 values give their product in Q30
FXP_INLINE int32_t fxp_mulhr_c(int32_t a, int32_t b)
{
	return (int32_t)((uint64_t)((int64_t)a * b + 0x80000000) >> 32);
}

// --- From 16-bit halves: exactly the plain versions' results, with only 32 x 32 -> 32 products ---
//
// For a core whose 32 x 32 -> 64 multiply is a library call: the CK803S (an HR_C7000's core; the compiler's name for it
// has no DSP extension, ck803se does) makes each with __muldi3, six multiplies, about 25 instructions and a call. On
// the full-rate decoder's worst frames those calls were 41 % of all it ran (an instruction-level model of the CK803S
// build). Each operand splits into its high half xh (signed) and low half xl (0..65535), so
// x y = xh yh 2^32 + (xh yl + xl yh) 2^16 + xl yl; every product fits 32 bits (|xh yl| <= 32768 * 65535 < 2^31, xl yl
// in uint32_t), and the sums carry in 64 or wrap in 32 bits as the wide product's words do.

FXP_INLINE int64_t fxp_mul64_h(int32_t a, int32_t b)
{
	int32_t ah = a >> 16, bh = b >> 16;
	uint32_t al = (uint32_t)a & 0xFFFFu, bl = (uint32_t)b & 0xFFFFu;
	int64_t mid = (int64_t)(ah * (int32_t)bl) + (int64_t)((int32_t)al * bh);

	return (int64_t)(((uint64_t)(int64_t)(ah * bh) << 32) + ((uint64_t)mid << 16) + (uint64_t)(al * bl));
}

// For 0 <= s <= 16 every term but xl yl is a whole multiple of 2^s, so the shift's floor is each term shifted and only
// the low word of each is needed: four products and no 64-bit arithmetic at all. Wider shifts go through the 64 bits
FXP_INLINE int32_t fxp_mulshr_h(int32_t a, int32_t b, int s)
{
	if (s <= 16) {
		int32_t ah = a >> 16, bh = b >> 16;
		uint32_t al = (uint32_t)a & 0xFFFFu, bl = (uint32_t)b & 0xFFFFu;
		uint32_t mid = (uint32_t)(ah * (int32_t)bl) + (uint32_t)((int32_t)al * bh);
		uint32_t hi = s == 0 ? 0u : (uint32_t)(ah * bh) << ((32 - s) & 31);

		return (int32_t)(hi + (mid << (16 - s)) + ((al * bl) >> s));
	}
	return (int32_t)(uint32_t)((uint64_t)fxp_mul64_h(a, b) >> s);
}

// The same for a 16-bit factor c (-32768 <= c <= 32768), 0 <= s <= 16, as a Q15 sine or gain is: c's high half is 0 or
// -1, so two products do, x c = xh c 2^16 + xl c (|xl c| <= 65535 * 32768 < 2^31). The synthesis's oscillators and FFT
FXP_INLINE int32_t fxp_mulshr16_h(int32_t x, int32_t c, int s)
{
	return (int32_t)(((uint32_t)((x >> 16) * c) << (16 - s)) + (uint32_t)(((int32_t)(x & 0xFFFF) * c) >> s));
}

// The rounded high word (fxp_mulhr_c) through the same four products: the 64-bit add and shift are inline on C-SKY,
// only the multiply was a library call. The soft BPTC decoder's Q31 products (DMR) run this on a core without HI/LO
FXP_INLINE int32_t fxp_mulhr_h(int32_t a, int32_t b)
{
	return (int32_t)((uint64_t)(fxp_mul64_h(a, b) + 0x80000000) >> 32);
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

// One instruction (SMMULR) where a 32 x 32 multiply and a 64-bit shift take four
FXP_INLINE int32_t fxp_mulhr(int32_t a, int32_t b)
{
	int32_t y;

	__asm__("smmulr %0, %1, %2" : "=r"(y) : "r"(a), "r"(b));
	return y;
}

FXP_INLINE int32_t fxp_dot16(const int16_t *a, const int16_t *b, unsigned n)
{
	const fxp_u32a *p = (const fxp_u32a *)(const void *)a, *q = (const fxp_u32a *)(const void *)b;
	int32_t y = 0;

	for (unsigned i = 0; i < n / 2; i++)
		y = (int32_t)__smlad((int16x2_t)p[i], (int16x2_t)q[i], y);
	return y;
}

// Each coefficient word (one tap of each channel) is loaded once for all four outputs, and each sample word once:
// two loads and four SMLADs a tap, the sample words rotating through four registers (unrolled by four). c and x must
// be 4-byte aligned
FXP_INLINE void fxp_fir2ch4(const int16_t *c, const int16_t *x, unsigned n, int32_t *y)
{
	const fxp_u32a *p = (const fxp_u32a *)(const void *)c, *q = (const fxp_u32a *)(const void *)x;
	int32_t a0 = y[0], a1 = y[1], a2 = y[2], a3 = y[3];
	uint32_t h0 = q[0], h1 = q[1], h2 = q[2], k, h3;
	unsigned blocks = n / 4;

	q += 3;
	if (blocks) {
		__asm__ volatile(
			"1:\n\t"
			"ldr %[k], [%[p]], #4\n\t"
			"ldr %[h3], [%[q]], #4\n\t"
			"smlad %[a0], %[k], %[h0], %[a0]\n\t"
			"smlad %[a1], %[k], %[h1], %[a1]\n\t"
			"smlad %[a2], %[k], %[h2], %[a2]\n\t"
			"smlad %[a3], %[k], %[h3], %[a3]\n\t"
			"ldr %[k], [%[p]], #4\n\t"
			"ldr %[h0], [%[q]], #4\n\t"
			"smlad %[a0], %[k], %[h1], %[a0]\n\t"
			"smlad %[a1], %[k], %[h2], %[a1]\n\t"
			"smlad %[a2], %[k], %[h3], %[a2]\n\t"
			"smlad %[a3], %[k], %[h0], %[a3]\n\t"
			"ldr %[k], [%[p]], #4\n\t"
			"ldr %[h1], [%[q]], #4\n\t"
			"smlad %[a0], %[k], %[h2], %[a0]\n\t"
			"smlad %[a1], %[k], %[h3], %[a1]\n\t"
			"smlad %[a2], %[k], %[h0], %[a2]\n\t"
			"smlad %[a3], %[k], %[h1], %[a3]\n\t"
			"ldr %[k], [%[p]], #4\n\t"
			"ldr %[h2], [%[q]], #4\n\t"
			"smlad %[a0], %[k], %[h3], %[a0]\n\t"
			"smlad %[a1], %[k], %[h0], %[a1]\n\t"
			"smlad %[a2], %[k], %[h1], %[a2]\n\t"
			"smlad %[a3], %[k], %[h2], %[a3]\n\t"
			"subs %[n], %[n], #1\n\t"
			"bne 1b"
			: [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3), [p] "+r"(p), [q] "+r"(q), [n] "+r"(blocks),
			  [h0] "+r"(h0), [h1] "+r"(h1), [h2] "+r"(h2), [k] "=&r"(k), [h3] "=&r"(h3)
			:
			: "cc", "memory");
	}
	// The last n % 4 taps: h0-h2 are again the next three sample words
	for (unsigned i = 0; i < n % 4; i++) {
		k = *p++;
		h3 = *q++;
		a0 = (int32_t)__smlad((int16x2_t)k, (int16x2_t)h0, a0);
		a1 = (int32_t)__smlad((int16x2_t)k, (int16x2_t)h1, a1);
		a2 = (int32_t)__smlad((int16x2_t)k, (int16x2_t)h2, a2);
		a3 = (int32_t)__smlad((int16x2_t)k, (int16x2_t)h3, a3);
		h0 = h1;
		h1 = h2;
		h2 = h3;
	}
	y[0] = a0;
	y[1] = a1;
	y[2] = a2;
	y[3] = a3;
}
#else
FXP_INLINE void fxp_fir2ch4(const int16_t *c, const int16_t *x, unsigned n, int32_t *y) { fxp_fir2ch4_c(c, x, n, y); }
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

FXP_INLINE int32_t fxp_mulhr(int32_t a, int32_t b)
{
	uint32_t h, l;

	__asm__("muls %2, %3\n\tmfhi %0\n\tmflo %1" : "=&r"(h), "=&r"(l) : "r"(a), "r"(b));
	return (int32_t)(h + (l >> 31));	// + 2^31 carries into the high word when bit 31 of the low one is set
}
#elif FXP_HALVES
FXP_INLINE int64_t fxp_mul64(int32_t a, int32_t b) { return fxp_mul64_h(a, b); }
FXP_INLINE int32_t fxp_mulshr(int32_t a, int32_t b, int s) { return fxp_mulshr_h(a, b, s); }
FXP_INLINE int32_t fxp_mulhr(int32_t a, int32_t b) { return fxp_mulhr_h(a, b); }
#else
FXP_INLINE int64_t fxp_mul64(int32_t a, int32_t b) { return fxp_mul64_c(a, b); }
FXP_INLINE int32_t fxp_mulshr(int32_t a, int32_t b, int s) { return fxp_mulshr_c(a, b, s); }
#if !FXP_ARM_DSP
FXP_INLINE int32_t fxp_mulhr(int32_t a, int32_t b) { return fxp_mulhr_c(a, b); }
#endif
#endif

// (x * c) >> s for a 16-bit c, 0 <= s <= 16: two products where the core has no wide multiply, else fxp_mulshr (one
// instruction on Arm and with HI/LO)
#if FXP_HALVES
FXP_INLINE int32_t fxp_mulshr16(int32_t x, int32_t c, int s) { return fxp_mulshr16_h(x, c, s); }
#else
FXP_INLINE int32_t fxp_mulshr16(int32_t x, int32_t c, int s) { return fxp_mulshr(x, c, s); }
#endif

// acc + a * b (wraps like a hardware accumulate)
FXP_INLINE int64_t fxp_mac64(int64_t acc, int32_t a, int32_t b)
{
	return (int64_t)((uint64_t)acc + (uint64_t)fxp_mul64(a, b));
}

#endif
