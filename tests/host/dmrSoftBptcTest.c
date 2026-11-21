// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host test for core/dmr/fec/dmrSoftBptc: clean and noisy bursts, erasures, the accept check
 *
 *   make -C tests/host dmrSoftBptcTest && tests/host/dmrSoftBptcTest
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/fec/dmrFec.h"
#include "dmr/fec/dmrSoftBptc.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rngState = 12345;

static uint32_t rng(void)
{
	rngState = rngState * 1664525u + 1013904223u;
	return rngState;
}

static double gauss(void)
{
	double u1 = ((rng() >> 8) + 1.0) / 16777217.0, u2 = (rng() >> 8) / 16777216.0;
	return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

// CRC-CCITT over the first 10 bytes, in the last 2: what a CSBK carries (the mask doesn't matter here)
static uint16_t crc16(const uint8_t *p)
{
	uint16_t c = 0;
	for (int i = 0; i < 10; i++) {
		c ^= (uint16_t)(p[i] << 8);
		for (int b = 0; b < 8; b++) {
			c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
		}
	}
	return c;
}

static bool crcOk(const uint8_t *payload, void *ctx)
{
	int *calls = ctx;
	if (calls) {
		(*calls)++;
	}
	return crc16(payload) == (uint16_t)(payload[10] << 8 | payload[11]);
}

static bool never(const uint8_t *payload, void *ctx)
{
	(void)payload;
	(void)ctx;
	return false;
}

static void randomPayload(uint8_t *p)
{
	for (int i = 0; i < 10; i++) {
		p[i] = (uint8_t)(rng() >> 24);
	}
	uint16_t c = crc16(p);
	p[10] = (uint8_t)(c >> 8);
	p[11] = (uint8_t)c;
}

// The info bits of a burst, in the order the decoder takes them
static void infoBits(const uint8_t *burst, uint8_t *bits)
{
	for (int i = 0; i < DMR_SOFT_BPTC_BITS; i++) {
		int b = i < 98 ? i : i + 68;
		bits[i] = (burst[b / 8] >> (7 - b % 8)) & 1;
	}
}

static int8_t toLlr(double v)
{
	long q = lround(v * 8.0);
	return (int8_t)(q > 127 ? 127 : q < -127 ? -127 : q);
}

static void testClean(void)
{
	dmrSoftBptcWork_t w;
	uint8_t payload[12], out[12], burst[DMR_FEC_BURST_BYTES], bits[DMR_SOFT_BPTC_BITS], rbits;
	int8_t llr[DMR_SOFT_BPTC_BITS];
	for (int n = 0; n < 64; n++) {
		randomPayload(payload);
		memset(burst, 0, sizeof(burst));
		dmrFecBptcEncode(payload, burst);
		// R(3) is matrix bit 0 = info bit 0, outside every codeword; R(2)-R(0) are in row 0's and stay 0 as encoded
		uint8_t r = (uint8_t)((n & 1) << 3);
		infoBits(burst, bits);
		bits[0] = (uint8_t)(n & 1);
		for (int i = 0; i < DMR_SOFT_BPTC_BITS; i++) {
			llr[i] = bits[i] ? 40 : -40;
		}
		int it = dmrSoftBptcDecode(&w, llr, out, &rbits, DMR_SOFT_BPTC_ITERS, crcOk, NULL);
		CHECK(it == 0, "clean burst %d: %d iterations", n, it);
		CHECK(memcmp(out, payload, 12) == 0, "clean burst %d: payload", n);
		CHECK(rbits == r, "clean burst %d: R %x, want %x", n, rbits, r);
	}
}

// AWGN on +-1 at a given Eb/N0 per coded bit: the soft decoder must decode clearly more than the hard one, and every
// payload it accepts must be the one sent
static void testNoise(double sigma, int bursts, double minSoft)
{
	dmrSoftBptcWork_t w;
	uint8_t payload[12], out[12], burst[DMR_FEC_BURST_BYTES], hb[DMR_FEC_BURST_BYTES], bits[DMR_SOFT_BPTC_BITS];
	int8_t llr[DMR_SOFT_BPTC_BITS];
	int hardOk = 0, softOk = 0, wrong = 0;
	for (int n = 0; n < bursts; n++) {
		randomPayload(payload);
		memset(burst, 0, sizeof(burst));
		dmrFecBptcEncode(payload, burst);
		infoBits(burst, bits);
		memset(hb, 0, sizeof(hb));
		for (int i = 0; i < DMR_SOFT_BPTC_BITS; i++) {
			double y = (bits[i] ? 1.0 : -1.0) + sigma * gauss();
			llr[i] = toLlr(2.0 * y / (sigma * sigma));
			if (y > 0) {
				int b = i < 98 ? i : i + 68;
				hb[b / 8] |= (uint8_t)(0x80u >> (b % 8));
			}
		}
		if (dmrFecBptcDecode(hb, out) >= 0 && crcOk(out, NULL) && memcmp(out, payload, 12) == 0) {
			hardOk++;
		}
		if (dmrSoftBptcDecode(&w, llr, out, NULL, DMR_SOFT_BPTC_ITERS, crcOk, NULL) >= 0) {
			if (memcmp(out, payload, 12) == 0) {
				softOk++;
			} else {
				wrong++;
			}
		}
	}
	printf("  sigma %.2f: hard %d/%d, soft %d/%d, wrong %d\n", sigma, hardOk, bursts, softOk, bursts, wrong);
	CHECK(softOk >= minSoft * bursts, "sigma %.2f: soft %d/%d below %.0f %%", sigma, softOk, bursts, minSoft * 100);
	CHECK(softOk > hardOk, "sigma %.2f: soft %d not above hard %d", sigma, softOk, hardOk);
	CHECK(wrong == 0, "sigma %.2f: %d wrong payloads accepted", sigma, wrong);
}

// Erased bits (LLR 0) are filled in from the rest: a whole row's worth spread over the matrix
static void testErasures(void)
{
	dmrSoftBptcWork_t w;
	uint8_t payload[12], out[12], burst[DMR_FEC_BURST_BYTES], bits[DMR_SOFT_BPTC_BITS];
	int8_t llr[DMR_SOFT_BPTC_BITS];
	randomPayload(payload);
	memset(burst, 0, sizeof(burst));
	dmrFecBptcEncode(payload, burst);
	infoBits(burst, bits);
	for (int i = 0; i < DMR_SOFT_BPTC_BITS; i++) {
		llr[i] = (i % 13 == 5) ? 0 : bits[i] ? 30 : -30;	// 15 erasures
	}
	int it = dmrSoftBptcDecode(&w, llr, out, NULL, DMR_SOFT_BPTC_ITERS, crcOk, NULL);
	CHECK(it >= 0 && memcmp(out, payload, 12) == 0, "15 erasures: %d", it);
	// A burst of 24 erased info bits in a row (a fade): the interleaver spreads it
	for (int i = 0; i < DMR_SOFT_BPTC_BITS; i++) {
		llr[i] = (i >= 60 && i < 84) ? 0 : bits[i] ? 30 : -30;
	}
	it = dmrSoftBptcDecode(&w, llr, out, NULL, DMR_SOFT_BPTC_ITERS, crcOk, NULL);
	CHECK(it >= 0 && memcmp(out, payload, 12) == 0, "24 erased in a row: %d", it);
}

// accept decides: a payload it refuses never comes back; a clean burst is decided by the hard decoder alone, accept
// asked once
static void testAccept(void)
{
	dmrSoftBptcWork_t w;
	uint8_t payload[12], out[12], burst[DMR_FEC_BURST_BYTES], bits[DMR_SOFT_BPTC_BITS];
	int8_t llr[DMR_SOFT_BPTC_BITS];
	randomPayload(payload);
	memset(burst, 0, sizeof(burst));
	dmrFecBptcEncode(payload, burst);
	infoBits(burst, bits);
	for (int i = 0; i < DMR_SOFT_BPTC_BITS; i++) {
		llr[i] = bits[i] ? 30 : -30;
	}
	CHECK(dmrSoftBptcDecode(&w, llr, out, NULL, 3, never, NULL) == -1, "refused payload returned");
	int calls = 0;
	CHECK(dmrSoftBptcDecode(&w, llr, out, NULL, 3, crcOk, &calls) == 0 && calls == 1, "accept calls %d", calls);
	CHECK(dmrSoftBptcDecode(&w, llr, out, NULL, 3, NULL, NULL) == 0 && memcmp(out, payload, 12) == 0, "no accept");
	// All erased: nothing to decode, and no crash
	memset(llr, 0, sizeof(llr));
	(void)dmrSoftBptcDecode(&w, llr, out, NULL, DMR_SOFT_BPTC_ITERS, crcOk, NULL);
}

int main(void)
{
	testClean();
	testErasures();
	testAccept();
	testNoise(0.55, 400, 0.97);
	testNoise(0.65, 400, 0.90);
	testNoise(0.85, 1000, 0.60);
	if (failures) {
		printf("dmrSoftBptcTest: %d FAILED\n", failures);
		return 1;
	}
	printf("dmrSoftBptcTest: all passed\n");
	return 0;
}
