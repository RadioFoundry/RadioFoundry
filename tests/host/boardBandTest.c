// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The band check every radio layer shares (hal/board.h board_in_band): edges, gaps, receive-only ranges, and a
// frequency in two overlapping bands where only the second may transmit. board_power_name: a level's label per band,
// power_names outside every band, "?" past the levels
#include <stdio.h>
#include <string.h>
#include "board.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 108000000, 136000000, false },	// receive only
	{ 400000000, 480000000, false },	// receive only, overlapped by the next
	{ 430000000, 450000000, true },
};
static const char *const names[] = { "L", "M", "H" };
static const char *const names_vhf[] = { "250mW", "5W", "50W" };
static const char *const names_uhf[] = { "50mW", "5W", "45W" };
static const board_power_band_t power_bands[] = {
	{ 136000000, 174000000, names_vhf },
	{ 400000000, 480000000, names_uhf },
};
const board_t board = { .bands = bands, .band_count = sizeof bands / sizeof bands[0], .power_count = 3,
			.power_names = names, .power_bands = power_bands, .power_band_count = 2 };

int main(void)
{
	CHECK(board_in_band(136000000, true) && board_in_band(174000000, true));
	CHECK(!board_in_band(135999999, true) && board_in_band(135999999, false));
	CHECK(!board_in_band(174000001, false) && !board_in_band(0, false));
	CHECK(board_in_band(120000000, false) && !board_in_band(120000000, true));
	// in the receive-only band first and in the transmit one after it: may transmit
	CHECK(board_in_band(440000000, true) && board_in_band(440000000, false));
	CHECK(board_in_band(460000000, false) && !board_in_band(460000000, true));
	// power level names per band
	CHECK(strcmp(board_power_name(2, 146520000), "50W") == 0 && strcmp(board_power_name(2, 446100000), "45W") == 0);
	CHECK(strcmp(board_power_name(0, 136000000), "250mW") == 0 && strcmp(board_power_name(0, 480000000), "50mW") == 0);
	CHECK(strcmp(board_power_name(1, 300000000), "M") == 0);	// in no band: power_names
	CHECK(strcmp(board_power_name(3, 446100000), "?") == 0);
	if (failures) {
		printf("boardBandTest: %d failures\n", failures);
		return 1;
	}
	printf("boardBandTest: all passed\n");
	return 0;
}
