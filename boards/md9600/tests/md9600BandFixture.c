// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Rated and optional receive bands for the frequency-plan and RF-sequencing host tests.
#include "config.h"
#include "board.h"
#if CONFIG_RX_EXT
static bool rx_ext_on;
#endif
static const band_t bands[] = {
 {136000000, 174000000, true}, {400000000, 480000000, true},
#if CONFIG_RX_EXT
 {500000000, 503000000, false},
#endif
};
const board_t board = {
 .bands = bands, .band_count = sizeof(bands) / sizeof(bands[0]),
#if CONFIG_RX_EXT
 .rx_ext_count = 1, .rx_ext_on = &rx_ext_on,
#endif
};
