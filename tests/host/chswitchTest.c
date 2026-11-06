// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The channel switch decoder (drivers/input/chswitch): a code counts only once it held settle_ms, so the code a knob
// passes through between two positions never selects that other position
#include <stdio.h>

#include "input/chswitch/chswitch.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SETTLE	150u

// A 16-position switch whose codes run 6 2 9 12 5 1 10 14 15 7 0 8 13 4 3 11 from position 0 (one radio's, measured)
static const int8_t table[16] = {
	[6] = 0, [2] = 1, [9] = 2, [12] = 3, [5] = 4, [1] = 5, [10] = 6, [14] = 7,
	[15] = 8, [7] = 9, [0] = 10, [8] = 11, [13] = 12, [4] = 13, [3] = 14, [11] = 15,
};

// Feed code from ms t0 to t1 in 10 ms scans; returns the last result
static int8_t hold(chswitch_t *s, uint8_t code, uint32_t t0, uint32_t t1)
{
	int8_t p = -1;
	for (uint32_t t = t0; t <= t1; t += 10u) {
		p = chswitch_poll(s, code, t);
	}
	return p;
}

int main(void)
{
	chswitch_t s;
	// At power-on: nothing until the first code has settled, then its position
	chswitch_init(&s, table, 16, SETTLE, 6, 1000);
	CHECK(chswitch_poll(&s, 6, 1000) == -1);
	CHECK(hold(&s, 6, 1010, 1140) == -1);
	CHECK(chswitch_poll(&s, 6, 1150) == 0);
	// One click: the old position until the new code settles
	CHECK(hold(&s, 2, 1200, 1340) == 0);
	CHECK(chswitch_poll(&s, 2, 1350) == 1);
	// Between positions 2 (code 9) and 3 (code 12) the lines read 13 (position 12) for 100 ms: never selected
	CHECK(hold(&s, 9, 1400, 1600) == 2);
	CHECK(hold(&s, 13, 1610, 1710) == 2);
	CHECK(hold(&s, 12, 1720, 1860) == 2);
	CHECK(chswitch_poll(&s, 12, 1870) == 3);
	// A turn that doesn't stop anywhere: only where it rests counts
	for (uint8_t c = 0; c < 16; c++) {
		CHECK(chswitch_poll(&s, c, 2000u + 20u * c) == 3);
	}
	CHECK(hold(&s, 11, 2400, 2600) == 15);
	// A code outside the table, or one no position gives, keeps the position
	static const int8_t sparse[4] = { 0, -1, 1, 2 };
	chswitch_init(&s, sparse, 4, SETTLE, 0, 0);
	CHECK(hold(&s, 0, 0, 200) == 0);
	CHECK(hold(&s, 1, 210, 500) == 0);
	CHECK(hold(&s, 7, 510, 800) == 0);
	CHECK(hold(&s, 3, 810, 1000) == 2);
	// Across the clock's wrap
	chswitch_init(&s, table, 16, SETTLE, 6, 0xFFFFFFC0u);
	CHECK(chswitch_poll(&s, 6, 0xFFFFFFF0u) == -1);
	CHECK(chswitch_poll(&s, 6, 0x60u) == 0);
	printf("chswitchTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
