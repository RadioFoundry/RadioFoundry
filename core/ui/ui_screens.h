// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Screen drawing (docs/ux/wireframes): layout from the display size, no task or input code, so host tests can render
#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/radio.h"
#include "font.h"
#include "gfx.h"

#define UI_ENTRY_MAX	8	// VFO entry digits: 3 MHz digits and 5 decimals (10 Hz)
#define UI_ENTRY_MHZ	3
#define UI_QUICK_TILES	9

// What the screens show besides the radio view; owned by the UI task
typedef struct {
	uint32_t now_ms;
	uint8_t battery_pct;
	bool entry;			// VFO frequency entry in progress
	uint8_t entry_len;
	char entry_digits[UI_ENTRY_MAX + 1];
	const char *toast;		// one-line message over the hint row, NULL = none
	uint8_t quick_focus;		// 0..8 = tiles 1..9
	bool quick_edit;		// the focused tile's value editor is open
	uint8_t brightness;		// backlight percent chosen in quick settings
} ui_state_t;

enum { QT_POWER, QT_SQUELCH, QT_TONE, QT_MODE, QT_MONITOR, QT_VOX, QT_VFO, QT_BANDWIDTH, QT_BRIGHTNESS };

// Fonts and region positions for the current display, from gfx_width() / gfx_height()
typedef struct {
	const font_t *tiny, *small, *medium, *large;
	int strip_h;
	int zone_y;		// < 0: no zone line on this display
	int channel_y, talk_y, chips_y;
	int sep_y;		// < 0: no separator
	int activity_y, activity_h;
	int hint_y;
} ui_layout_t;

extern ui_layout_t ui_layout;

// Pick fonts and place the regions; call after gfx_init
void ui_layout_init(void);

void ui_draw_splash(const char *product, const char *board_name, const char *git);
void ui_draw_home(const radio_view_t *v, const ui_state_t *s);
void ui_draw_quick(const radio_view_t *v, const ui_state_t *s);
// Diagnostics: raw key bits, encoder total, radio readings and the board's own lines
void ui_draw_diag(const radio_view_t *v, uint32_t keys, int32_t enc_total);

// Quick settings tile: false if it can't be used yet. label and value get short texts
bool ui_quick_tile(int tile, const radio_view_t *v, const ui_state_t *s, char *label, size_t label_len,
	char *value, size_t value_len);

// Text for the board's bands ("136-174 400-480"), for hints
void ui_bands_text(char *out, size_t len);

#endif
