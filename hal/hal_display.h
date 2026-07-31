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
	void (*init)(void);
	// Show rows y0 .. y1-1 of a full width x height frame buffer
	void (*flush)(const void *fb, uint16_t y0, uint16_t y1);
	void (*set_backlight)(uint8_t percent);
} hal_display_t;

#endif
