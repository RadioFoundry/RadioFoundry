// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DM-32UV's band table (boards/dm32/bands_dm32.h) through the band check the radio layer and the core share
// (hal/board.h board_in_band): 480-520 MHz receives and never transmits (#852), transmit still ends at 480 MHz, and
// the switched extension (radio-features.md 6.2) starts where the receive range ends
#include <stdio.h>
#include "bands_dm32.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static bool rx_ext_on;
static const char *const names[] = { "L", "H" };
const board_t board = { .bands = dm32_bands, .band_count = sizeof dm32_bands / sizeof dm32_bands[0],
			.rx_ext_count = DM32_RX_EXT_COUNT, .rx_ext_on = &rx_ext_on, .power_count = 2, .power_names = names };

int main(void)
{
	// The rated transmit bands, unchanged
	CHECK(board_in_band(136000000, true) && board_in_band(174000000, true));
	CHECK(board_in_band(400000000, true) && board_in_band(446000000, true) && board_in_band(480000000, true));

	// 480-520 MHz: receive, with the extension off, and never transmit
	CHECK(!rx_ext_on);
	CHECK(board_in_band(480000001, false) && !board_in_band(480000001, true));
	CHECK(board_in_band(500000000, false) && !board_in_band(500000000, true));
	CHECK(board_in_band(506937500, false) && !board_in_band(506937500, true));	// a UHF-T channel
	CHECK(board_in_band(520000000, false) && !board_in_band(520000000, true));
	CHECK(board_tx_band(500000000) < 0);
	CHECK(!board_in_band(520000001, false));	// past it: only with the extension

	// The extension: 520-740 receive only while it is on, and it doesn't make 480-520 transmit
	board_rx_ext_set(true);
	CHECK(board_in_band(520000001, false) && board_in_band(740000000, false) && !board_in_band(740000001, false));
	CHECK(!board_in_band(600000000, true) && !board_in_band(500000000, true));
	CHECK(board_in_band(300000000, false) && !board_in_band(300000000, true));
	board_rx_ext_set(false);
	CHECK(!board_in_band(600000000, false) && board_in_band(500000000, false));

	// Every transmit entry stays inside 136-174 / 400-480 MHz, whatever is added later
	for (unsigned i = 0; i < sizeof dm32_bands / sizeof dm32_bands[0]; i++) {
		const band_t *b = &dm32_bands[i];
		if (b->tx) {
			CHECK((b->lo_hz >= 136000000 && b->hi_hz <= 174000000) || (b->lo_hz >= 400000000 && b->hi_hz <= 480000000));
		}
	}
	// The extension is the last DM32_RX_EXT_COUNT entries, all receive only
	for (unsigned i = sizeof dm32_bands / sizeof dm32_bands[0] - DM32_RX_EXT_COUNT; i < sizeof dm32_bands / sizeof dm32_bands[0]; i++) {
		CHECK(!dm32_bands[i].tx);
	}

	if (failures) {
		printf("dm32BandTest: %d failures\n", failures);
		return 1;
	}
	printf("dm32BandTest: all passed\n");
	return 0;
}
