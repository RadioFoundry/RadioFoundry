// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 mbe contributors
// Scalar helpers for the D-STAR codec's fixed-point MBE code (dstar_mbe_fixed.h), and its tables
#include "dstar/dstar_mbe_fixed.h"

#include "dstar_mbe_tables.inc"

int32_t dstar_mbe_log2_q12(uint32_t x)
{
	if (x == 0)
		return -32 * 4096;
	int e = 31 - dstar_mbe_clz(x);
	uint32_t m = x << (31 - e);	// 1.31: [2^31, 2^32)
	uint32_t k = (m >> 26) & 31u, t = (m >> 10) & 0xffffu;
	uint32_t a = dstar_mbe_log2_tab_q16[k], b = dstar_mbe_log2_tab_q16[k + 1];
	uint32_t f = a + (((b - a) * t) >> 16);	// Q16 fraction
	return (int32_t)(e << 12) + (int32_t)((f + 8) >> 4);
}

uint32_t dstar_mbe_pow2_q12(int32_t x)
{
	int32_t i = x >> 12;	// floor
	uint32_t f = (uint32_t)x & 4095u, k = f >> 7, t = f & 127u;
	uint32_t a = dstar_mbe_pow2_tab_q28[k], b = dstar_mbe_pow2_tab_q28[k + 1];
	uint32_t m = a + (((b - a) * t) >> 7);	// Q28, [2^28, 2^29)
	if (i >= 31)
		return 0x7fffffffu;
	if (i >= 28)
		return m << (i - 28);
	int32_t sh = 28 - i;
	if (sh > 29)
		return 0;	// below one half
	return (m + (1u << (sh - 1))) >> sh;
}
