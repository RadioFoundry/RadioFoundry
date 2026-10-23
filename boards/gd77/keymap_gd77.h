// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 key map (docs/ux/information-architecture.md 7.3): the physical keys of keys.h to the logical actions. The
// roles are the board's configuration and stay provisional until a radio shows which matrix position is which key
// (hardware-gd77.md 9.1, the manual key test in tests/hil/test_gd77.py): the names of the six navigation keys follow
// their likeliest reading, and only this table changes when they turn out otherwise. Pure data: tests/host checks the
// rules every key map follows (information-architecture.md 2).
#ifndef GD77_KEYMAP_H
#define GD77_KEYMAP_H

#include <stdint.h>
#include "hal_input.h"
#include "keys.h"

// The top key: short is the reassignable top key role (power level by default), held is emergency where it is built,
// else the same role's long function, as on the DM-1701
#if defined(CONFIG_DMR_EMERGENCY) && CONFIG_DMR_EMERGENCY
#define GD77_TOP_LONG	ACT_EMERGENCY
#else
#define GD77_TOP_LONG	ACT_USER_3
#endif

// Matrix position -> physical key (provisional): A green, B red, C up, D down, E and F the left / right pair
#define GD77_KEY_GREEN	KEY_NAV_A
#define GD77_KEY_RED	KEY_NAV_B
#define GD77_KEY_UP	KEY_NAV_C
#define GD77_KEY_DOWN	KEY_NAV_D
#define GD77_KEY_LEFT	KEY_NAV_E
#define GD77_KEY_RIGHT	KEY_NAV_F

__attribute__((unused)) static const keymap_entry_t gd77_keymap[] = {
	{ GD77_KEY_GREEN, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ GD77_KEY_RED, ACT_BACK, ACT_BACK, false, false, 0 },
	{ GD77_KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ GD77_KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, 0 },
	{ GD77_KEY_LEFT, ACT_NAV_LEFT, ACT_USER_1, false, true, 0 },
	{ GD77_KEY_RIGHT, ACT_NAV_RIGHT, ACT_USER_4, false, true, 0 },
	{ KEY_STAR, ACT_QUICK, ACT_QUICK, false, false, 0 },
	{ KEY_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false, 0 },
	{ KEY_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false, 0 },
	{ KEY_1, ACT_DIGIT_1, ACT_DIGIT_1, false, false, 0 },
	{ KEY_2, ACT_DIGIT_2, ACT_DIGIT_2, false, false, 0 },
	{ KEY_3, ACT_DIGIT_3, ACT_DIGIT_3, false, false, 0 },
	{ KEY_4, ACT_DIGIT_4, ACT_DIGIT_4, false, false, 0 },
	{ KEY_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false, 0 },
	{ KEY_6, ACT_DIGIT_6, ACT_DIGIT_6, false, false, 0 },
	{ KEY_7, ACT_DIGIT_7, ACT_DIGIT_7, false, false, 0 },
	{ KEY_8, ACT_DIGIT_8, ACT_DIGIT_8, false, false, 0 },
	{ KEY_9, ACT_DIGIT_9, ACT_DIGIT_9, false, false, 0 },
	{ KEY_SIDE_B1, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ KEY_SIDE_B9, ACT_TALK_TARGET, ACT_USER_2, false, false, 0 },
	{ KEY_TOP, ACT_USER_3, GD77_TOP_LONG, false, false, 0 },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};

#define GD77_KEYMAP_LEN	(sizeof(gd77_keymap) / sizeof(gd77_keymap[0]))

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h), in role order: side key 1, side key 2,
// top key
__attribute__((unused)) static const uint8_t gd77_side_keys[] = { KEY_SIDE_B1, KEY_SIDE_B9, KEY_TOP };

#endif
