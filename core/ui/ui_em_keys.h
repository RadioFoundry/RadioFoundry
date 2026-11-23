// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef RFOUNDRY_UI_EM_KEYS_H
#define RFOUNDRY_UI_EM_KEYS_H

#include "config.h"
#include "ui_svc.h"
#include "codeplug/codeplug_keys.h"

typedef struct {
	bool key;
	uint8_t alert;
	uint8_t hold_s;
} ui_em_settings_t;

// Indexed by the contiguous UI setting IDs: Hold, Key, Alert.
_Static_assert(UI_SET_EM_KEY == UI_SET_EM_HOLD_S + 1 && UI_SET_EM_ALERT == UI_SET_EM_HOLD_S + 2,
	       "emergency setting row order");
static const codeplug_key_row_t ui_em_keys[] = {
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_EM_HOLD, CODEPLUG_T_UINT, CONFIG_DMR_EMERGENCY, ui_em_settings_t, hold_s, 1, UI_EM_HOLD_MAX_S),
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_EM_KEY, CODEPLUG_T_BOOL, CONFIG_DMR_EMERGENCY, ui_em_settings_t, key, 0, 1),
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_EM_ALERT, CODEPLUG_T_ENUM, CONFIG_DMR_EMERGENCY, ui_em_settings_t, alert, 0, 1),
};
#define UI_EM_KEYS_N ((uint8_t)(sizeof(ui_em_keys) / sizeof(ui_em_keys[0])))
_Static_assert(UI_EM_KEYS_N <= CODEPLUG_KEYS_MAX, "codeplug_keys_store's batch");

#endif
