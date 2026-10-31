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

// The datasheet's Hz x 2^27 / 6.5 MHz and Hz x 2^21 / 203125 as the working firmwares compute them: x 2.064888 for the
// tone in Hz x 10 and x 10.32444, so their words come out the same (the exact ratios round a few inputs the other way)
#define CTCSS_NUM	258111u		// 2.064888 = 258111 / 125000
#define CTCSS_DEN	125000u
#define TONE_NUM	1290555u	// 10.32444 = 1290555 / 125000
#define TONE_DEN	125000u

uint16_t subaudio_ctcss_word(uint16_t hz_x10, bool round)
{
	return (uint16_t)(((uint64_t)hz_x10 * CTCSS_NUM + (round ? CTCSS_DEN / 2u : 0u)) / CTCSS_DEN);
}

uint16_t subaudio_tone_word(uint16_t hz, bool round)
{
	return (uint16_t)(((uint64_t)hz * TONE_NUM + (round ? TONE_DEN / 2u : 0u)) / TONE_DEN);
}
