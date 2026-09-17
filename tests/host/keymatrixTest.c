// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Key matrix decode with column-to-ground keys (DM-32 SK1 / SK2 on columns 3 / 4) (docs/spec/hardware-dm32.md 6.1,
// 6.2; the encoder is quadratureTest's)
#include <stdio.h>
#include "input/keymatrix/keymatrix.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The DM-32 layout with its own key numbers (boards/dm32/input_dm32.c uses the board's enum)
enum { K0, K1, K2, K3, K4, K5, K6, K7, K8, K9, STAR, HASH, UP, DOWN, LEFT, RIGHT, GREEN, RED, SK1, SK2 };
#define N KEYMATRIX_NONE
static const uint8_t map[5 * 4] = {
	GREEN, K3, K2, K1,
	LEFT, K6, K5, K4,
	RIGHT, K9, K8, K7,
	RED, HASH, K0, STAR,
	UP, DOWN, N, N,
};
static const uint8_t direct[4] = { N, N, SK1, SK2 };
static const keymatrix_t m = { 5, 4, map, direct };

static uint32_t decode(uint8_t idle, uint8_t r0, uint8_t r1, uint8_t r2, uint8_t r3, uint8_t r4)
{
	const uint8_t rows[5] = { r0, r1, r2, r3, r4 };
	return keymatrix_decode(&m, idle, rows);
}

int main(void)
{
	CHECK(decode(0, 0, 0, 0, 0, 0) == 0);
	CHECK(decode(0, 0x01, 0, 0, 0, 0) == 1u << GREEN);
	CHECK(decode(0, 0x08, 0, 0, 0, 0) == 1u << K1);
	CHECK(decode(0, 0, 0, 0, 0x04, 0) == 1u << K0);
	CHECK(decode(0, 0, 0, 0, 0, 0x02) == 1u << DOWN);
	CHECK(decode(0, 0, 0, 0, 0, 0x0C) == 0);		// row 4 has no keys in columns 3 / 4
	CHECK(decode(0, 0x01, 0, 0, 0x01, 0) == ((1u << GREEN) | (1u << RED)));
	// SK1 holds column 3 low with no row driven: it reads low in every row too, none of which counts
	CHECK(decode(0x04, 0x04, 0x04, 0x04, 0x04, 0x04) == 1u << SK1);
	// ... other columns still scan
	CHECK(decode(0x04, 0x05, 0x04, 0x04, 0x04, 0x04) == ((1u << SK1) | (1u << GREEN)));
	CHECK(decode(0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C) == ((1u << SK1) | (1u << SK2)));
	// A column low at idle with no direct key (a stuck line) reports nothing for that column
	CHECK(decode(0x01, 0x01, 0x01, 0x01, 0x01, 0x01) == 0);
	if (failures) {
		printf("keymatrixTest: %d failures\n", failures);
		return 1;
	}
	printf("keymatrixTest: all passed\n");
	return 0;
}
