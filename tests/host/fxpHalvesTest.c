// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// core/dsp/fxp.h's kernels made from 16-bit halves (fxp_mul64_h, fxp_mulshr_h, fxp_mulshr16_h, fxp_mulhr_h: what a C-SKY build
// without HI/LO runs) against the plain 64-bit versions they must equal for every input: the corners of each operand's
// range, every shift, and a few million random pairs, half of them with the low halves at their own edges (0, 1,
// 0x7FFF, 0x8000, 0xFFFF), where a carry or a sign between the halves would go wrong.
#include <stdio.h>
#include <stdint.h>

#include "dsp/fxp.h"

#if defined(FXP_HALVES)
#define FXP_HALVES_BUILT	1
#else
#define FXP_HALVES_BUILT	0
#endif

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t rng_state = 0x2545F491u;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static const int32_t EDGE[] = {
	0, 1, -1, 2, -2, 0x7FFF, 0x8000, -0x8000, -0x8001, 0xFFFF, 0x10000, -0x10000, 0x10001, -0x10001, 0x12345678,
	-0x12345678, 0x7FFFFFFF, -0x7FFFFFFF, (int32_t)0x80000000u, 0x7FFF0000, (int32_t)0x80008000u, 0x0000FFFF,
	(int32_t)0xFFFF0000u, (int32_t)0xFFFF8000u, 0x00008000, 46341, -46341, 1 << 20, -(1 << 20), 1 << 30, -(1 << 30),
};
#define NEDGE	(int)(sizeof EDGE / sizeof EDGE[0])

// A random value, or one whose low half sits at an edge
static int32_t pick(void)
{
	static const uint16_t lo[] = { 0, 1, 0x7FFF, 0x8000, 0xFFFF };
	uint32_t r = rng();

	if (rng() & 1)
		r = (r & 0xFFFF0000u) | lo[rng() % 5u];
	switch (rng() % 4u) {
	case 0: return (int32_t)r;
	case 1: return (int32_t)r >> (rng() % 31u);	// small magnitudes of either sign
	case 2: return EDGE[rng() % (unsigned)NEDGE];
	default: return (int32_t)(r & 0x001FFFFFu) - (1 << 20);	// the synthesis's sample range
	}
}

static void one(int32_t a, int32_t b)
{
	CHECK(fxp_mul64_h(a, b) == fxp_mul64_c(a, b), "mul64 %d * %d", a, b);
	CHECK(fxp_mulhr_h(a, b) == fxp_mulhr_c(a, b), "mulhr %d * %d", a, b);
	for (int s = 0; s < 32; s++) {
		int32_t h = fxp_mulshr_h(a, b, s), c = fxp_mulshr_c(a, b, s);

		CHECK(h == c, "mulshr %d * %d >> %d: %d, plain %d", a, b, s, h, c);
	}
}

static void one16(int32_t x, int32_t c)
{
	for (int s = 0; s <= 16; s++) {
		int32_t h = fxp_mulshr16_h(x, c, s), p = fxp_mulshr_c(x, c, s);

		CHECK(h == p, "mulshr16 %d * %d >> %d: %d, plain %d", x, c, s, h, p);
	}
}

int main(void)
{
	int f0;

	// the corners, each pair
	f0 = failures;
	for (int i = 0; i < NEDGE; i++)
		for (int j = 0; j < NEDGE; j++)
			one(EDGE[i], EDGE[j]);
	printf("fxp_mul64_h, fxp_mulshr_h, fxp_mulhr_h: %d x %d corner pairs, shifts 0..31: %s\n", NEDGE, NEDGE,
	       failures == f0 ? "equal" : "DIFFER");
	f0 = failures;
	for (int i = 0; i < 2000000; i++)
		one(pick(), pick());
	printf("fxp_mul64_h, fxp_mulshr_h, fxp_mulhr_h: 2,000,000 random pairs: %s\n", failures == f0 ? "equal" : "DIFFER");

	// a 16-bit factor: every c from -32768 to 32768 against the corners of x, then random pairs
	f0 = failures;
	for (int32_t c = -32768; c <= 32768; c++)
		for (int i = 0; i < NEDGE; i++)
			one16(EDGE[i], c);
	for (int i = 0; i < 2000000; i++) {
		int32_t c = (int32_t)(rng() % 65537u) - 32768;

		one16(pick(), c);
	}
	printf("fxp_mulshr16_h: every 16-bit factor by %d corners, 2,000,000 random pairs, shifts 0..16: %s\n", NEDGE,
	       failures == f0 ? "equal" : "DIFFER");
	// the selected versions: what this build runs (the halves with -DFXP_SPLIT16)
	f0 = failures;
	for (int i = 0; i < 100000; i++) {
		int32_t a = pick(), b = pick(), c = (int32_t)(rng() % 65537u) - 32768;

		CHECK(fxp_mul64(a, b) == fxp_mul64_c(a, b), "fxp_mul64");
		CHECK(fxp_mulshr(a, b, 15) == fxp_mulshr_c(a, b, 15), "fxp_mulshr");
		CHECK(fxp_mulshr16(a, c, 15) == fxp_mulshr_c(a, c, 15), "fxp_mulshr16");
		CHECK(fxp_mulhr(a, b) == fxp_mulhr_c(a, b), "fxp_mulhr");
	}
	printf("fxp_mul64, fxp_mulshr, fxp_mulshr16, fxp_mulhr as built (%s): %s\n", FXP_HALVES_BUILT ? "16-bit halves" : "plain",
	       failures == f0 ? "equal" : "DIFFER");
	printf("fxpHalvesTest: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
