// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Packed dibits (hal_air.h): four symbols to a byte, the first on air in bits 7-6 of byte 0.
#ifndef RFOUNDRY_AIR_SYMS_H
#define RFOUNDRY_AIR_SYMS_H

#include <stdint.h>

static inline uint8_t air_sym_get(const uint8_t *p, uint16_t i)
{
	return (uint8_t)((p[i >> 2] >> (6 - 2 * (i & 3))) & 3);
}

static inline void air_sym_put(uint8_t *p, uint16_t i, uint8_t dibit)
{
	uint8_t sh = (uint8_t)(6 - 2 * (i & 3));

	p[i >> 2] = (uint8_t)((p[i >> 2] & ~(3u << sh)) | ((dibit & 3u) << sh));
}

// The symbol level of a dibit: 01 = +3, 00 = +1, 10 = -1, 11 = -3
static inline int8_t air_sym_level(uint8_t dibit)
{
	static const int8_t level[4] = { 1, 3, -1, -3 };

	return level[dibit & 3];
}

// n symbols from src at symbol si to dst at symbol di (the ranges must not overlap)
static inline void air_syms_copy(uint8_t *dst, uint16_t di, const uint8_t *src, uint16_t si, uint16_t n)
{
	while (n--)
		air_sym_put(dst, di++, air_sym_get(src, si++));
}

// Bits, most significant first, for the modes' own fields: bit i of a packed stream (bit 0 = bit 7 of byte 0)
static inline uint8_t air_bit_get(const uint8_t *p, uint16_t i)
{
	return (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1);
}

static inline void air_bit_put(uint8_t *p, uint16_t i, uint8_t bit)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7));

	p[i >> 3] = (uint8_t)(bit ? (p[i >> 3] | m) : (p[i >> 3] & ~m));
}

#endif
