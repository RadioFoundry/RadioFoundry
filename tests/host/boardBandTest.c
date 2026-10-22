// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The band check every radio layer shares (hal/board.h board_in_band): edges, gaps, receive-only ranges, and a
// frequency in two overlapping bands where only the second may transmit
#include <stdio.h>
#include "board.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 108000000, 136000000, false },	// receive only
	{ 400000000, 480000000, false },	// receive only, overlapped by the next
	{ 430000000, 450000000, true },
};
const board_t board = { .bands = bands, .band_count = sizeof bands / sizeof bands[0] };

int main(void)
{
	CHECK(board_in_band(136000000, true) && board_in_band(174000000, true));
	CHECK(!board_in_band(135999999, true) && board_in_band(135999999, false));
	CHECK(!board_in_band(174000001, false) && !board_in_band(0, false));
	CHECK(board_in_band(120000000, false) && !board_in_band(120000000, true));
	// in the receive-only band first and in the transmit one after it: may transmit
	CHECK(board_in_band(440000000, true) && board_in_band(440000000, false));
	CHECK(board_in_band(460000000, false) && !board_in_band(460000000, true));
	if (failures) {
		printf("boardBandTest: %d failures\n", failures);
		return 1;
	}
	printf("boardBandTest: all passed\n");
	return 0;
}
