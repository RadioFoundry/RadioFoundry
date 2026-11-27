// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "config.h"
#include "board.h"
#if CONFIG_RX_EXT
static bool extended;
#endif
static const band_t bands[] = {
	{ 136000000u, 174000000u, true },
	{ 400000000u, 480000000u, true },
#if CONFIG_RX_EXT
	{ 118000000u, 135999999u, false },
#endif
};
const board_t board = {
	.bands = bands, .band_count = sizeof(bands) / sizeof(bands[0]),
#if CONFIG_RX_EXT
	.rx_ext_count = 1, .rx_ext_on = &extended,
#endif
};
