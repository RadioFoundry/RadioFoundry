// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// AMBE+2 3600x2450 air frame FEC: interleave coverage, Golay codes, round trips with injected errors, burst layout,
// and known-good vectors.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmr/ambe/ambeFec.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

static void randomBits(uint8_t *bits, int n)
{
	for (int i = 0; i < n; i++)
		bits[i] = (uint8_t)(rnd() & 1u);
}

static void flipAir(uint8_t *frame, unsigned airbit)
{
	frame[airbit >> 3] ^= (uint8_t)(0x80u >> (airbit & 7u));
}

// Gotcha 4.3: the interleave must cover every one of the 72 bits exactly once
static void testInterleaveCoverage(void)
{
	int seen[72] = { 0 };
	for (int n = 0; n < 72; n++) {
		int a = ambeAirBitOfSeq((uint8_t)n);
		CHECK(a >= 0 && a < 72, "air bit %d out of range for S%d", a, n);
		if (a >= 0 && a < 72)
			seen[a]++;
	}
	for (int a = 0; a < 72; a++)
		CHECK(seen[a] == 1, "air bit %d covered %d times", a, seen[a]);

	// Each single code word bit lands on exactly one air bit, and de-interleaving brings it back
	static const uint8_t len[4] = { 24, 23, 11, 14 };
	for (int w = 0; w < 4; w++) {
		for (int b = 0; b < len[w]; b++) {
			uint32_t c[4] = { 0 }, back[4];
			uint8_t frame[9];
			c[w] = 1u << b;
			ambeInterleave(c, frame);
			int ones = 0;
			for (int i = 0; i < 9; i++)
				for (int k = 0; k < 8; k++)
					ones += (frame[i] >> k) & 1;
			CHECK(ones == 1, "C%d bit %d sets %d air bits", w, b, ones);
			ambeDeinterleave(frame, back);
			CHECK(memcmp(c, back, sizeof c) == 0, "C%d bit %d de-interleave", w, b);
		}
	}

	// Spot checks of the layout: dibit 0 = (C0 bit 23, C0 bit 5), dibit 1 = (C1 bit 10, C2 bit 3), dibit 35 = (C2 bit 4,
	// C3 bit 0) in the AMBE+2 3600x2450 ordering (C0 MSB = S0, C1 starts at S24, C2 at S47, C3 at S58)
	CHECK(ambeAirBitOfSeq(0) == 0, "C0 MSB first on the air");
	CHECK(ambeAirBitOfSeq(18) == 1, "C0 bit 5 is air bit 1");
	CHECK(ambeAirBitOfSeq(24 + 12) == 2, "C1 bit 10 is air bit 2");
	CHECK(ambeAirBitOfSeq(47 + 7) == 3, "C2 bit 3 is air bit 3");
	CHECK(ambeAirBitOfSeq(47 + 6) == 70, "C2 bit 4 is air bit 70");
	CHECK(ambeAirBitOfSeq(71) == 71, "C3 LSB last on the air");
}

static void testGolay(void)
{
	// The generator: x^11 mod g = g without its top term
	CHECK(ambeGolay23Encode(1) == ((1u << 11) | 0x475u), "Golay(23,12) of 1 is 0x%X", ambeGolay23Encode(1));
	int minw23 = 99, minw24 = 99;
	for (uint32_t d = 1; d < 4096; d++) {
		uint32_t c23 = ambeGolay23Encode((uint16_t)d), c24 = ambeGolay24Encode((uint16_t)d);
		int w23 = __builtin_popcount(c23), w24 = __builtin_popcount(c24);
		if (w23 < minw23)
			minw23 = w23;
		if (w24 < minw24)
			minw24 = w24;
		CHECK((w24 & 1) == 0, "Golay(24,12) word for %u has odd weight", d);
	}
	CHECK(minw23 == 7, "Golay(23,12) minimum distance %d", minw23);
	CHECK(minw24 == 8, "Golay(24,12) minimum distance %d", minw24);

	// Every error pattern of weight <= 3 corrected, for a few data words; every weight-4 pattern detected (24,12)
	static const uint16_t datas[] = { 0x000, 0xFFF, 0xA5C, 0x123 };
	for (unsigned di = 0; di < sizeof datas / sizeof datas[0]; di++) {
		uint16_t d = datas[di], out;
		uint32_t c23 = ambeGolay23Encode(d), c24 = ambeGolay24Encode(d);
		int bad = 0;
		for (int i = 0; i < 23; i++)
			for (int j = i; j < 23; j++)
				for (int k = j; k < 23; k++) {
					uint32_t e = (1u << i) ^ (1u << j) ^ (1u << k);	// weight 1 or 3 (i = j = k cancels to weight 1)
					int n = ambeGolay23Decode(c23 ^ e, &out);
					if (out != d || n != __builtin_popcount(e))
						bad++;
				}
		CHECK(bad == 0, "Golay(23,12) data 0x%03X: %d patterns of weight <= 3 not corrected", d, bad);
		bad = 0;
		for (int i = 0; i < 24; i++)
			for (int j = i + 1; j < 24; j++) {
				uint32_t e = (1u << i) | (1u << j);
				int n = ambeGolay24Decode(c24 ^ e, &out);
				if (out != d || n != 2)
					bad++;
			}
		CHECK(bad == 0, "Golay(24,12) data 0x%03X: %d double errors not corrected", d, bad);
		int undetected = 0, total = 0;
		for (int i = 0; i < 24; i++)
			for (int j = i + 1; j < 24; j++)
				for (int k = j + 1; k < 24; k++) {
					int n = ambeGolay24Decode(c24 ^ ((1u << i) | (1u << j) | (1u << k)), &out);
					if (out != d || n != 3)
						bad++;
					for (int l = k + 1; l < 24; l += 5) {
						total++;
						if (ambeGolay24Decode(c24 ^ ((1u << i) | (1u << j) | (1u << k) | (1u << l)), &out) >= 0)
							undetected++;
					}
				}
		CHECK(bad == 0, "Golay(24,12) data 0x%03X: %d triple errors not corrected", d, bad);
		CHECK(undetected == 0, "Golay(24,12) data 0x%03X: %d of %d quadruple errors not detected", d, undetected, total);
	}
}

static void testWhitening(void)
{
	// x0 = 16 * data; x' = 173 x + 13849 mod 65536; bit = x >> 15, first bit on C1's MSB
	uint16_t data = 0xABC;
	uint32_t x = 16u * data, want = 0;
	for (int k = 0; k < 23; k++) {
		x = (173u * x + 13849u) % 65536u;
		want = (want << 1) | (x >= 32768u);
	}
	CHECK(ambeC1Mask(data) == want, "C1 mask 0x%06X, want 0x%06X", ambeC1Mask(data), want);
	CHECK(ambeC1Mask(0) != 0, "mask for data 0 is not zero (13849 offset)");
}

static void testRoundTrip(void)
{
	uint8_t bits[49], back[49], frame[9];
	ambe_fec_result_t r;
	for (int t = 0; t < 2000; t++) {
		randomBits(bits, 49);
		ambeFecEncode(bits, frame);
		int n = ambeFecDecode(frame, back, &r);
		CHECK(n == 0 && !r.c0_failed && memcmp(bits, back, 49) == 0, "clean round trip %d", t);
	}

	// Up to 3 errors in C0 and up to 3 in C1 (positions in the S sequence) are corrected and counted
	for (int t = 0; t < 3000; t++) {
		randomBits(bits, 49);
		ambeFecEncode(bits, frame);
		int e0 = (int)(rnd() % 4), e1 = (int)(rnd() % 4);
		int used[72] = { 0 };
		for (int i = 0; i < e0; i++) {
			int s;
			do s = (int)(rnd() % 24); while (used[s]);
			used[s] = 1;
			flipAir(frame, ambeAirBitOfSeq((uint8_t)s));
		}
		for (int i = 0; i < e1; i++) {
			int s;
			do s = 24 + (int)(rnd() % 23); while (used[s]);
			used[s] = 1;
			flipAir(frame, ambeAirBitOfSeq((uint8_t)s));
		}
		int n = ambeFecDecode(frame, back, &r);
		CHECK(memcmp(bits, back, 49) == 0 && r.c0_errors == e0 && r.c1_errors == e1 && n == e0 + e1 && !r.c0_failed,
		      "errors C0 %d C1 %d: decoded %d (%d, %d)", e0, e1, n, r.c0_errors, r.c1_errors);
	}

	// 4 errors in C0 are flagged as a lost frame
	int missed = 0;
	for (int t = 0; t < 500; t++) {
		randomBits(bits, 49);
		ambeFecEncode(bits, frame);
		int used[24] = { 0 };
		for (int i = 0; i < 4; i++) {
			int s;
			do s = (int)(rnd() % 24); while (used[s]);
			used[s] = 1;
			flipAir(frame, ambeAirBitOfSeq((uint8_t)s));
		}
		ambeFecDecode(frame, back, &r);
		if (!r.c0_failed || r.errors != 7)
			missed++;
	}
	CHECK(missed == 0, "%d C0 quadruple errors not flagged", missed);

	// Errors in C2 / C3 pass through uncorrected and uncounted (no protection)
	randomBits(bits, 49);
	ambeFecEncode(bits, frame);
	flipAir(frame, ambeAirBitOfSeq(47));	// C2 MSB = parameter bit 24
	flipAir(frame, ambeAirBitOfSeq(71));	// C3 LSB = parameter bit 48
	int n = ambeFecDecode(frame, back, NULL);
	CHECK(n == 0 && back[24] == (bits[24] ^ 1) && back[48] == (bits[48] ^ 1), "C2/C3 unprotected");
}

// Vectors produced by the stock DM-1701 V02.06 firmware's own AMBE+2 FEC routine (encode ECC entry 0x08074BFC, run
// on the host in an instruction set emulator against the decrypted stock image): 49 parameter bits in, 72 air bits
// out. Our encoder matched it on 249 of 249 vectors (random, all zeros, all ones, each single bit).
static const struct { uint8_t in[7]; uint8_t out[9]; } stock_vectors[] = {
	{ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x22, 0x00, 0x02, 0x04, 0x02, 0x20, 0x40, 0x44, 0x00 } },
	{ { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x80 }, { 0xDD, 0xDD, 0xFF, 0xFF, 0xDF, 0xBF, 0xBB, 0xFF, 0xBB } },
	{ { 0x04, 0x2A, 0x62, 0xFC, 0xF5, 0x44, 0x00 }, { 0x61, 0x15, 0x7C, 0x30, 0x74, 0xB6, 0x2E, 0x72, 0xE4 } },
	{ { 0xD3, 0xA2, 0x6C, 0x25, 0x2B, 0xB8, 0x80 }, { 0xDA, 0x4B, 0x21, 0xAD, 0x97, 0x81, 0xDB, 0x40, 0xE5 } },
	{ { 0x6B, 0x80, 0x7E, 0xA5, 0x4A, 0x3B, 0x00 }, { 0x78, 0x94, 0x87, 0xCF, 0xC0, 0x23, 0x17, 0x81, 0xB8 } },
	{ { 0x44, 0x24, 0x3F, 0x23, 0xF3, 0x5C, 0x80 }, { 0x3F, 0x57, 0x18, 0x25, 0x52, 0x90, 0x17, 0x94, 0x87 } },
	{ { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x42, 0xE0, 0x22, 0x66, 0x62, 0x40, 0x88, 0xCC, 0x08 } },
};

static void testStockVectors(void)
{
	for (unsigned v = 0; v < sizeof stock_vectors / sizeof stock_vectors[0]; v++) {
		uint8_t bits[49], frame[9], back[49];
		for (int i = 0; i < 49; i++)
			bits[i] = (stock_vectors[v].in[i >> 3] >> (7 - (i & 7))) & 1;
		ambeFecEncode(bits, frame);
		CHECK(memcmp(frame, stock_vectors[v].out, 9) == 0, "stock vector %u encode", v);
		int n = ambeFecDecode(stock_vectors[v].out, back, NULL);
		CHECK(n == 0 && memcmp(bits, back, 49) == 0, "stock vector %u decode", v);
	}
}

static void testBurst(void)
{
	uint8_t frames[3][9], back[3][9], burst[27], full[33];
	for (int f = 0; f < 3; f++)
		for (int i = 0; i < 9; i++)
			frames[f][i] = (uint8_t)rnd();
	ambeBurstJoin(frames, burst);
	ambeBurstSplit(burst, back);
	CHECK(memcmp(frames, back, sizeof frames) == 0, "27-byte burst round trip");
	CHECK(memcmp(&burst[9], frames[1], 9) == 0, "frame 2 is bytes 9-17");

	memset(full, 0x5A, sizeof full);	// middle 48 bits (bytes 13.5 .. 19.5) must survive
	ambeFullBurstJoin(frames, full);
	ambeFullBurstSplit(full, back);
	CHECK(memcmp(frames, back, sizeof frames) == 0, "33-byte burst round trip");
	// burst bits 108..155 untouched: byte 13 low nibble, bytes 14..18, byte 19 high nibble
	CHECK((full[13] & 0x0F) == 0x0A && full[14] == 0x5A && full[18] == 0x5A && (full[19] & 0xF0) == 0x50, "sync / EMB kept");
	// The first 13.5 bytes are the first 108 payload bits; the last 13.5 bytes the other 108
	CHECK(memcmp(full, burst, 13) == 0 && (full[13] >> 4) == (burst[13] >> 4), "first half");
	int ok = 1;
	for (int p = 108; p < 216; p++) {
		int a = (burst[p >> 3] >> (7 - (p & 7))) & 1, q = p + 48, b = (full[q >> 3] >> (7 - (q & 7))) & 1;
		if (a != b)
			ok = 0;
	}
	CHECK(ok, "second half after the 48 middle bits");
}

int main(void)
{
	testInterleaveCoverage();
	testGolay();
	testWhitening();
	testRoundTrip();
	testStockVectors();
	testBurst();
	printf("# ambeFecTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
