// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

#include "hamming.h"

// Parity parts of the generator-matrix rows, top data bit first (TIA-102.BAAA-A Table 5-4).
static const uint8_t par15[11] = {
	0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x7, 0x6, 0x5, 0x3,
};
static const uint8_t par10[6] = {
	0xE, 0xD, 0xB, 0x7, 0x3, 0xC,
};

static unsigned parity_of(const uint8_t *tab, unsigned k, unsigned data)
{
	unsigned p = 0;
	for (unsigned j = 0; j < k; j++) {
		if (data & (1u << (k - 1 - j))) {
			p ^= tab[j];
		}
	}
	return p;
}

// The syndrome of a single error is that bit's column: a data row's parity part, or one bit
// of the identity for a parity bit.
static bool correct(const uint8_t *tab, unsigned k, uint16_t *cw)
{
	unsigned data = (*cw >> 4) & ((1u << k) - 1);
	unsigned s = parity_of(tab, k, data) ^ (*cw & 0xF);

	if (!s) {
		return true;
	}
	if (!(s & (s - 1))) {
		*cw ^= (uint16_t)s;	// one bit in the parity field
		return true;
	}
	for (unsigned j = 0; j < k; j++) {
		if (tab[j] == s) {
			*cw ^= (uint16_t)(0x10u << (k - 1 - j));
			return true;
		}
	}
	return false;
}

uint16_t fec_hamming15_encode(uint16_t data11)
{
	data11 &= 0x7FF;
	return (uint16_t)((data11 << 4) | parity_of(par15, 11, data11));
}

bool fec_hamming15_correct(uint16_t *cw)
{
	return correct(par15, 11, cw);
}

uint16_t fec_hamming10_encode(uint8_t data6)
{
	data6 &= 0x3F;
	return (uint16_t)((data6 << 4) | parity_of(par10, 6, data6));
}

bool fec_hamming10_correct(uint16_t *cw)
{
	return correct(par10, 6, cw);
}
