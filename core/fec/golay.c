// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

#include "golay.h"

// Parity rows P[j] of the (24,12,8) systematic generator [I | B] (TIA-102.BAAA-A Table 5-3,
// 4-digit octal parity halves). Row j belongs to data bit 11-j. Its low bit is the overall
// parity bit; the upper 11 bits are x^(22-j) mod g(x), g = x^11+x^10+x^6+x^5+x^4+x^2+1 (6165 octal).
static const uint16_t row[12] = {
	06165, 03073, 07550, 03664, 01732, 06631, 03315, 01547, 06706, 05227, 04476, 04353,
};

static unsigned popcnt(uint32_t v)
{
	unsigned n = 0;
	for (; v; v &= v - 1) {
		n++;
	}
	return n;
}

// d * B over GF(2): XOR of the parity rows of the set data bits.
static uint16_t parity12(uint16_t d)
{
	uint16_t p = 0;
	for (unsigned j = 0; j < 12; j++) {
		if (d & (0x800 >> j)) {
			p ^= row[j];
		}
	}
	return p;
}

// s * B^T: B is orthogonal (B B^T = I), which turns the syndrome of a parity-side error
// pattern into the error pattern itself. Bit j is the dot product of s with row j.
static uint16_t parity12_t(uint16_t s)
{
	uint16_t p = 0;
	for (unsigned j = 0; j < 12; j++) {
		p |= (uint16_t)(popcnt(s & row[j]) & 1) << (11 - j);
	}
	return p;
}

// Column i of B as a 12-bit vector (row i of B^T).
static uint16_t column(unsigned i)
{
	uint16_t c = 0;
	for (unsigned j = 0; j < 12; j++) {
		c |= (uint16_t)((row[j] >> (11 - i)) & 1) << (11 - j);
	}
	return c;
}

// Error-pattern search for the extended code (the classic 4-step decoder for G = [I | B],
// B B^T = I). With r = (a | b) and e = (e1 | e2): s = b + aB = e2 + e1 B, s' = s B^T = e1 + e2 B^T.
// On success *fixed is the corrected word and the return value is the weight corrected.
static int golay24_fix(uint32_t cw, uint32_t *fixed)
{
	uint16_t a = (cw >> 12) & 0xFFF;
	uint16_t b = cw & 0xFFF;
	uint16_t s = b ^ parity12(a);
	uint16_t e1 = 0;
	uint16_t e2 = 0;
	int ok = 0;

	if (popcnt(s) <= 3) {
		e2 = s;
		ok = 1;
	}
	for (unsigned i = 0; i < 12 && !ok; i++) {
		if (popcnt(s ^ row[i]) <= 2) {
			e1 = 0x800 >> i;
			e2 = s ^ row[i];
			ok = 1;
		}
	}
	if (!ok) {
		uint16_t s2 = parity12_t(s);
		if (popcnt(s2) <= 3) {
			e1 = s2;
			ok = 1;
		}
		for (unsigned i = 0; i < 12 && !ok; i++) {
			uint16_t c = column(i);
			if (popcnt(s2 ^ c) <= 2) {
				e2 = 0x800 >> i;
				e1 = s2 ^ c;
				ok = 1;
			}
		}
	}
	if (!ok) {
		return -1;
	}
	*fixed = cw ^ ((uint32_t)e1 << 12) ^ e2;
	return (int)(popcnt(e1) + popcnt(e2));
}

uint32_t fec_golay24_encode(uint16_t data12)
{
	data12 &= 0xFFF;
	return ((uint32_t)data12 << 12) | parity12(data12);
}

int fec_golay24_decode(uint32_t cw, uint16_t *data12)
{
	uint32_t fixed;
	int n = golay24_fix(cw & 0xFFFFFF, &fixed);

	if (n >= 0) {
		*data12 = (uint16_t)(fixed >> 12);
	}
	return n;
}

uint32_t fec_golay23_encode(uint16_t data12)
{
	return fec_golay24_encode(data12) >> 1;
}

// The 23-bit word is the extended word without its overall parity bit. That bit is rebuilt
// as even parity; if the received word had an odd number of errors the rebuilt bit is wrong
// and a 3-error word looks like 4 errors, so the other value is tried. The code is perfect:
// one of the two always lands within 3 of a code word.
int fec_golay23_decode(uint32_t cw, uint16_t *data12)
{
	uint32_t r = cw & 0x7FFFFF;
	uint32_t fixed;
	uint32_t p = popcnt(r) & 1;

	if (golay24_fix((r << 1) | p, &fixed) < 0 && golay24_fix((r << 1) | (p ^ 1), &fixed) < 0) {
		return 0;	// unreachable for a perfect code; keeps the contract total
	}
	fixed >>= 1;
	*data12 = (uint16_t)(fixed >> 11);
	return (int)popcnt(fixed ^ r);
}

uint32_t fec_golay18_encode(uint8_t data6)
{
	return fec_golay24_encode(data6 & 0x3F);
}

// The 6 deleted data bits are known zeros, so an error pattern touching them is rejected.
int fec_golay18_decode(uint32_t cw, uint8_t *data6)
{
	uint32_t fixed;
	int n = golay24_fix(cw & 0x3FFFF, &fixed);

	if (n < 0 || (fixed >> 18)) {
		return -1;
	}
	*data6 = (uint8_t)((fixed >> 12) & 0x3F);
	return n;
}
