// SPDX-License-Identifier: MIT AND ISC
// Copyright (c) 2026 The OpenDigitalRadio Authors
// SPDX-FileCopyrightText: 2010 DSD Author (MOTOROLA_BP_KEYS, from DSD-FME include/bp.h, ISC license, see LICENSES/ISC.txt)
/*
 * DMR voice privacy (static keystream "basic privacy" schemes)
 *
 * The keystreams follow DSD-FME (github.com/lwvmobile/dsd-fme), which decodes them on air; the Motorola Basic Privacy
 * key table is DSD-FME's include/bp.h:
 *
 *   Copyright (C) 2010 DSD Author
 *   Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
 *   granted, provided that the above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 *   WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 *   CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 *   OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
 *   THIS SOFTWARE.
 */

#include <string.h>
#include "dmr/dmrPrivacy.h"

#define FRAMES_PER_SUPERFRAME  18

// Motorola Basic Privacy: key number -> 16-bit keystream seed. This table is third-party data: DSD-FME's include/bp.h
// (github.com/lwvmobile/dsd-fme), Copyright (C) 2010 DSD Author, ISC license (above and LICENSES/ISC.txt).
static const uint16_t MOTOROLA_BP_KEYS[256] = {
	0x0000, 0x1F00, 0xE300, 0xFC00, 0x2503, 0x3A03, 0xC603, 0xD903,
	0x4A05, 0x5505, 0xA905, 0xB605, 0x6F06, 0x7006, 0x8C06, 0x9306,
	0x2618, 0x3918, 0xC518, 0xDA18, 0x031B, 0x1C1B, 0xE01B, 0xFF1B,
	0x6C1D, 0x731D, 0x8F1D, 0x901D, 0x491E, 0x561E, 0xAA1E, 0xB51E,
	0x4B28, 0x5428, 0xA828, 0xB728, 0x6E2B, 0x712B, 0x8D2B, 0x922B,
	0x012D, 0x1E2D, 0xE22D, 0xFD2D, 0x242E, 0x3B2E, 0xC72E, 0xD82E,
	0x6D30, 0x7230, 0x8E30, 0x9130, 0x4833, 0x5733, 0xAB33, 0xB433,
	0x2735, 0x3835, 0xC435, 0xDB35, 0x0236, 0x1D36, 0xE136, 0xFE36,
	0x2B49, 0x3449, 0xC849, 0xD749, 0x0E4A, 0x114A, 0xED4A, 0xF24A,
	0x614C, 0x7E4C, 0x824C, 0x9D4C, 0x444F, 0x5B4F, 0xA74F, 0xB84F,
	0x0D51, 0x1251, 0xEE51, 0xF151, 0x2852, 0x3752, 0xCB52, 0xD452,
	0x4754, 0x5854, 0xA454, 0xBB54, 0x6257, 0x7D57, 0x8157, 0x9E57,
	0x6061, 0x7F61, 0x8361, 0x9C61, 0x4562, 0x5A62, 0xA662, 0xB962,
	0x2A64, 0x3564, 0xC964, 0xD664, 0x0F67, 0x1067, 0xEC67, 0xF367,
	0x4679, 0x5979, 0xA579, 0xBA79, 0x637A, 0x7C7A, 0x807A, 0x9F7A,
	0x0C7C, 0x137C, 0xEF7C, 0xF07C, 0x297F, 0x367F, 0xCA7F, 0xD57F,
	0x4D89, 0x5289, 0xAE89, 0xB189, 0x688A, 0x778A, 0x8B8A, 0x948A,
	0x078C, 0x188C, 0xE48C, 0xFB8C, 0x228F, 0x3D8F, 0xC18F, 0xDE8F,
	0x6B91, 0x7491, 0x8891, 0x9791, 0x4E92, 0x5192, 0xAD92, 0xB292,
	0x2194, 0x3E94, 0xC294, 0xDD94, 0x0497, 0x1B97, 0xE797, 0xF897,
	0x06A1, 0x19A1, 0xE5A1, 0xFAA1, 0x23A2, 0x3CA2, 0xC0A2, 0xDFA2,
	0x4CA4, 0x53A4, 0xAFA4, 0xB0A4, 0x69A7, 0x76A7, 0x8AA7, 0x95A7,
	0x20B9, 0x3FB9, 0xC3B9, 0xDCB9, 0x05BA, 0x1ABA, 0xE6BA, 0xF9BA,
	0x6ABC, 0x75BC, 0x89BC, 0x96BC, 0x4FBF, 0x50BF, 0xACBF, 0xB3BF,
	0x66C0, 0x79C0, 0x85C0, 0x9AC0, 0x43C3, 0x5CC3, 0xA0C3, 0xBFC3,
	0x2CC5, 0x33C5, 0xCFC5, 0xD0C5, 0x09C6, 0x16C6, 0xEAC6, 0xF5C6,
	0x84D0, 0x85DF, 0x8AD3, 0x8BDC, 0xB6D5, 0xB7DA, 0xB8D6, 0xB9D9,
	0xD0DA, 0xD1D5, 0xDED9, 0xDFD6, 0xE2DF, 0xE3D0, 0xECDC, 0xEDD3,
	0x2DE8, 0x32E8, 0xCEE8, 0xD1E8, 0x08EB, 0x17EB, 0xEBEB, 0xF4EB,
	0x67ED, 0x78ED, 0x84ED, 0x9BED, 0x42EE, 0x5DEE, 0xA1EE, 0xBEEE,
	0x0BF0, 0x14F0, 0xE8F0, 0xF7F0, 0x2EF3, 0x31F3, 0xCDF3, 0xD2F3,
	0x41F5, 0x5EF5, 0xA2F5, 0xBDF5, 0x64F6, 0x7BF6, 0x87F6, 0x98F6,
};

// The AMBE+2 silence frame, which Anytone leaves in the clear (DSD-FME dsd_mbe.c)
static const uint8_t AMBE_SILENCE[7] = { 0xF8, 0x01, 0xA9, 0x9F, 0x8C, 0xE0, 0x80 };

bool dmrPrivacyKeyIsValid(dmrPrivacyType_t type, uint16_t key)
{
	switch (type)
	{
		case DMR_PRIVACY_MOTOROLA_BP:
			return ((key >= 1) && (key <= 255));
		case DMR_PRIVACY_ANYTONE_BP:
			return true;
		default:
			return false;
	}
}

uint16_t dmrPrivacyKeyMax(dmrPrivacyType_t type)
{
	return ((type == DMR_PRIVACY_ANYTONE_BP) ? 0xFFFF : 255);
}

const char *dmrPrivacyTypeName(dmrPrivacyType_t type)
{
	switch (type)
	{
		case DMR_PRIVACY_MOTOROLA_BP:
			return "Moto BP";
		case DMR_PRIVACY_ANYTONE_BP:
			return "Anytone BP";
		default:
			return "Off";
	}
}

uint8_t dmrPrivacyServiceOptions(dmrPrivacyType_t type, uint8_t serviceOptions)
{
	return ((type != DMR_PRIVACY_OFF) ? (serviceOptions | DMR_PRIVACY_SERVICE_OPTION) : serviceOptions);
}

uint8_t dmrPrivacyFID(dmrPrivacyType_t type, uint8_t fid)
{
	return ((type != DMR_PRIVACY_OFF) ? DMR_PRIVACY_FID : fid);
}

bool dmrPrivacyLCIsPrivate(const uint8_t *lc)
{
	return ((lc[2] & DMR_PRIVACY_SERVICE_OPTION) != 0);
}

// Golay(23,12) codeword, generator 0xC75, data in the top 12 bits
static uint32_t golay23(uint32_t data)
{
	uint32_t r = data << 11;

	for (int i = 22; i >= 11; i--)
	{
		if (r & (1U << i))
		{
			r ^= (0xC75U << (i - 11));
		}
	}

	return ((data << 11) | r);
}

static uint32_t parity(uint32_t v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	v ^= v >> 2;
	v ^= v >> 1;

	return (v & 1);
}

// The pseudo random sequence XORed onto C1, seeded with the C0 data u0, MSB of C1 first
static uint32_t c1Scramble(uint32_t u0)
{
	uint32_t p = u0 << 4;
	uint32_t mask = 0;

	for (int i = 0; i < 23; i++)
	{
		p = ((173 * p) + 13849) & 0xFFFF;
		mask = (mask << 1) | (p >> 15);
	}

	return mask;
}

static uint32_t bitsToWord(const uint8_t *bits, int n)
{
	uint32_t v = 0;

	for (int i = 0; i < n; i++)
	{
		v = (v << 1) | (bits[i] & 1);
	}

	return v;
}

static void wordToBits(uint32_t v, int n, uint8_t *bits)
{
	for (int i = 0; i < n; i++)
	{
		bits[i] = (v >> (n - 1 - i)) & 1;
	}
}

// On-air bit 4k+j is bit 18j+k of C0 | C1 | C2 | C3 (MSB first)
static int interleaveIndex(int n)
{
	return ((18 * (n & 3)) + (n >> 2));
}

void dmrPrivacyAmbeEncode(const uint8_t *bits, uint8_t *frame)
{
	uint8_t s[72];
	uint32_t u0 = bitsToWord(&bits[0], 12);
	uint32_t c0 = (golay23(u0) << 1);

	c0 |= parity(c0);
	wordToBits(c0, 24, &s[0]);
	wordToBits(golay23(bitsToWord(&bits[12], 12)) ^ c1Scramble(u0), 23, &s[24]);
	memcpy(&s[47], &bits[24], 25); // C2 and C3 are sent without FEC

	memset(frame, 0, DMR_PRIVACY_AMBE_FRAME_BYTES);

	for (int n = 0; n < 72; n++)
	{
		if (s[interleaveIndex(n)] & 1)
		{
			frame[n >> 3] |= (0x80 >> (n & 7));
		}
	}
}

void dmrPrivacyAmbeExtract(const uint8_t *frame, uint8_t *bits)
{
	uint8_t s[72];

	for (int n = 0; n < 72; n++)
	{
		s[interleaveIndex(n)] = (frame[n >> 3] >> (7 - (n & 7))) & 1;
	}

	uint32_t u0 = bitsToWord(&s[0], 12);

	memcpy(&bits[0], &s[0], 12);
	wordToBits((bitsToWord(&s[24], 23) ^ c1Scramble(u0)) >> 11, 12, &bits[12]);
	memcpy(&bits[24], &s[47], 25);
}

static bool frameIsClearForAnytone(const uint8_t *bits)
{
	bool isSilence = true;
	bool isZeroed = true;

	for (int i = 0; i < DMR_PRIVACY_AMBE_FRAME_BITS; i++)
	{
		if (bits[i] != ((AMBE_SILENCE[i >> 3] >> (7 - (i & 7))) & 1))
		{
			isSilence = false;
		}

		if ((i >= 24) && (i < 44) && bits[i])
		{
			isZeroed = false;
		}
	}

	return (isSilence || isZeroed);
}

static void applyFrame(dmrPrivacyType_t type, uint16_t key, uint8_t *bits, int frameIndex)
{
	switch (type)
	{
		case DMR_PRIVACY_MOTOROLA_BP:
			{
				uint64_t k = MOTOROLA_BP_KEYS[key & 0xFF];

				k = ((k & 0xFF0F) << 32) | (k << 16) | k; // 48 bits, the last parameter bit stays clear

				for (int i = 0; i < 48; i++)
				{
					bits[i] ^= (k >> (47 - i)) & 1;
				}
			}
			break;

		case DMR_PRIVACY_ANYTONE_BP:
			if (frameIsClearForAnytone(bits) == false)
			{
				// Nibbles 1 and 3 inverted, 2 and 4 plus 8, the 16 bits repeated through the superframe
				uint16_t k = (~key & 0xF0F0) | ((((key >> 8) + 8) & 0x0F) << 8) | ((key + 8) & 0x0F);
				int position = frameIndex * DMR_PRIVACY_AMBE_FRAME_BITS;

				for (int i = 0; i < DMR_PRIVACY_AMBE_FRAME_BITS; i++, position++)
				{
					bits[i] ^= (k >> (15 - (position & 15))) & 1;
				}
			}
			break;

		default:
			break;
	}
}

void dmrPrivacyApplyBurst(dmrPrivacyType_t type, uint16_t key, uint8_t *ambe, uint8_t voiceSequence, bool correctErrors)
{
	if ((type == DMR_PRIVACY_OFF) || (type >= DMR_PRIVACY_NUM_TYPES) || (voiceSequence >= (FRAMES_PER_SUPERFRAME / DMR_PRIVACY_AMBE_BURST_FRAMES)))
	{
		return;
	}

	for (int f = 0; f < DMR_PRIVACY_AMBE_BURST_FRAMES; f++)
	{
		uint8_t *frame = &ambe[f * DMR_PRIVACY_AMBE_FRAME_BYTES];
		uint8_t bits[DMR_PRIVACY_AMBE_FRAME_BITS];

		if (correctErrors)
		{
			uint16_t decoded[DMR_PRIVACY_AMBE_FRAME_BITS];

			initFrame(frame, decoded);

			for (int i = 0; i < DMR_PRIVACY_AMBE_FRAME_BITS; i++)
			{
				bits[i] = decoded[i] & 1;
			}
		}
		else
		{
			dmrPrivacyAmbeExtract(frame, bits);
		}

		applyFrame(type, key, bits, (voiceSequence * DMR_PRIVACY_AMBE_BURST_FRAMES) + f);
		dmrPrivacyAmbeEncode(bits, frame);
	}
}
