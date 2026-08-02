// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Display: the core draws into a frame buffer it owns and hands rows to the board to show
#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include <stdint.h>

typedef struct {
	uint16_t width;		// visible pixels
	uint16_t height;
	uint8_t bpp;		// 16 (RGB565, native byte order in the frame buffer) or 1 (rows of bytes, MSB left)
	// Safe area: pixels at each edge that are hard to see (bezel, lens). The UI draws inside them; they stay background
	uint8_t inset_top, inset_bottom, inset_left, inset_right;
	void (*init)(void);
	// Show rows y0 .. y1-1 of a full width x height frame buffer
	void (*flush)(const void *fb, uint16_t y0, uint16_t y1);
	void (*set_backlight)(uint8_t percent);
} hal_display_t;

#endif
