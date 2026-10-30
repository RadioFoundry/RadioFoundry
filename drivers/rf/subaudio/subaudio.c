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

#define CTCSS_NUM	134217728u	// 2^27, over 6.5 MHz x 10 (the input is Hz x 10)
#define CTCSS_DEN	65000000u
#define TONE_NUM	2097152u	// 2^21
#define TONE_DEN	203125u

uint16_t subaudio_ctcss_word(uint16_t hz_x10, bool round)
{
	return (uint16_t)(((uint64_t)hz_x10 * CTCSS_NUM + (round ? CTCSS_DEN / 2u : 0u)) / CTCSS_DEN);
}

uint16_t subaudio_tone_word(uint16_t hz, bool round)
{
	return (uint16_t)(((uint64_t)hz * TONE_NUM + (round ? TONE_DEN / 2u : 0u)) / TONE_DEN);
}
