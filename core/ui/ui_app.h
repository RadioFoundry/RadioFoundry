// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI's behavior without the RTOS: one navigation stack of screens, logical actions in, drawing out
// (docs/ux/information-architecture.md, docs/spec/ui.md). ui.c runs it in the UI task; the host simulator
// (tests/host/sim/ui_sim.c) runs the same code against a simulated radio, codeplug and display.
#ifndef UI_APP_H
#define UI_APP_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_input.h"
#include "radio/radio.h"

// Call once after gfx_init and ui_layout_init (reads the board's key map for fallbacks and hint texts)
void ui_app_init(void);
// One logical input event (PTT / MONITOR edges included: the radio core gets those from the input layer directly)
void ui_app_key(const input_event_t *ev);
// Time passes and the radio view may have changed: timeouts, banners, Recent. True if the screen should be redrawn
bool ui_app_tick(const radio_view_t *v, uint32_t now_ms);
// Draw the current screen into the frame buffer (no flush)
void ui_app_draw(void);
// Backlight percent the user chose (Quick settings / Settings > Display) and the time-out in seconds (0 = never)
uint8_t ui_app_brightness(void);
uint16_t ui_app_backlight_s(void);

// For the debug link and the simulator: what is on screen
typedef struct {
	const char *screen;	// "home", "quick", "hub", "find", ...
	uint8_t depth;		// 0 = Home
	uint16_t focus;		// focused row / tile
	uint16_t count;		// rows on a list screen
	bool locked;
	bool field_use;		// the screen is tagged "field use" (depth rule, usability-tests.md 2)
	const char *banner;	// banner title, "" = none
	const char *toast;	// "" = none
	const char *query;	// Find text so far, "" = none
	uint32_t draws;		// frames drawn so far
	bool strip_drawn;	// the last frame drew the status strip with the state pill or a merged title (P2)
} ui_app_status_t;
void ui_app_status(ui_app_status_t *st);
// The label of the focused row on a list screen ("" otherwise), for scripted checks
const char *ui_app_focus_label(void);

#endif
