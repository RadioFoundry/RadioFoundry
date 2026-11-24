// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stdio.h>
#include <string.h>
#include "nxdn/nxdn_scramble.h"
#include "dmr/ambe/ambeFec.h"

int main(void)
{
	uint8_t bits[49] = { 0 }, before[49];
	// NXDN TS 1-D v1.3, 7.2.2.1: all 20 bytes of the key-1 UDCH example.
	// Consecutive EHR positions take consecutive 49-bit slices of the same keystream.
	static const uint8_t plain_data[] = "ABCDEFGHIJKLMNOPQRST";
	static const uint8_t scrambled_data[] = {
		0xc1, 0x43, 0x43, 0x42, 0x45, 0x52, 0x47, 0x30, 0x48, 0x5a,
		0x4d, 0x2c, 0x58, 0x0e, 0x30, 0xd1, 0x50, 0x54, 0x55, 0x40,
	};
	for (unsigned pos = 0; pos * sizeof bits < 8 * sizeof scrambled_data; pos++) {
		memset(bits, 0, sizeof bits);
		if (!nxdn_scramble_ehr(1, (uint8_t)(pos / 4), (uint8_t)(pos % 4), bits))
			return 1;
		for (unsigned i = 0; i < sizeof bits; i++) {
			unsigned bit = pos * sizeof bits + i;
			if (bit >= 8 * sizeof scrambled_data)
				break;
			unsigned expected = (plain_data[bit / 8] ^ scrambled_data[bit / 8]) >> (7 - bit % 8);
			if (bits[i] != (expected & 1u))
				return puts("FAIL standard UDCH vector"), 1;
		}
		if (!nxdn_scramble_ehr(1, (uint8_t)(pos / 4), (uint8_t)(pos % 4), bits))
			return 1;
		for (unsigned i = 0; i < sizeof bits; i++)
			if (bits[i])
				return puts("FAIL reversible"), 1;
	}
	memset(bits, 0, sizeof bits);
	if (!nxdn_scramble_ehr(32767, 3, 3, bits))
		return puts("FAIL maximum key"), 1;
	{
		// NXDN TS 1-D v1.3, 7.2.1.1: the EHR 1031 Hz tone with key 1.
		const uint8_t plain[7] = { 0xfe, 0xe2, 0x12, 0x12, 0x12, 0x10, 0x00 };
		static const uint8_t expected_voice[16][9] = {
			{ 0x2a, 0xac, 0xf8, 0xc7, 0xcc, 0xa0, 0x84, 0x74, 0x64 },
			{ 0xec, 0x8e, 0xff, 0xd1, 0x27, 0x58, 0x56, 0xa4, 0x8a },
			{ 0xa0, 0x80, 0xd2, 0x9d, 0xac, 0xe0, 0x81, 0xec, 0x62 },
			{ 0x9f, 0x14, 0xfd, 0xc3, 0x2d, 0x06, 0xb1, 0x64, 0x4d },
			{ 0xb7, 0x7d, 0xa5, 0x30, 0xf7, 0x31, 0x39, 0x0c, 0xc8 },
			{ 0xac, 0xcc, 0x99, 0xf7, 0x53, 0x20, 0xb0, 0xa4, 0x06 },
			{ 0xc1, 0xd0, 0x5f, 0x61, 0x88, 0x84, 0x01, 0x65, 0xef },
			{ 0xf5, 0x0a, 0xfd, 0xc5, 0x3c, 0x4a, 0x55, 0x68, 0x50 },
			{ 0x90, 0x79, 0x62, 0xd4, 0x90, 0x35, 0x32, 0x63, 0xb8 },
			{ 0x95, 0x7b, 0x84, 0x04, 0x78, 0xe8, 0x76, 0xce, 0x6c },
			{ 0xe2, 0x86, 0xa4, 0xe7, 0xce, 0xa4, 0x81, 0x20, 0x2e },
			{ 0x12, 0x8c, 0xbd, 0x81, 0x0b, 0x03, 0xf4, 0xa7, 0xe8 },
			{ 0xf5, 0x5f, 0xf2, 0x1a, 0x19, 0x9d, 0x64, 0x40, 0x20 },
			{ 0x4f, 0x48, 0x9c, 0x91, 0xb0, 0xa7, 0x88, 0x2f, 0xda },
			{ 0xde, 0x27, 0x56, 0xa9, 0xc2, 0x8a, 0x23, 0xcb, 0x8d },
			{ 0x0e, 0x8c, 0xbd, 0x83, 0x5c, 0x2f, 0x1c, 0x36, 0x64 },
		};
		uint8_t voice[9], original[9];

		for (unsigned i = 0; i < 49; i++)
			bits[i] = (uint8_t)((plain[i / 8] >> (7 - i % 8)) & 1u);
		ambeFecEncode(bits, original);
		for (uint8_t pos = 0; pos < 16; pos++) {
			memcpy(voice, original, sizeof voice);
			if (!nxdn_scramble_vch(1, pos / 4, pos % 4, voice) ||
			    memcmp(voice, expected_voice[pos], sizeof voice))
				return puts("FAIL standard EHR vector"), 1;
			if (!nxdn_scramble_vch(1, pos / 4, pos % 4, voice) || memcmp(voice, original, sizeof voice))
				return puts("FAIL voice round trip"), 1;
		}
		voice[0] ^= 0x80;
		if (!nxdn_scramble_vch(1, 0, 0, voice) || memcmp(voice, expected_voice[0], sizeof voice))
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
