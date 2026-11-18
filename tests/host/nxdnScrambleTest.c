// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 dB-SPL
#include <stdio.h>
#include <string.h>
#include "nxdn/nxdn_scramble.h"
#include "dmr/ambe/ambeFec.h"

int main(void)
{
	uint8_t bits[49] = { 0 }, before[49];
	uint8_t reg[15], expected[16][49];
	// Starting with S0 alone set, feedback reaches S0 again after 15 shifts.
	static const uint8_t first[20] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 };

	if (!nxdn_scramble_ehr(1, 0, 0, bits) || memcmp(bits, first, sizeof first))
		return puts("FAIL initial sequence"), 1;
	// TS 1-D 7.2.2.1: the first two bytes of its key-1 user-data example.
	{
		const uint8_t plain[2] = { 0x41, 0x42 };
		const uint8_t cipher[2] = { 0xc1, 0x43 };
		for (unsigned i = 0; i < 16; i++)
			if ((((plain[i / 8] ^ cipher[i / 8]) >> (7 - i % 8)) & 1u) != bits[i])
				return puts("FAIL standard vector"), 1;
	}
	if (!nxdn_scramble_ehr(1, 0, 0, bits))
		return 1;
	for (unsigned i = 0; i < sizeof bits; i++)
		if (bits[i])
			return puts("FAIL reversible"), 1;
	// Model the diagram as individual stages and compare all 16 positions, including the positions
	// skipped by the 9600 bit/s control frames.
	for (unsigned i = 0; i < 15; i++)
		reg[i] = (uint8_t)((0x5271u >> i) & 1u);
	for (unsigned p = 0; p < 16; p++) {
		for (unsigned i = 0; i < 49; i++) {
			uint8_t feedback = reg[0] ^ reg[1];

			expected[p][i] = reg[0];
			for (unsigned j = 0; j < 14; j++)
				reg[j] = reg[j + 1];
			reg[14] = feedback;
		}
	}
	for (unsigned p = 0; p < 16; p++) {
		memset(bits, 0, sizeof bits);
		if (!nxdn_scramble_ehr(0x5271, (uint8_t)(p / 4), (uint8_t)(p % 4), bits) ||
		    memcmp(bits, expected[p], sizeof bits))
			return puts("FAIL superframe position"), 1;
	}
	memset(bits, 0, sizeof bits);
	if (!nxdn_scramble_ehr(32767, 3, 3, bits))
		return puts("FAIL maximum key"), 1;
	{
		const uint8_t plain[7] = { 0xfe, 0xe2, 0x12, 0x12, 0x12, 0x10, 0x00 };
		const uint8_t expected_voice[9] = { 0x2a, 0xac, 0xf8, 0xc7, 0xcc, 0xa0, 0x84, 0x74, 0x64 };
		uint8_t voice[9], original[9];

		for (unsigned i = 0; i < 49; i++)
			bits[i] = (uint8_t)((plain[i / 8] >> (7 - i % 8)) & 1u);
		ambeFecEncode(bits, original);
		memcpy(voice, original, sizeof voice);
		if (!nxdn_scramble_vch(1, 0, 0, voice) || memcmp(voice, expected_voice, sizeof voice))
			return puts("FAIL voice vector"), 1;
		if (!nxdn_scramble_vch(1, 0, 0, voice) || memcmp(voice, original, sizeof voice))
			return puts("FAIL voice round trip"), 1;
		voice[0] ^= 0x80;
		if (!nxdn_scramble_vch(1, 0, 0, voice) || memcmp(voice, expected_voice, sizeof voice))
			return puts("FAIL voice FEC correction"), 1;
	}
	memset(bits, 0x5a, sizeof bits);
	memcpy(before, bits, sizeof bits);
	if (nxdn_scramble_ehr(0, 0, 0, bits) || nxdn_scramble_ehr(32768, 0, 0, bits) ||
	    nxdn_scramble_ehr(1, 4, 0, bits) || nxdn_scramble_ehr(1, 0, 4, bits) ||
	    memcmp(bits, before, sizeof bits))
		return puts("FAIL validation"), 1;
	puts("nxdnScrambleTest: passed");
	return 0;
}
