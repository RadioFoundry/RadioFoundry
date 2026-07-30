// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Host test for core/dmr/fec: Hamming, BPTC(196,96), Golay(20,8) and the slot type
 *
 *   make -C tests/host
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/fec/dmrFec.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static int popcount(uint32_t v)
{
	int n = 0;

	for (; v; v &= v - 1)
	{
		n++;
	}
	return n;
}

static void testHamming(void)
{
	int bad = 0;

	for (uint16_t d = 0; d < 2048; d++)
	{
		uint16_t cw = dmrFecHamming15113Encode(d), t = cw;

		bad += !dmrFecHamming15113Correct(&t) || (t != cw) || ((cw >> 4) != d);
		for (int b = 0; b < 15; b++)
		{
			t = cw ^ (uint16_t)(1u << b);
			bad += !dmrFecHamming15113Correct(&t) || (t != cw);
		}
	}
	CHECK(bad == 0, "Hamming(15,11,3): %d single errors not corrected", bad);

	bad = 0;
	for (uint16_t d = 0; d < 512; d++)
	{
		uint16_t cw = dmrFecHamming1393Encode(d), t = cw;

		bad += !dmrFecHamming1393Correct(&t) || (t != cw) || ((cw >> 4) != d);
		for (int b = 0; b < 13; b++)
		{
			t = cw ^ (uint16_t)(1u << b);
			bad += !dmrFecHamming1393Correct(&t) || (t != cw);
		}
	}
	CHECK(bad == 0, "Hamming(13,9,3): %d single errors not corrected", bad);
}

static void testGolay(void)
{
	int minDistance = 99;

	for (int a = 0; a < 256; a++)
	{
		uint32_t ca = dmrFecGolay2087Encode((uint8_t)a);

		CHECK((ca >> 12) == (uint32_t)a, "Golay(20,8) %02X: systematic", a);
		for (int b = a + 1; b < 256; b++)
		{
			int d = popcount(ca ^ dmrFecGolay2087Encode((uint8_t)b));

			minDistance = (d < minDistance) ? d : minDistance;
		}
	}
	CHECK(minDistance == 8, "Golay(20,8) minimum distance %d", minDistance);

	// Every pattern of up to 3 errors on a few codewords
	int bad = 0;
	for (int a = 0; a < 256; a += 37)
	{
		uint32_t cw = dmrFecGolay2087Encode((uint8_t)a);
		uint8_t out;

		for (int i = 0; i < 20; i++)
		{
			for (int j = i; j < 20; j++)
			{
				for (int k = j; k < 20; k++)
				{
					uint32_t e = (1u << i) ^ ((j != i) ? (1u << j) : 0) ^ ((k != j) ? (1u << k) : 0);

					bad += (dmrFecGolay2087Decode(cw ^ e, &out) < 0) || (out != a);
				}
			}
		}
	}
	CHECK(bad == 0, "Golay(20,8): %d patterns of up to 3 errors not corrected", bad);
}

static void testSlotType(void)
{
	uint8_t burst[33], cc, dt;

	memset(burst, 0x5A, sizeof(burst));
	dmrFecSlotTypeEncode(2, 0x2, burst);
	CHECK(((burst[12] >> 2) & 0x0F) == 2, "colour code in bits 98-101");
	CHECK((((burst[12] & 0x03) << 2) | (burst[13] >> 6)) == 0x2, "data type in bits 102-105");
	CHECK((burst[13] & 0x0F) == 0x0A, "sync bits 108-111 untouched");
	CHECK((burst[19] & 0xF0) == 0x50, "sync bits 152-155 untouched");
	CHECK(dmrFecSlotTypeDecode(burst, &cc, &dt) && (cc == 2) && (dt == 0x2), "decode");

	for (int c = 0; c < 16; c++)
	{
		for (int t = 0; t < 16; t++)
		{
			dmrFecSlotTypeEncode((uint8_t)c, (uint8_t)t, burst);
			burst[12] ^= 0x40;// one error in the first half
			burst[20] ^= 0x10;// and one in the second
			CHECK(dmrFecSlotTypeDecode(burst, &cc, &dt) && (cc == c) && (dt == t), "CC %d DT %d with 2 errors", c, t);
		}
	}
}

// The info bit positions in the burst: 0-97 and 166-263
static int infoBit(int i)
{
	return (i < 98) ? i : (i + 68);
}

static void flip(uint8_t *burst, int n)
{
	burst[n >> 3] ^= (uint8_t)(0x80 >> (n & 7));
}

static void testBptc(void)
{
	// ETSI TS 102 361-1 figure D.1: the idle message's info bits
	static const uint8_t IDLE[12] = { 0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xCF, 0xCD, 0x8A, 0x41 };
	uint8_t burst[33], back[12], payload[12];

	memset(burst, 0xA5, sizeof(burst));
	dmrFecBptcEncode(IDLE, burst);
	CHECK((burst[12] & 0x3F) == (0xA5 & 0x3F) && burst[14] == 0xA5 && (burst[20] & 0xFC) == (0xA5 & 0xFC),
		  "BPTC leaves the slot type and sync alone");
	CHECK((dmrFecBptcDecode(burst, back) == 0) && (memcmp(back, IDLE, 12) == 0), "idle round trip");

	srand(1);
	int bad = 0;
	for (int round = 0; round < 200; round++)
	{
		for (int i = 0; i < 12; i++)
		{
			payload[i] = (uint8_t)rand();
		}
		dmrFecBptcEncode(payload, burst);

		// Any single error
		for (int i = 0; i < 196; i++)
		{
			uint8_t b2[33];

			memcpy(b2, burst, 33);
			flip(b2, infoBit(i));
			// info bit 0 is matrix index 0, R(3): in no row or column, so nothing to correct
			int r = dmrFecBptcDecode(b2, back);
			if ((r != ((i == 0) ? 0 : 1)) || (memcmp(back, payload, 12) != 0))
			{
				printf("single error at %d: %d\n", i, r);
				bad++;
			}
		}

		// Two random errors
		int i = rand() % 196, j = rand() % 196;
		if (i != j)
		{
			uint8_t b2[33];

			memcpy(b2, burst, 33);
			flip(b2, infoBit(i));
			flip(b2, infoBit(j));
			int r = dmrFecBptcDecode(b2, back);
			if ((r < 0) || (memcmp(back, payload, 12) != 0))
			{
				printf("errors at %d and %d: %d\n", i, j, r);
				bad++;
			}
		}
	}
	CHECK(bad == 0, "BPTC: %d error patterns not corrected", bad);
}

int main(void)
{
	testHamming();
	testGolay();
	testSlotType();
	testBptc();

	printf("# dmrFecTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
