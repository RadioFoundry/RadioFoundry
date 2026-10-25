// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Sub-audio math shared by the RF chip drivers
#include "subaudio.h"

#define DCS_GOLAY_POLY	0xC75u	// x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1
#define DCS_MARKER	0x800u	// word bits 11:9 = 100

uint32_t subaudio_dcs_word(uint16_t code)
{
	uint32_t data = DCS_MARKER | (code & 0x1FFu);
	uint32_t rem = data << 11;
	for (int bit = 22; bit >= 11; bit--) {
		if (rem & (1u << bit)) {
			rem ^= DCS_GOLAY_POLY << (bit - 11);
		}
	}
	return (rem << 12) | data;
}
