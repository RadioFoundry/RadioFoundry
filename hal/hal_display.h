// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Display: the core draws into a frame buffer it owns and hands rows to the board to show
#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include <stdint.h>

typedef struct {
	uint16_t width;		// visible pixels
	uint16_t height;
	// 16 (RGB565, native byte order in the frame buffer), 4 (16 colors: palette indexes, two pixels a byte, high
	// nibble left; a full-resolution frame in a quarter of the RAM) or 1 (rows of bytes, MSB left)
	uint8_t bpp;
	// Safe area: pixels at each edge that are hard to see (bezel, lens). The UI draws inside them; they stay background
	uint8_t inset_top, inset_bottom, inset_left, inset_right;
	void (*init)(void);
	// Show rows y0 .. y1-1 of a full width x height frame buffer
	void (*flush)(const void *fb, uint16_t y0, uint16_t y1);
	// bpp 4 instead of flush: the same with the palette, 16 RGB565 colors the indexes stand for
	void (*flush_indexed)(const void *fb, const uint16_t *palette, uint16_t y0, uint16_t y1);
	void (*set_backlight)(uint8_t percent);
	// Optional, for the debug link: the factory's panel byte as read (-1: not read) and the controller type in use
	void (*panel)(int16_t *raw, uint8_t *type);
} hal_display_t;

#endif
