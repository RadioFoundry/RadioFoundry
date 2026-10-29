// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Two-level analog keypad (drivers/input/adckeys) with a two-ladder hand microphone's windows
// (docs/spec/hardware-md9600.md 9.3): every key at its window's edges and center, the gaps between windows, idle,
// and the debounce holding a key through levels that pass another key's window
#include <stdio.h>
#include "input/adckeys/adckeys.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

enum { K1, K2, K3, KA, K4, K5, K6, KB, K7, K8, K9, KC, STAR, K0, HASH, KD, DASH, AB, UP, DOWN };
static const adckeys_window_t rows[5] = {
	{ 2830, 3444 }, { 2215, 2705 }, { 1477, 2090 }, { 862, 1352 }, { 247, 737 },
};
static const adckeys_window_t cols[4] = {
	{ 0, 1106 }, { 1231, 1844 }, { 1969, 2705 }, { 2830, 3320 },
};
static const uint8_t map[5 * 4] = {
	K1, K2, K3, KA,
	K4, K5, K6, KB,
	K7, K8, K9, KC,
	STAR, K0, HASH, KD,
	DASH, AB, UP, DOWN,
};
static const adckeys_t pad = { rows, 5, cols, 4, map, 3445 };

int main(void)
{
	// Every key at both edges and the center of its two windows
	for (uint8_t r = 0; r < 5; r++) {
		for (uint8_t c = 0; c < 4; c++) {
			uint8_t want = map[r * 4 + c];
			uint16_t rl[3] = { rows[r].lo, (uint16_t)((rows[r].lo + rows[r].hi) / 2), rows[r].hi };
			uint16_t cl[3] = { cols[c].lo, (uint16_t)((cols[c].lo + cols[c].hi) / 2), cols[c].hi };
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++) {
					CHECK(adckeys_decode(&pad, rl[i], cl[j]) == want);
				}
			}
		}
	}
	// Idle: both levels at the top
	CHECK(adckeys_decode(&pad, 4095, 4095) == ADCKEYS_NONE);
	CHECK(adckeys_decode(&pad, 3445, 3445) == ADCKEYS_NONE);
	// One level idle, the other in a window: no key (no row or column matches the idle level)
	CHECK(adckeys_decode(&pad, 4095, 1500) == ADCKEYS_NONE);
	CHECK(adckeys_decode(&pad, 2500, 4095) == ADCKEYS_NONE);
	// Gaps between windows
	CHECK(adckeys_decode(&pad, 2770, 500) == ADCKEYS_NONE);		// between rows 0 and 1
	CHECK(adckeys_decode(&pad, 100, 500) == ADCKEYS_NONE);		// below row 4
	CHECK(adckeys_decode(&pad, 2500, 1150) == ADCKEYS_NONE);	// between columns 0 and 1
	CHECK(adckeys_decode(&pad, 2500, 3400) == ADCKEYS_NONE);	// above column 3

	// Debounce over two scans: a key is taken on its second scan, released on the second idle one
	adckeys_state_t s;
	adckeys_init(&s);
	CHECK(adckeys_debounce(&s, ADCKEYS_NONE, 2) == ADCKEYS_NONE);
	CHECK(adckeys_debounce(&s, K5, 2) == ADCKEYS_NONE);
	CHECK(adckeys_debounce(&s, K5, 2) == K5);
	CHECK(adckeys_debounce(&s, K5, 2) == K5);
	// The level passing through another window for one scan as the key comes up: K5 stays, then none
	CHECK(adckeys_debounce(&s, K8, 2) == K5);
	CHECK(adckeys_debounce(&s, ADCKEYS_NONE, 2) == K5);
	CHECK(adckeys_debounce(&s, ADCKEYS_NONE, 2) == ADCKEYS_NONE);
	// Alternating readings never settle: nothing reported
	for (int i = 0; i < 10; i++) {
		CHECK(adckeys_debounce(&s, (i & 1) ? K1 : K2, 2) == ADCKEYS_NONE);
	}
	// need 1: taken at once; a long hold doesn't overflow the count
	adckeys_init(&s);
	CHECK(adckeys_debounce(&s, UP, 1) == UP);
	for (int i = 0; i < 1000; i++) {
		(void)adckeys_debounce(&s, UP, 3);
	}
	CHECK(s.count == 255 && adckeys_debounce(&s, UP, 3) == UP);

	printf("adckeysTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
