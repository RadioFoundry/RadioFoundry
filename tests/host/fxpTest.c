// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The fixed-point kernels (core/dsp/fxp.h): the plain versions against wide arithmetic. (A core's own body is checked
// against these on that core: the host only has the plain ones.)
#include <stdint.h>
#include <stdio.h>

#include "dsp/fxp.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0x1234567;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

int main(void)
{
	for (int t = 0; t < 20000; t++) {
		int16_t a[24], b[24];
		int64_t want = 0;
		unsigned n = 2 * (1 + rng() % 12);

		for (int i = 0; i < 24; i++) {
			// (full scale in a quarter of the runs: -32768 x -32768 is the one product that needs care)
			a[i] = (t & 3) ? (int16_t)rng() : (rng() & 1) ? 32767 : -32768;
			b[i] = (t & 3) ? (int16_t)rng() : (rng() & 1) ? 32767 : -32768;
		}
		for (int i = 0; i < 8; i++)
			want += (int64_t)a[i] * b[i];
		CHECK(fxp_dot16_8(a, b) == (int32_t)(uint32_t)want, "dot16_8");
		CHECK(fxp_dot16_8(a + 1, b + 1) == fxp_dot16(a + 1, b + 1, 8), "dot16_8 and dot16 agree, odd start");
		want = 0;
		for (unsigned i = 0; i < n; i++)
			want += (int64_t)a[i] * b[i];
		CHECK(fxp_dot16(a, b, n) == (int32_t)(uint32_t)want, "dot16 over %u", n);
	}
	for (int t = 0; t < 200000; t++) {
		int32_t x = (int32_t)(rng() % (1u << 24)) - (1 << 23), y = (int32_t)rng(), s = (int)(rng() % 32);
		uint32_t g = rng() >> (rng() % 23 + 9);
		int64_t p = (int64_t)x * g;

		if (g < (1u << 24) && p < ((int64_t)1 << 39) && p > -((int64_t)1 << 39))
			CHECK(fxp_scale16(x, g) == (int32_t)(p >> 16), "scale16 %d x %u", (int)x, (unsigned)g);
		CHECK(fxp_mul64(x, y) == (int64_t)x * y, "mul64");
		CHECK(fxp_mulshr(x, y, s) == (int32_t)(uint32_t)((uint64_t)((int64_t)x * y) >> s), "mulshr %d", (int)s);
		CHECK(fxp_mac64(p, x, y) == p + (int64_t)x * y, "mac64");
	}
	// The soft modem's use: a sum of five 16-bit samples by a gain of up to 2^20
	CHECK(fxp_scale16(-163840, 1u << 20) == -163840 * 16 && fxp_scale16(163839, 1u << 20) == 163839 * 16, "scale16 at the modem's ends");
	CHECK(fxp_sat16(40000) == 32767 && fxp_sat16(-40000) == -32768 && fxp_sat16(-5) == -5, "sat16");
	printf("fxpTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
