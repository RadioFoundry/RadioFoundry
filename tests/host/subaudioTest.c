// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Sub-audio math shared by the RF drivers: the DCS Golay word
#include <stdio.h>
#include <stdbool.h>
#include "rf/subaudio/subaudio.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int popcount23(uint32_t x)
{
	int n = 0;
	for (; x; x &= x - 1)
		n++;
	return n;
}

static uint32_t rot23(uint32_t x, int k)
{
	return ((x << k) | (x >> (23 - k))) & 0x7FFFFFu;
}

// DCS words: the datasheet value, the Golay(23,12) distance, and the known normal / inverted equivalences
static void test_dcs_word(void)
{
	// Datasheet example (AT1846S and FD6818): DCS 023 -> 0x763813
	CHECK(subaudio_dcs_word(023) == 0x763813);
	for (uint16_t code = 0; code < 0x200; code++) {
		uint32_t w = subaudio_dcs_word(code);
		CHECK((w & 0xFFFu) == (0x800u | code));
		CHECK(w < (1u << 23));
	}

	// Each word is a Golay codeword: any two differ in at least 7 bits
	int min_d = 23;
	for (uint16_t a = 0; a < 0x200; a++) {
		uint32_t wa = subaudio_dcs_word(a);
		for (uint16_t b = a + 1; b < 0x200; b++) {
			int d = popcount23(wa ^ subaudio_dcs_word(b));
			if (d < min_d)
				min_d = d;
		}
	}
	CHECK(min_d >= 7);

	// A receiver sees the inverted word as some rotation of another code's word: D023N = D047I and so on
	static const uint16_t pairs[][2] = { { 023, 047 }, { 025, 0244 }, { 026, 0464 }, { 0114, 0712 } };
	for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
		uint32_t inv = subaudio_dcs_word(pairs[i][0]) ^ 0x7FFFFFu;
		uint32_t other = subaudio_dcs_word(pairs[i][1]);
		bool found = false;
		for (int k = 0; k < 23; k++)
			found |= rot23(inv, k) == other;
		CHECK(found);
	}

	// Register split for a few codes (independently computed)
	CHECK(subaudio_dcs_word(025) == 0x6B7815);
	CHECK(subaudio_dcs_word(0754) == 0x20F9EC);
}

int main(void)
{
	test_dcs_word();
	if (failures) {
		printf("# FAILED %d\n", failures);
		return 1;
	}
	printf("# PASSED subaudioTest\n");
	return 0;
}
