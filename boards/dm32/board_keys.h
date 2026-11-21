// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Five of the power levels 0..8 of hardware-dm32.md 11.3: very low, low, mid, a step above mid, high (radio_dm32.c).
// Labels are nominal: the radio is rated 5 W VHF / 4 W UHF at high power; check against a meter at the bring-up
static const char *const power_names[] = { "L-", "L", "M", "M+", "H" };

// docs/ux/information-architecture.md 7.1 (the DM-32 has no top key; SK1 / SK2 are the side keys below the PTT)
static const keymap_entry_t keymap[] = {
	{ KEY_GREEN, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ KEY_RED, ACT_BACK, ACT_BACK, false, false, 0 },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, 0 },
	{ KEY_LEFT, ACT_NAV_LEFT, ACT_USER_1, false, true, 0 },
	{ KEY_RIGHT, ACT_NAV_RIGHT, ACT_USER_4, false, true, 0 },
	{ KEY_STAR, ACT_QUICK, ACT_QUICK, false, false, '*' },
	{ KEY_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false, '#' },
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
	{ KEY_SK1, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ KEY_SK2, ACT_AB, ACT_USER_2, false, false, 0 },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): the side keys top to bottom
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2 };
static const char *const side_key_names[] = { "SK1", "SK2" };
// The arrows left and right of the 4-way key are printed P1 and P2: programmable on Home, arrows in the menus
static const uint8_t home_keys[] = { KEY_LEFT, KEY_RIGHT };
static const char *const home_key_names[] = { "P1", "P2" };
