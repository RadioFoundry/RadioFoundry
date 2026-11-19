// SPDX-License-Identifier: MIT AND ISC
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2010 DSD Author (BPK_1_TO_4, from DSD-FME include/bp.h, ISC license, see LICENSES/ISC.txt)
/*
 * Host tests for the DMR privacy schemes: voice, and packet data (docs/spec/dmr-data.md 13)
 *
 *   make -C tests/host
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/dmrPrivacy.h"
#include "dmr/dmrData.h"
#include "dmr/ambe/ambeFec.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The firmware's coded silence frame (HR-C6000.c SILENCE_AUDIO, MMDVM's AMBE silence) and its 49 bits (DSD-FME)
static const uint8_t SILENCE_FRAME[9] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };
static const uint64_t SILENCE_BITS = 0xF801A99F8CE080ULL; // 56 bits, the first 49 used

// --- Reference keystreams, as DSD-FME applies them on receive (the behavior, written independently here) ---

// Motorola BP table values for keys 0-4 (third-party data: DSD-FME include/bp.h, ISC, see LICENSES/ISC.txt)
static const uint16_t BPK_1_TO_4[] = { 0x0000, 0x1F00, 0xE300, 0xFC00, 0x2503 };

// Motorola BP: the table value three times over bits 0-47, the first copy with its bits 4-7 cleared; bit 48 stays
static void refMotorolaBP(uint16_t key, uint8_t *bits)
{
	const uint16_t words[3] = { (uint16_t)(BPK_1_TO_4[key] & 0xFF0F), BPK_1_TO_4[key], BPK_1_TO_4[key] };

	for (int w = 0; w < 3; w++)
	{
		for (int b = 0; b < 16; b++)
		{
			bits[(16 * w) + b] ^= (words[w] >> (15 - b)) & 1;
		}
	}
}

// Anytone BP: the key's nibbles, first and third inverted, second and fourth plus 8 (mod 16), as 16 keystream bits
static void refAnytoneKeystream(uint16_t key, uint8_t *ks16)
{
	for (int n = 0; n < 4; n++)
	{
		uint8_t nibble = (key >> (12 - (4 * n))) & 0x0F;

		nibble = ((n & 1) == 0) ? (uint8_t)(~nibble & 0x0F) : (uint8_t)((nibble + 8) & 0x0F);
		for (int b = 0; b < 4; b++)
		{
			ks16[(4 * n) + b] = (nibble >> (3 - b)) & 1;
		}
	}
}

static void randomBits(uint8_t *bits)
{
	for (int i = 0; i < 49; i++)
	{
		bits[i] = rand() & 1;
	}
}

static void burstToBits(const uint8_t *burst, uint8_t bits[3][49])
{
	for (int f = 0; f < 3; f++)
	{
		dmrPrivacyAmbeExtract(&burst[f * 9], bits[f]);
	}
}

static void testFec(void)
{
	uint8_t bits[49], back[49], frame[9];

	for (int i = 0; i < 49; i++)
	{
		bits[i] = (SILENCE_BITS >> (55 - i)) & 1;
	}
	dmrPrivacyAmbeEncode(bits, frame);
	CHECK(memcmp(frame, SILENCE_FRAME, 9) == 0, "silence encodes to %02X%02X%02X%02X%02X%02X%02X%02X%02X", frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7], frame[8]);
	dmrPrivacyAmbeExtract(SILENCE_FRAME, back);
	CHECK(memcmp(back, bits, 49) == 0, "silence frame extracts to the silence bits");

	for (int n = 0; n < 500; n++)
	{

		randomBits(bits);
		dmrPrivacyAmbeEncode(bits, frame);
		dmrPrivacyAmbeExtract(frame, back);
		CHECK(memcmp(back, bits, 49) == 0, "encode/extract round trip %d", n);

		// One error in C0 and three in C1 are corrected by the FEC decode (ambeFec) (on-air bit 4k+j is codeword bit 18j+k)
		uint8_t damaged[9];
		memcpy(damaged, frame, 9);
		int c0Bit = rand() % 23; // C0 stream bits 0..22 (bit 23 is the parity bit, not decoded)
		int onAir = 4 * (c0Bit % 18) + (c0Bit / 18);
		damaged[onAir >> 3] ^= 0x80 >> (onAir & 7);
		for (int e = 0; e < 3; e++)
		{
			int c1Bit = 24 + ((rand() % 7) + (e * 7)); // three distinct C1 stream bits
			onAir = 4 * (c1Bit % 18) + (c1Bit / 18);
			damaged[onAir >> 3] ^= 0x80 >> (onAir & 7);
		}
		ambeFecDecode(damaged, back, NULL);
		CHECK(memcmp(back, bits, 49) == 0, "the FEC decode corrects 1+3 errors %d", n);
	}
}

static void testMotorolaBP(void)
{
	for (uint16_t key = 1; key <= 4; key++)
	{
		uint8_t plain[3][49], burst[27], orig[27], got[3][49];

		for (int f = 0; f < 3; f++)
		{
			randomBits(plain[f]);
			dmrPrivacyAmbeEncode(plain[f], &burst[f * 9]);
		}
		memcpy(orig, burst, 27);

		dmrPrivacyApplyBurst(DMR_PRIVACY_MOTOROLA_BP, key, burst, 4, false);
		burstToBits(burst, got);
		for (int f = 0; f < 3; f++)
		{
			uint8_t expect[49];
			memcpy(expect, plain[f], 49);
			refMotorolaBP(key, expect);
			CHECK(memcmp(got[f], expect, 49) == 0, "Motorola BP key %d frame %d matches DSD-FME", key, f);
		}

		dmrPrivacyApplyBurst(DMR_PRIVACY_MOTOROLA_BP, key, burst, 4, true); // decrypt, through the RX path
		CHECK(memcmp(burst, orig, 27) == 0, "Motorola BP key %d decrypts back", key);
	}
}

static void testAnytoneBP(void)
{
	const uint16_t keys[] = { 0x0000, 0x1234, 0x00F8, 0xFFFF, 0xA5C3 };

	for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
	{
		uint8_t ks[16];
		uint8_t plain[18][49], burst[6][27];
		int counter = 0;

		refAnytoneKeystream(keys[k], ks);

		for (int f = 0; f < 18; f++)
		{
			randomBits(plain[f]);
			if (f == 7)
			{
				for (int i = 0; i < 49; i++)
				{
					plain[f][i] = (SILENCE_BITS >> (55 - i)) & 1; // a silence frame is skipped, but still counts
				}
			}
			dmrPrivacyAmbeEncode(plain[f], &burst[f / 3][(f % 3) * 9]);
		}

		for (int b = 0; b < 6; b++)
		{
			uint8_t got[3][49];

			dmrPrivacyApplyBurst(DMR_PRIVACY_ANYTONE_BP, keys[k], burst[b], b, false);
			burstToBits(burst[b], got);

			for (int i = 0; i < 3; i++)
			{
				int f = b * 3 + i;
				uint8_t expect[49];
				memcpy(expect, plain[f], 49);
				if (f == 7)
				{
					counter += 49;
				}
				else
				{
					for (int j = 0; j < 49; j++)
					{
						expect[j] ^= ks[(counter++) % 16];
					}
				}
				CHECK(memcmp(got[i], expect, 49) == 0, "Anytone BP key %04X frame %d matches DSD-FME", keys[k], f);
			}

			dmrPrivacyApplyBurst(DMR_PRIVACY_ANYTONE_BP, keys[k], burst[b], b, true);
			uint8_t back[3][49];
			burstToBits(burst[b], back);
			for (int i = 0; i < 3; i++)
			{
				CHECK(memcmp(back[i], plain[b * 3 + i], 49) == 0, "Anytone BP key %04X frame %d decrypts back", keys[k], b * 3 + i);
			}
		}
	}
}

static void testSignaling(void)
{
	uint8_t lc[12] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x98, 0x88, 0xFF, 0, 0, 0 };

	CHECK(dmrPrivacyLCIsPrivate(lc) == false, "clear LC");
	lc[1] = dmrPrivacyFID(DMR_PRIVACY_MOTOROLA_BP, 0);
	lc[2] = dmrPrivacyServiceOptions(DMR_PRIVACY_MOTOROLA_BP, 0);
	CHECK((lc[1] == 0x10) && (lc[2] == 0x40) && dmrPrivacyLCIsPrivate(lc), "private LC");
	CHECK((dmrPrivacyFID(DMR_PRIVACY_OFF, 0) == 0) && (dmrPrivacyServiceOptions(DMR_PRIVACY_OFF, 0) == 0), "clear when off");
	CHECK(dmrPrivacyKeyIsValid(DMR_PRIVACY_MOTOROLA_BP, 0) == false && dmrPrivacyKeyIsValid(DMR_PRIVACY_MOTOROLA_BP, 255), "BP key range");

	uint8_t burst[27], orig[27];
	for (int i = 0; i < 27; i++)
	{
		burst[i] = rand();
	}
	memcpy(orig, burst, 27);
	dmrPrivacyApplyBurst(DMR_PRIVACY_OFF, 1, burst, 0, false);
	dmrPrivacyApplyBurst(DMR_PRIVACY_MOTOROLA_BP, 1, burst, 6, false); // not a voice burst
	CHECK(memcmp(burst, orig, 27) == 0, "untouched when off or not a voice burst");
}


// Packet data: what each scheme does with it, and the Motorola BP keystream (the key's table value repeated over the
// octets, high octet first, from the first user data octet)
static void testData(void)
{
	dmrDataPrivacy_t d;
	dmrPrivacyKey_t moto = { DMR_PRIVACY_MOTOROLA_BP, 4 }, anytone = { DMR_PRIVACY_ANYTONE_BP, 0x1234 };
	dmrPrivacyKey_t off = { DMR_PRIVACY_OFF, 0 }, bad = { DMR_PRIVACY_MOTOROLA_BP, 0 };

	dmrPrivacyDataConfig(&moto, &d);
	CHECK(d.mode == DMR_DATA_PRIVACY_ENCRYPT && d.alg == DMR_DATA_ALG_BASIC && d.crypt != NULL && d.ctx == &moto,
	      "Moto BP encrypts data");
	dmrPrivacyDataConfig(&anytone, &d);
	CHECK(d.mode == DMR_DATA_PRIVACY_BLOCK && d.crypt == NULL, "Anytone BP has no data form: blocked");
	dmrPrivacyDataConfig(&off, &d);
	CHECK(d.mode == DMR_DATA_PRIVACY_CLEAR && d.crypt == NULL, "off: clear");
	dmrPrivacyDataConfig(&bad, &d);
	CHECK(d.mode == DMR_DATA_PRIVACY_CLEAR, "an invalid key: clear");
	dmrPrivacyDataConfig(NULL, &d);
	CHECK(d.mode == DMR_DATA_PRIVACY_CLEAR, "no key: clear");

	// An IPv4 header's first octets under key 4 (0x2503), and back
	uint8_t data[7] = { 0x45, 0x00, 0x00, 0x1C, 0x12, 0x34, 0x40 };
	const uint8_t want[7] = { 0x45 ^ 0x25, 0x00 ^ 0x03, 0x00 ^ 0x25, 0x1C ^ 0x03, 0x12 ^ 0x25, 0x34 ^ 0x03, 0x40 ^ 0x25 };
	CHECK(dmrPrivacyDataCrypt(&moto, DMR_DATA_ALG_BASIC, 0, data, 7) && memcmp(data, want, 7) == 0, "BP data keystream");
	CHECK(dmrPrivacyDataCrypt(&moto, DMR_DATA_ALG_BASIC, 0, data, 7) && data[0] == 0x45 && data[6] == 0x40,
	      "the same XOR decrypts");
	// Each key 1-4 is its table value, whatever the message indicator
	for (uint16_t key = 1; key <= 4; key++)
	{
		dmrPrivacyKey_t k = { DMR_PRIVACY_MOTOROLA_BP, key };
		uint8_t z[4] = { 0, 0, 0, 0 };

		CHECK(dmrPrivacyDataCrypt(&k, DMR_DATA_ALG_BASIC, 0x12345678, z, 4) && z[0] == (BPK_1_TO_4[key] >> 8) &&
		      z[1] == (BPK_1_TO_4[key] & 0xFF) && z[2] == z[0] && z[3] == z[1], "key %u", key);
	}
	// Other algorithms, schemes without a data form and invalid keys leave the data alone
	memcpy(data, want, 7);
	CHECK(!dmrPrivacyDataCrypt(&moto, DMR_DATA_ALG_RC4, 0, data, 7) && memcmp(data, want, 7) == 0, "RC4 isn't BP");
	CHECK(!dmrPrivacyDataCrypt(&anytone, DMR_DATA_ALG_BASIC, 0, data, 7) && memcmp(data, want, 7) == 0, "Anytone BP");
	CHECK(!dmrPrivacyDataCrypt(&bad, DMR_DATA_ALG_BASIC, 0, data, 7) && !dmrPrivacyDataCrypt(NULL, 0, 0, data, 7),
	      "no valid key");
	// The key ranges and names come from the same table
	CHECK(dmrPrivacyKeyIsValid(DMR_PRIVACY_MOTOROLA_BP, 255) && !dmrPrivacyKeyIsValid(DMR_PRIVACY_MOTOROLA_BP, 256) &&
	      dmrPrivacyKeyIsValid(DMR_PRIVACY_ANYTONE_BP, 0) && !dmrPrivacyKeyIsValid(DMR_PRIVACY_OFF, 0) &&
	      !dmrPrivacyKeyIsValid(DMR_PRIVACY_NUM_TYPES, 1) && !dmrPrivacyKeyIsValid(DMR_PRIVACY_NUM_TYPES, 0),
	      "key ranges");
	CHECK(dmrPrivacyKeyMax(DMR_PRIVACY_MOTOROLA_BP) == 255 && dmrPrivacyKeyMax(DMR_PRIVACY_ANYTONE_BP) == 0xFFFF,
	      "key max");
	CHECK(strcmp(dmrPrivacyTypeName(DMR_PRIVACY_MOTOROLA_BP), "Moto BP") == 0 &&
	      strcmp(dmrPrivacyTypeName(DMR_PRIVACY_NUM_TYPES), "Off") == 0, "names");
}

int main(void)
{
	srand(1);
	testFec();
	testMotorolaBP();
	testAnytoneBP();
	testSignaling();
	testData();

	printf("# %s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
