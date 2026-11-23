// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

#include "bch63.h"
#include "gf64.h"

#define NSYN 22		// code roots a^1..a^22 give designed distance 23 (t = 11)
#define TMAX 11

// g(x), degree 47, octal 6331 1413 6723 5453 (TIA-102.BAAA-A 8.5.2); bit 47 is the leading term.
// It is the product of the minimal polynomials of a^1, a^3, ..., a^21.
#define GEN 0xCD930BDD3B2BULL

uint64_t fec_bch63_encode(uint16_t data)
{
	// Long division of data * x^47 by g(x), bit by bit.
	uint64_t cw = (uint64_t)data << 47;
	uint64_t rem = cw;

	for (int i = 62; i >= 47; i--) {
		if (rem & (1ULL << i)) {
			rem ^= GEN << (i - 47);
		}
	}
	return cw | rem;
}

int fec_bch63_decode(uint64_t cw, uint16_t *data)
{
	uint8_t syn[NSYN];
	uint8_t lam[NSYN + 1];
	unsigned nerr = 0;
	uint64_t fixed = cw & 0x7FFFFFFFFFFFFFFFULL;
	int nonzero = 0;

	// S[j] = r(a^(j+1)); bit i of the word is the coefficient of x^i.
	for (unsigned j = 0; j < NSYN; j++) {
		uint8_t acc = 0;
		for (unsigned i = 0; i < 63; i++) {
			if (fixed & (1ULL << i)) {
				acc ^= fec_gf64_alpha(i * (j + 1));
			}
		}
		syn[j] = acc;
		nonzero |= acc;
	}
	if (!nonzero) {
		*data = (uint16_t)(fixed >> 47);
		return 0;
	}

	for (unsigned i = 0; i <= NSYN; i++) {
		lam[i] = 0;
	}
	lam[0] = 1;
	int len = fec_gf64_bm(syn, NSYN, lam, 0);
	if (len < 1 || len > TMAX || lam[len] == 0) {
		return -1;
	}

	// Chien search: bit i is in error when Lambda(a^-i) = 0.
	for (unsigned i = 0; i < 63; i++) {
		uint8_t x = fec_gf64_alpha(FEC_GF64_ORDER - i);
		uint8_t acc = 0;
		for (int q = len; q >= 0; q--) {
			acc = fec_gf64_mul(acc, x) ^ lam[q];
		}
		if (!acc) {
			fixed ^= 1ULL << i;
			nerr++;
		}
	}
	if ((int)nerr != len) {
		return -1;
	}
	// Re-encoding is cheaper than recomputing 22 syndromes and rules out any false locator.
	if (fec_bch63_encode((uint16_t)(fixed >> 47)) != fixed) {
		return -1;
	}
	*data = (uint16_t)(fixed >> 47);
	return (int)nerr;
}
