// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Display: the core draws into a frame buffer it owns and hands rows to the board to show
#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

// What a board knows of its panel, for the debug link
typedef struct {
	int16_t factory;	// the factory's panel byte as read, -1: not read
	uint8_t type;		// the controller type in use (the board's driver's numbering)
	const char *controller;	// the controller family the board recognized ("unknown" when none), or NULL
	const char *source;	// what chose the type: "id", "factory", "default" or "build", or NULL
	bool id_read;		// the panel's ID registers were read
	uint8_t id[3];		// read display ID (MIPI DCS 0x04), after its dummy byte
	uint8_t id_reg[3];	// ID1-ID3 (0xDA, 0xDB, 0xDC), each after its dummy byte
} hal_display_panel_t;

typedef struct {
	uint16_t width;		// visible pixels
	uint16_t height;
	// 16 (RGB565, native byte order in the frame buffer), 4 (16 colors: palette indexes, two pixels a byte, high
	// nibble left; a full-resolution frame in a quarter of the RAM) or 1 (rows of bytes, MSB left)
	uint8_t bpp;
	// Safe area: pixels at each edge that are hard to see (bezel, lens). The UI draws inside them; they stay background
	uint8_t inset_top, inset_bottom, inset_left, inset_right;
	// The frame buffer holds the safe area only, not the whole panel (bpp 4 only): nothing is drawn in the insets, so a
	// board whose RAM is short keeps (width x height - safe area) pixels fewer. flush_indexed then gets rows of a
	// (width - inset_left - inset_right) x (height - inset_top - inset_bottom) buffer, its row 0 the safe area's top
	// row, and shows them inside the insets; fill_insets(color) paints the four insets in the background color, which
	// gfx asks for with the first frame and whenever the background changes. Screen captures rebuild the whole panel
	// (gfx_read_rgb565, gfx_read_packed)
	bool fb_safe_area;
	void (*fill_insets)(uint16_t rgb565);
	void (*init)(void);
	// Show rows y0 .. y1-1 of a full width x height frame buffer (with fb_safe_area: of the safe area's buffer)
	void (*flush)(const void *fb, uint16_t y0, uint16_t y1);
	// bpp 4 instead of flush: the same with the palette, 16 RGB565 colors the indexes stand for
	void (*flush_indexed)(const void *fb, const uint16_t *palette, uint16_t y0, uint16_t y1);
	void (*set_backlight)(uint8_t percent);
	// Optional, for the backlight tests (Settings > Advanced > Backlight tests, CONFIG_BACKLIGHT_TEST): the PWM compare value straight, 0
	// (off) .. backlight_raw_max (on through the whole period), and the one in force. A board without them has the
	// brightness page alone
	uint16_t backlight_raw_max;
	void (*set_backlight_raw)(uint16_t value);
	uint16_t (*backlight_raw)(void);
	// Optional, with the above: the PWM rates the board can run, fastest first, backlight_rate_std its standard one.
	// Selects rate i (< backlight_rates) and returns its frequency in Hz; the compare value keeps its meaning at every
	// rate. Without them (or with one rate) there is no rate page
	uint8_t backlight_rates;
	uint8_t backlight_rate_std;
	uint32_t (*set_backlight_rate)(uint8_t i);
	// Optional, for the debug link and the support info. Fills only what the board knows: the caller clears the
	// struct first (factory -1)
	void (*panel)(hal_display_panel_t *p);
} hal_display_t;

#endif
