// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-32UV band table (board.c's board_t.bands), in a header of its own so the host test (tests/dm32BandTest.c) checks
// the table the radio runs rather than a copy of it.
//
// Receive goes to 520 MHz, transmit stops at 480 (#852). The stock CPS offers a 400-520 MHz UHF range, and on the
// classic RF section the FD6818's synthesizer reported lock at every frequency tried from 136 to 520 MHz
// (hardware-dm32.md 8.10). 480-520 needs no register value the radio doesn't already write: the FD6818's band row
// from 370 MHz up (8.3) covers 400-740 MHz with one pair of band words, set-up B writes fixed ones, and the UHF LNA
// is on from 280 MHz up (radio_dm32.c lna). Transmit stays at 400-480: the PA's factory calibration has points only
// across 400-470 (11.3), and nothing past 480 has been measured on air. A transmit band entry never extends past
// what the radio is rated for, so the 480-520 entry is receive only, and the radio layer refuses to key there
// (radio_dm32.c tx, board_in_band with need_tx)
#ifndef DM32_BANDS_H
#define DM32_BANDS_H

#include "board.h"
#include "config.h"

// The last entries, in force only while Settings > Advanced > RX past bands is on (radio-features.md 6.2)
#define DM32_RX_EXT_COUNT	3u

static const band_t dm32_bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
	{ 480000000, 520000000, false },	// receive only, always: the range the stock firmware receives (above)
#if CONFIG_AM_RX
	{ 108000000, 136000000, false },	// the air band, AM receive only (docs/spec/airband-and-broadcast.md 3)
#endif
#if CONFIG_BCAST_FM
	{ 65000000, 108000000, false },		// the broadcast band, on the RDA5807M (airband-and-broadcast.md 4)
#endif
#if CONFIG_RX_EXT
	// Receive only past the rated bands, while the user turns it on: the FD6818 band table (hardware-dm32.md 8.3).
	// Keep these last, DM32_RX_EXT_COUNT of them
	{ 16000000, 65000000, false },
	{ 174000000, 400000000, false },
	{ 520000000, 740000000, false },
#endif
};

#endif
