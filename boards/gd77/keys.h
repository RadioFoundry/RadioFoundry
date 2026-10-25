// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 keys (docs/spec/hardware-gd77.md 9.1): the physical key bits input_gd77.c reports and the matrix positions.
// The digits and * # are placed by the keypad's layout; the six other matrix keys and the four direct keys have
// provisional names until a radio shows which is which. Pure data: tests/host/gd77KeysTest.c checks the decode. The
// tables are marked unused for the board files that only need the key bits
#ifndef GD77_KEYS_H
#define GD77_KEYS_H

#include <stdint.h>
#include "input/keymatrix/keymatrix.h"

enum {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	KEY_STAR, KEY_HASH,
	KEY_NAV_A, KEY_NAV_B, KEY_NAV_C, KEY_NAV_D,	// row PTB22, columns PTC0..PTC3 (names provisional)
	KEY_NAV_E, KEY_NAV_F,				// row PTB23, columns PTC0, PTC1
	KEY_SIDE_B1, KEY_SIDE_B9, KEY_TOP, KEY_PTT,	// direct keys PTB1, PTB9, PTA2, PTA1 (roles provisional)
	KEY_COUNT
};

#define N	KEYMATRIX_NONE
// [column driven][row read]: keymatrix_t's rows are the driven lines, its columns the read ones
__attribute__((unused)) static const uint8_t gd77_key_map[4 * 5] = {
	KEY_1, KEY_2, KEY_3, KEY_NAV_A, KEY_NAV_E,	// PTC0 low
	KEY_4, KEY_5, KEY_6, KEY_NAV_B, KEY_NAV_F,	// PTC1
	KEY_7, KEY_8, KEY_9, KEY_NAV_C, N,		// PTC2
	KEY_STAR, KEY_0, KEY_HASH, KEY_NAV_D, N,	// PTC3
};
__attribute__((unused)) static const uint8_t gd77_key_direct[5] = { N, N, N, N, N };
#undef N

__attribute__((unused)) static const keymatrix_t gd77_keymatrix = { .rows = 4, .cols = 5, .map = gd77_key_map, .direct = gd77_key_direct };

#endif
