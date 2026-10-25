// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ST7567 / UC1701-class 128x64 monochrome LCD controller on a byte-wide serial bus with a data/command line
// (docs/spec/hardware-dm1801pro.md 3). Generic: the board supplies the bus and the panel's orientation. The controller
// is page addressed: a page is 8 pixel rows, one byte per column with the top row in bit 0. The core's 1 bpp frame
// buffer holds rows of bytes, most significant bit left (hal_display.h), so each page is a transposition.
#ifndef ST7567_H
#define ST7567_H

#include <stdint.h>
#include <stdbool.h>

#define ST7567_WIDTH		128u
#define ST7567_HEIGHT		64u
#define ST7567_PAGES		(ST7567_HEIGHT / 8u)

typedef struct {
	void (*begin)(void);			// take the bus (chip select low)
	void (*end)(void);
	void (*cmd)(uint8_t c);			// one byte with D/C low
	void (*data)(const uint8_t *d, uint32_t n);	// bytes with D/C high
	void (*reset)(bool level);		// reset pin level (low = in reset)
	void (*delay_ms)(uint32_t ms);
} st7567_bus_t;

typedef struct {
	const st7567_bus_t *bus;
	uint8_t col_offset;	// first visible column of the controller's 132 (the board's)
	bool seg_reverse;	// mirror left / right
	bool com_reverse;	// mirror top / bottom
	uint8_t regulator;	// internal regulator ratio, 0..7
	uint8_t contrast;	// electronic volume, 0..63
	uint8_t bias;		// 0 = 1/9, 1 = 1/7
} st7567_t;

// Reset, configuration, clear the display memory, display on
void st7567_init(const st7567_t *p);
// Send frame buffer rows y0 .. y1-1 of a 128 x 64 1 bpp buffer: every page the rows touch, whole
void st7567_write_rows(const st7567_t *p, const uint8_t *fb, uint16_t y0, uint16_t y1);
void st7567_set_contrast(const st7567_t *p, uint8_t contrast);
void st7567_display_on(const st7567_t *p, bool on);

// One page of the frame buffer as column bytes (pure, for tests)
void st7567_page_bytes(const uint8_t *fb, uint8_t page, uint8_t out[ST7567_WIDTH]);

#endif
