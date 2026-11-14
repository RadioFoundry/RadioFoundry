// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host test for core/dmr/fec: Hamming, BPTC(196,96), Golay(20,8), the slot type, the EMB's QR(16,7,6) and the
 * reverse channel
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
	CHECK(((burst[12] >> 2) & 0x0F) == 2, "color code in bits 98-101");
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
	// ETSI TS 102 361-1 table D.2: the idle message's info bits, I(95) first
	static const uint8_t IDLE[12] = { 0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91 };
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

// QR(16,7,6), ETSI TS 102 361-1 B.3.2. The code is checked against an independent construction: the 15 bit words
// m(x) g(x) for all m of degree below 7 (non-systematic, by multiplication rather than division), each with an even
// parity bit, must be exactly the encoder's codeword set
static void testQr1676(void)
{
	static uint8_t isCodeword[65536 / 8];
	int bad = 0;

	// Known codewords: data 1, 2, 4 and 7 (the generator rows and a sum of them)
	CHECK(dmrFecQr1676Encode(1) == 0x0273, "QR(16,7,6) 1 -> %04X", dmrFecQr1676Encode(1));
	CHECK(dmrFecQr1676Encode(2) == 0x04E5, "QR(16,7,6) 2 -> %04X", dmrFecQr1676Encode(2));
	CHECK(dmrFecQr1676Encode(4) == 0x09C9, "QR(16,7,6) 4 -> %04X", dmrFecQr1676Encode(4));
	CHECK(dmrFecQr1676Encode(7) == 0x0F5F, "QR(16,7,6) 7 -> %04X", dmrFecQr1676Encode(7));
	CHECK(dmrFecQr1676Encode(0) == 0x0000, "QR(16,7,6) 0");

	memset(isCodeword, 0, sizeof(isCodeword));
	for (uint32_t msg = 0; msg < 128; msg++)
	{
		uint32_t w = 0;

		for (int i = 0; i < 7; i++)
		{
			if (msg & (1u << i))
			{
				w ^= 0x139u << i;	// g(x) = x^8 + x^5 + x^4 + x^3 + 1
			}
		}
		w = (w << 1) | (uint32_t)(popcount(w) & 1);
		isCodeword[w >> 3] |= (uint8_t)(1u << (w & 7));
	}
	for (uint16_t d = 0; d < 128; d++)
	{
		uint16_t cw = dmrFecQr1676Encode((uint8_t)d);
		uint8_t out = 0xFF;

		bad += !(isCodeword[cw >> 3] & (1u << (cw & 7))) || ((cw >> 9) != d);
		bad += (dmrFecQr1676Decode(cw, &out) != 0) || (out != d);
		for (uint16_t e = d + 1; e < 128; e++)
		{
			bad += popcount((uint32_t)(cw ^ dmrFecQr1676Encode((uint8_t)e))) < 6;
		}
	}
	CHECK(bad == 0, "QR(16,7,6): %d codewords wrong or closer than 6", bad);

	// Every 1 and 2 bit error corrected, every 3 bit error detected
	bad = 0;
	for (uint16_t d = 0; d < 128; d += 5)
	{
		uint16_t cw = dmrFecQr1676Encode((uint8_t)d);

		for (int a = 0; a < 16; a++)
		{
			for (int b = a; b < 16; b++)
			{
				uint8_t out = 0xFF;
				uint16_t e = (uint16_t)(cw ^ (1u << a) ^ ((a != b) ? (1u << b) : 0));

				bad += (dmrFecQr1676Decode(e, &out) != ((a != b) ? 2 : 1)) || (out != d);
				for (int c = b + 1; (a != b) && (c < 16); c++)
				{
					bad += dmrFecQr1676Decode((uint16_t)(e ^ (1u << c)), &out) != -1;
				}
			}
		}
	}
	CHECK(bad == 0, "QR(16,7,6): %d error patterns mishandled", bad);

	// EMB: color code 1, PI 0, LCSS first (data 0001 0 01 = 9); round trip of every field combination
	CHECK(dmrFecEmbEncode(1, false, 1) == (dmrFecQr1676Encode(8) ^ dmrFecQr1676Encode(1)), "EMB CC1 LCSS1");
	bad = 0;
	for (uint8_t cc = 0; cc < 16; cc++)
	{
		for (uint8_t lcss = 0; lcss < 4; lcss++)
		{
			uint8_t c2, l2;
			bool pi;
			uint16_t emb = dmrFecEmbEncode(cc, (lcss & 1) != 0, lcss);

			bad += (dmrFecEmbDecode((uint16_t)(emb ^ 0x0101), &c2, &pi, &l2) != 2) || (c2 != cc) || (l2 != lcss) ||
				   (pi != ((lcss & 1) != 0));
		}
	}
	CHECK(bad == 0, "EMB: %d round trips wrong", bad);
}

static void testReverseChannel(void)
{
	// RC info by hand from B.3.13: x^7 = x^5 + x^2 + x + 1 mod G7, so cease (0100, x^9) has CRC 0111011 (0x3B),
	// 0x41 masked; cease request (0101, x^9 + x^7) 0011100 (0x1C), 0x66 masked; increase power (0000) 0, 0x7A masked
	CHECK(dmrFecRcInfo(DMR_RC_CEASE) == 0x241, "RC info cease %03X", dmrFecRcInfo(DMR_RC_CEASE));
	CHECK(dmrFecRcInfo(DMR_RC_CEASE_REQUEST) == 0x2E6, "RC info cease request %03X", dmrFecRcInfo(DMR_RC_CEASE_REQUEST));
	CHECK(dmrFecRcInfo(DMR_RC_POWER_UP) == 0x07A, "RC info power up %03X", dmrFecRcInfo(DMR_RC_POWER_UP));

	int bad = 0, accepted = 0;

	for (uint16_t info = 0; info < 2048; info++)
	{
		int c = dmrFecRcInfoDecode(info);

		accepted += c >= 0;
		bad += c >= 0 && dmrFecRcInfo((uint8_t)c) != info;
	}
	CHECK(bad == 0 && accepted == 16, "RC info: %d of 2048 accepted, %d wrong", accepted, bad);

	// The field's bit order (tables B.5A, B.5B): TX(31) RC(10), TX(30) PC(7), TX(16) PC(0), TX(11) RC(0), TX(1) H(0),
	// TX(0) PC(8). PC(15-j) is the complement of the row's bit j, and H(0) the row's even parity
	for (uint8_t c = 0; c < 16; c++)
	{
		uint16_t info = dmrFecRcInfo(c);
		uint16_t h15 = dmrFecHamming15113Encode(info);
		uint32_t f = dmrFecRcEncode(c);
		uint32_t h0 = (uint32_t)popcount((uint32_t)h15) & 1u;

		bad += ((f >> 31) & 1u) != ((info >> 10) & 1u);
		bad += ((f >> 30) & 1u) != (((info >> 2) & 1u) ^ 1u);	// PC(7): column 8, RC(2)
		bad += ((f >> 11) & 1u) != (info & 1u);
		bad += ((f >> 16) & 1u) != (h0 ^ 1u);			// PC(0): column 15, H(0)
		bad += ((f >> 1) & 1u) != h0;
		bad += (f & 1u) != (((info >> 3) & 1u) ^ 1u);		// PC(8): column 7, RC(3)
		bad += popcount(f) != 16;	// each column holds a bit and its complement
	}
	CHECK(bad == 0, "RC field order: %d bits wrong", bad);

	// Every command back through up to 3 bit errors; 4 errors are never taken for another command
	int min = 32;

	bad = 0;
	for (uint8_t c = 0; c < 16; c++)
	{
		uint32_t f = dmrFecRcEncode(c);
		uint8_t got;

		for (uint8_t o = 0; o < 16; o++)
		{
			if (o != c && popcount(f ^ dmrFecRcEncode(o)) < min)
			{
				min = popcount(f ^ dmrFecRcEncode(o));
			}
		}
		bad += dmrFecRcDecode(f, &got) != 0 || got != c;
		for (int a = 0; a < 32; a++)
		{
			for (int b = a + 1; b < 32; b++)
			{
				for (int d = b + 1; d < 32; d++)
				{
					uint32_t e = (1u << a) | (1u << b) | (1u << d);

					bad += dmrFecRcDecode(f ^ (1u << a), &got) != 1 || got != c;
					bad += dmrFecRcDecode(f ^ e, &got) != 3 || got != c;
					for (int x = d + 1; x < 32; x += 7)
					{
						int r = dmrFecRcDecode(f ^ e ^ (1u << x), &got);

						bad += r >= 0 && got != c;
					}
				}
			}
		}
	}
	CHECK(min == 12, "RC codewords: minimum distance %d", min);
	CHECK(bad == 0, "RC decode: %d wrong", bad);

	// The embedded field (6.4.2): EMB CC 2, PI 1, LCSS 00 is QR(16,7,6) of 0x14, split around the RC
	uint8_t field[6], got;
	uint16_t emb14 = dmrFecQr1676Encode(0x14);

	dmrFecRcField(2, DMR_RC_CEASE, field);
	CHECK(field[0] == (emb14 >> 8) && field[5] == (emb14 & 0xFF), "RC field: EMB");
	CHECK(((uint32_t)field[1] << 24 | (uint32_t)field[2] << 16 | (uint32_t)field[3] << 8 | field[4]) ==
	      dmrFecRcEncode(DMR_RC_CEASE), "RC field: RC");
	// A BS's cease as sent on air, CC 2
	static const uint8_t onAir[6] = { 0x28, 0x70, 0xD0, 0x4F, 0x1F, 0x7E };
	CHECK(memcmp(field, onAir, 6) == 0, "RC field: as on air");
	CHECK(dmrFecRcFromField(field, 2, &got) == 0 && got == DMR_RC_CEASE, "RC field: decode");
	field[2] ^= 0x41;	// two RC bit errors and one EMB bit error
	field[0] ^= 0x02;
	CHECK(dmrFecRcFromField(field, 2, &got) == 3 && got == DMR_RC_CEASE, "RC field: errors corrected");
	CHECK(dmrFecRcFromField(field, 3, &got) < 0, "RC field: another color code");
	// Not an RC: PI 0 (a null fragment or this channel's LC), another LCSS, a BS voice or data sync
	uint16_t embs[2] = { dmrFecEmbEncode(2, false, 0), dmrFecEmbEncode(2, true, 1) };
	for (int k = 0; k < 2; k++)
	{
		dmrFecRcField(2, DMR_RC_CEASE, field);
		field[0] = (uint8_t)(embs[k] >> 8);
		field[5] = (uint8_t)embs[k];
		CHECK(dmrFecRcFromField(field, 2, &got) < 0, "RC field: EMB %04X taken", embs[k]);
	}
	static const uint8_t syncs[2][6] = { { 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 }, { 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D } };
	for (int k = 0; k < 2; k++)
	{
		CHECK(dmrFecRcFromField(syncs[k], 2, &got) < 0, "RC field: sync %d taken", k);
	}
}

int main(void)
{
	testHamming();
	testGolay();
	testSlotType();
	testBptc();
	testQr1676();
	testReverseChannel();

	printf("# dmrFecTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
