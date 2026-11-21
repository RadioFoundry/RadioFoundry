// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <assert.h>
#include <stdio.h>
#include "board.h"
#include "fuzz.h"
#include "../pins.h"
#if defined(PIN_GPS_POWER) || defined(PIN_GPS_RXD)
#error "Unresolved board nets must not inherit peripheral assignments"
#endif
_Static_assert(PIN_BCAST_SDA == 0x19 && PIN_BCAST_SCL == 0x1A, "Broadcast receiver bus");
_Static_assert(PIN_BCAST_EN == 0x1D && !BCAST_EN_ON_LEVEL, "Broadcast receiver enable, low = on");
_Static_assert(PIN_BT_ENABLE == 0x30, "Bluetooth enable");
_Static_assert(PIN_ENC_A == 0x27 && PIN_ENC_B == 0x26, "Channel encoder inputs");
_Static_assert(PIN_LCD_RESET == 0x42 && PIN_LCD_RD == 0x46 && PIN_KEY_ROW0 == 0x47, "Shared LCD bus");
#define CONFIG_AM_RX 1
#define CONFIG_BCAST_FM 1
#define CONFIG_RX_EXT 1
#include "../bands.h"
static bool rx_ext_on;
const board_t board = {
	.bands = bands, .band_count = sizeof bands / sizeof bands[0],
	.rx_ext_count = 3, .rx_ext_on = &rx_ext_on,
};
// Transmit only inside the two rated bands (hardware-dm36.md 8), edges included; never in the receive-only bands
static bool rated(uint32_t hz)
{
	return (hz >= 136000000u && hz <= 174000000u) || (hz >= 400000000u && hz <= 480000000u);
}

int main(void)
{
	fuzz_t f;
	fuzz_init(&f, 0x36);
	unsigned checks = 0;
	for (unsigned ext = 0; ext < 2; ext++) {
		board_rx_ext_set(ext != 0);
		for (unsigned i = 0; i < board.band_count; i++) {
			assert(bands[i].tx == rated(bands[i].lo_hz / 2 + bands[i].hi_hz / 2));	// an edge can be shared
			const uint32_t edge[] = { bands[i].lo_hz - 1, bands[i].lo_hz, bands[i].hi_hz, bands[i].hi_hz + 1 };
			for (unsigned j = 0; j < sizeof edge / sizeof edge[0]; j++) {
				assert(board_in_band(edge[j], true) == rated(edge[j]));
				assert((board_tx_band(edge[j]) >= 0) == rated(edge[j]));
				checks += 2;
			}
		}
		assert(board_in_band(146520000, true) && board_in_band(446000000, true));
		assert(!board_in_band(121500000, true) && !board_in_band(98000000, true));
		assert(!board_in_band(0, true) && !board_in_band(UINT32_MAX, true));
		for (long i = 0; i < fuzz_iters(10000); i++) {
			uint32_t hz = fuzz_next(&f);
			assert(board_in_band(hz, true) == rated(hz));
			assert((board_tx_band(hz) >= 0) == rated(hz));
			checks += 2;
		}
	}
	printf("txBandsTest: %u TX checks plus edge assertions passed\n", checks);
	return 0;
}
