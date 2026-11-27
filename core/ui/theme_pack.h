// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 RadioFoundry Theme Pack contributors
#ifndef UI_THEME_PACK_H
#define UI_THEME_PACK_H

#include <stdbool.h>
#include <stdint.h>

#define THEME_PACK_MAX_THEMES 10u
#define THEME_PACK_NAME_LEN 24u
#define THEME_PACK_COLOR_COUNT 18u

// Load and validate the complete pack from the board's dedicated area. Invalid or absent data exposes no pack themes.
void ui_theme_pack_init(void);
bool ui_theme_pack_reload(void);
bool ui_theme_pack_supported(void);
uint8_t ui_theme_pack_count(void);
bool ui_theme_pack_get(uint8_t index, char name[THEME_PACK_NAME_LEN], uint16_t colors[THEME_PACK_COLOR_COUNT]);
bool ui_theme_pack_transfer_begin(uint16_t length);
bool ui_theme_pack_transfer_data(uint16_t offset, const uint8_t *data, uint16_t length);
bool ui_theme_pack_transfer_commit(void);
bool ui_theme_pack_remove(void);

#endif
