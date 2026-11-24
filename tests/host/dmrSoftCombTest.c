// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host test for core/dmr/fec/dmrSoftComb.c: a control channel's rotation of CSBKs received through noise, the reserved
 * bits set as some systems send them, the chip's decode wrong. Combining must recover most bursts, never pick a payload
 * that wasn't sent, and leave a good chip decode alone.
 *
 *   make -C tests/host
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/fec/dmrFec.h"
#include "dmr/fec/dmrSoftComb.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static uint32_t rng = 12345u;

static double uniform(void)
{
	rng = rng * 1664525u + 1013904223u;
	return ((rng >> 8) + 0.5) / 16777216.0;
}

static double gauss(void)
{
	return sqrt(-2.0 * log(uniform())) * cos(6.283185307179586 * uniform());
}

static bool infoBit(const uint8_t *burst, int i)
{
	int b = i < 98 ? i : i + 68;
	return (burst[b / 8] >> (7 - (b % 8))) & 1u;
}

// The soft values of payload's codeword (reserved bits r) through noise sigma, on the scale the HR_C6000 / HR_C7000
// tap gives (a clean bit about 24)
static void received(const uint8_t *payload, uint8_t r, double sigma, int8_t *v)
{
	uint8_t burst[DMR_FEC_BURST_BYTES];
	memset(burst, 0, sizeof(burst));
	dmrFecBptcEncodeR(payload, r, burst);
	for (int i = 0; i < DMR_SOFT_BITS; i++) {
		double x = (infoBit(burst, i) ? 24.0 : -24.0) + sigma * gauss();
		v[i] = (int8_t)(x > 31 ? 31 : x < -32 ? -32 : x);
	}
}

static int hardDecodes(const int8_t *v, uint8_t *out)
{
	uint8_t burst[DMR_FEC_BURST_BYTES];
	memset(burst, 0, sizeof(burst));
	for (int i = 0; i < DMR_SOFT_BITS; i++) {
		int b = i < 98 ? i : i + 68;
		if (v[i] > 0) {
			burst[b / 8] |= (uint8_t)(0x80u >> (b % 8));
		}
	}
	return dmrFecBptcDecode(burst, out) >= 0;
}

static void testReservedBits(void)
{
	uint8_t p[12] = { 0x9e, 0x00, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x11, 0x22 }, q[12], burst[33];
	for (uint8_t r = 0; r < 16; r++) {
		uint8_t got = 0xFF;
		memset(burst, 0, sizeof(burst));
		dmrFecBptcEncodeR(p, r, burst);
		CHECK(dmrFecBptcDecodeR(burst, q, &got) == 0 && got == r && memcmp(p, q, 12) == 0, "R bits %u: got %u", r, got);
	}
}

// A rotation of 4 CSBKs on each slot through noise sigma. chipRight: the chip's decode is the payload sent, else one
// bit off. Checks: at least minPct right after combining; a payload that wasn't sent never comes out twice (the accept
// invalid CRC rule takes only repeats); a right chip decode is almost never replaced
static void testRotation(double sigma, int minPct, bool chipRight)
{
	static dmrSoftComb_t c;
	static uint8_t wrongs[2000][12];
	uint8_t msg[2][4][12];
	dmrSoftCombInit(&c);
	for (int s = 0; s < 2; s++) {
		for (int m = 0; m < 4; m++) {
			for (int k = 0; k < 12; k++) {
				msg[s][m][k] = (uint8_t)(uniform() * 256);
			}
		}
	}
	int n = 0, right = 0, alone = 0, wrong = 0, repeats = 0;
	for (int t = 0; t < 2000; t++) {
		int s = t & 1, m = (t / 2) % 4;
		uint8_t tag, pay[12], guess[12], tmp[12];
		int8_t *v = dmrSoftCombStage(&c, &tag);
		received(msg[s][m], 0x4, sigma, v);
		alone += hardDecodes(v, tmp) && memcmp(tmp, msg[s][m], 12) == 0;
		memcpy(guess, msg[s][m], 12);
		guess[5] ^= chipRight ? 0 : 0x10;
		memcpy(pay, guess, 12);
		dmrSoftCombTake(&c, tag, (uint8_t)s, pay);
		n++;
		right += memcmp(pay, msg[s][m], 12) == 0;
		if (memcmp(pay, msg[s][m], 12) != 0 && memcmp(pay, guess, 12) != 0) {
			for (int k = 0; k < wrong; k++) {
				repeats += memcmp(wrongs[k], pay, 12) == 0;
			}
			memcpy(wrongs[wrong++], pay, 12);
		}
	}
	printf("sigma %.0f, chip %s: %d%% decode alone, %d%% right after combining, %d replaced wrongly, %d repeats\n", sigma,
	       chipRight ? "right" : "wrong", 100 * alone / n, 100 * right / n, wrong, repeats);
	CHECK(100 * right / n >= minPct, "sigma %.0f: %d%% right, want %d%%", sigma, 100 * right / n, minPct);
	CHECK(repeats == 0, "a wrong payload came out twice");
	CHECK(!chipRight || wrong <= n / 100, "%d right chip decodes replaced", wrong);
	CHECK(c.st.taken == (uint32_t)n && c.st.stale == 0, "taken %u stale %u", (unsigned)c.st.taken, (unsigned)c.st.stale);
}

// A good chip decode of a clean burst stays as it is; the stage of a burst overwritten before it was taken is refused
static void testKeepsAndStale(void)
{
	static dmrSoftComb_t c;
	uint8_t p[12] = { 0x83, 0x06, 0x0f, 0x53, 0x5f, 0x1c, 0xfe, 0x78, 0x50, 0x02, 0xbc, 0x5d }, pay[12], tag, tag2;
	dmrSoftCombInit(&c);
	received(p, 0, 0.0, dmrSoftCombStage(&c, &tag));
	memcpy(pay, p, 12);
	CHECK(!dmrSoftCombTake(&c, tag, 0, pay) && memcmp(pay, p, 12) == 0, "a clean burst changed");

	(void)dmrSoftCombStage(&c, &tag);
	(void)dmrSoftCombStage(&c, &tag2);
	(void)dmrSoftCombStage(&c, &tag2);	// two stages: the first is reused
	CHECK(!dmrSoftCombTake(&c, tag, 0, pay) && c.st.stale == 1, "stale stage taken");
	CHECK(!dmrSoftCombTake(&c, 0, 0, pay) && !dmrSoftCombTake(&c, tag2, 2, pay), "bad tag or slot taken");
}

// The glue's holder: allocated on, counters kept over a free, everything under the lock that the diag read takes
static int locks, allocs, frees;
static void *testAlloc(size_t n) { allocs++; return malloc(n); }
static void testFree(void *p) { frees++; free(p); }
static void testLock(void *ctx) { (void)ctx; locks++; }
static void testUnlock(void *ctx) { (void)ctx; locks--; }

static void testHold(void)
{
	static const dmrSoftHoldOps_t ops = { testAlloc, testFree, testLock, testUnlock };
	dmrSoftComb_t *volatile at = NULL;
	dmrSoftHold_t h = { &at, &ops, { 0 } };
	dmrSoftStats_t st;
	uint8_t tag, tag2, pay[12] = { 0 };

	dmrSoftHoldSet(&h, true);
	dmrSoftHoldSet(&h, true);
	CHECK(at != NULL && allocs == 1, "hold: one combiner");
	(void)dmrSoftCombStage(at, &tag);
	(void)dmrSoftCombStage(at, &tag2);
	(void)dmrSoftCombStage(at, &tag2);
	(void)dmrSoftCombTake(at, tag, 0, pay);	// one stale: its stage was reused
	dmrSoftHoldSet(&h, false);
	CHECK(at == NULL && frees == 1 && locks == 0, "hold: freed");
	dmrSoftHoldSet(&h, true);
	(void)dmrSoftCombStage(at, &tag);
	(void)dmrSoftCombStage(at, &tag2);
	(void)dmrSoftCombStage(at, &tag2);
	(void)dmrSoftCombTake(at, tag, 0, pay);	// another: its stage was reused
	dmrSoftHoldStats(&h, &st);
	CHECK(st.stale == 2 && locks == 0, "hold: counters over a free, %u stale", (unsigned)st.stale);
	dmrSoftHoldSet(&h, false);
	CHECK(allocs == 2 && frees == 2, "hold: freed again");
}

int main(void)
{
	testHold();
	testReservedBits();
	testKeepsAndStale();
	testRotation(22.0, 90, false);	// none decode alone
	testRotation(26.0, 40, false);
	testRotation(32.0, 0, false);	// too noisy to combine: only no repeats
	testRotation(22.0, 99, true);
	testRotation(32.0, 90, true);
	if (failures) {
		printf("dmrSoftCombTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("dmrSoftCombTest: all passed\n");
	return 0;
}
