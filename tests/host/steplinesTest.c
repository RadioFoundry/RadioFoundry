// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The step lines (drivers/input/steplines): one press per held level change on either edge, bounce ignored, presses
// paced with a scan up between them, a fast roll kept whole, lines independent, lines beyond the count ignored
#include <stdio.h>
#include "input/steplines/steplines.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static steplines_t s;

// Scans with the same levels until n scans have passed; returns the presses of line `bit` seen
static int run(uint8_t levels, int n, uint8_t bit)
{
	int presses = 0;
	uint8_t prev = 0;
	for (int i = 0; i < n; i++) {
		uint8_t out = steplines_scan(&s, levels);
		if ((out & bit) && !(prev & bit)) {
			presses++;
		}
		prev = out;
	}
	return presses;
}

static void test_edges(void)
{
	steplines_init(&s, 4, 0x0F);
	CHECK(run(0x0F, 5, 0x01) == 0);		// at rest: nothing
	CHECK(run(0x0E, 5, 0x01) == 1);		// falling edge: one press
	CHECK(run(0x0F, 5, 0x01) == 1);		// rising edge: one more
	CHECK(run(0x0F, 5, 0x0F) == 0);
}

static void test_bounce(void)
{
	steplines_init(&s, 4, 0x0F);
	// A single-scan glitch never counts
	CHECK(steplines_scan(&s, 0x0E) == 0);
	CHECK(steplines_scan(&s, 0x0F) == 0);
	CHECK(run(0x0F, 5, 0x0F) == 0);
	// Two agreeing scans do, and the press comes out on the second scan
	CHECK(steplines_scan(&s, 0x0E) == 0);
	CHECK(steplines_scan(&s, 0x0E) == 0x01);
	CHECK(steplines_scan(&s, 0x0E) == 0);	// one scan long
}

static void test_pacing(void)
{
	steplines_init(&s, 4, 0x00);
	// Three steps two scans apart: each press needs a scan up first, so they come out one per two scans
	uint8_t lv = 0;
	int presses = 0;
	uint8_t prev = 0;
	for (int i = 0; i < 6; i++) {
		if (i % 2 == 0) {
			lv ^= 0x02;
		}
		uint8_t out = steplines_scan(&s, lv);
		CHECK(!((out & 0x02) && (prev & 0x02)));	// never two scans in a row
		presses += (out & 0x02) && !(prev & 0x02);
		prev = out;
	}
	presses += run(lv, 10, 0x02);
	CHECK(presses == 3);
}

static void test_fast_roll(void)
{
	steplines_init(&s, 4, 0x00);
	// Twenty steps, each held for two scans
	uint8_t lv = 0;
	int presses = 0;
	uint8_t prev = 0;
	for (int i = 0; i < 20; i++) {
		lv ^= 0x04;
		for (int j = 0; j < 2; j++) {
			uint8_t out = steplines_scan(&s, lv);
			presses += (out & 0x04) && !(prev & 0x04);
			prev = out;
		}
	}
	presses += run(lv, 20, 0x04);
	// A level counts at most every two scans and a press goes out every two scans, so a roll at the fastest pace the
	// scan can see loses nothing and the backlog never fills
	CHECK(presses == 20);
	CHECK(s.pending[2] == 0);
}

static void test_independent_lines(void)
{
	steplines_init(&s, 4, 0x0F);
	// Up and left at once: both come out in the same scan
	CHECK(steplines_scan(&s, 0x0A) == 0);
	CHECK(steplines_scan(&s, 0x0A) == 0x05);
	CHECK(steplines_scan(&s, 0x0A) == 0);
}

static void test_unused_lines(void)
{
	steplines_init(&s, 2, 0x00);
	// Lines 2 and up are not in use: their changes never press
	CHECK(run(0xFC, 5, 0xFC) == 0);
	CHECK(run(0xFD, 5, 0x01) == 1);
}

int main(void)
{
	test_edges();
	test_bounce();
	test_pacing();
	test_fast_roll();
	test_independent_lines();
	test_unused_lines();
	printf("steplinesTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
