// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// ST7789-class 240x320 color TFT controller on an 8-bit 8080 bus (docs/spec/hardware-dm32.md 7). Generic: the board
// supplies the bus and the panel's orientation. The frame buffer may be smaller than the panel by an integer factor:
// every pixel is then sent as a scale x scale block (the core's frame buffer is limited to 40 KB, core/ui/gfx.h).
#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	void (*begin)(void);			// take the bus
	void (*end)(void);
	void (*cmd)(uint8_t c);			// one byte with D/C low
	void (*data)(const uint8_t *d, uint32_t n);	// bytes with D/C high
	void (*pixels)(const uint16_t *px, uint32_t n);	// RGB565, each sent high byte first
	void (*reset)(bool level);		// reset pin level (low = in reset)
	void (*delay_ms)(uint32_t ms);
} st7789_bus_t;

#define ST7789_MAX_LINE		320u	// longest panel row in pixels

typedef struct {
	const st7789_bus_t *bus;
	uint16_t width, height;		// panel pixels in the chosen orientation (240 x 320 portrait, 320 x 240 landscape)
	uint8_t madctl;			// memory access control (0xC0: portrait turned 180 degrees, the DM-32's value)
	bool invert;			// display inversion for normal colors (the DM-32 panel needs it)
	uint8_t scale;			// frame buffer pixel -> scale x scale panel pixels (1 or more)
} st7789_t;

// Reset, init table, clear the frame memory to black, display on
void st7789_init(const st7789_t *p);
// Send frame buffer rows y0 .. y1-1 of a (width / scale) x (height / scale) RGB565 buffer (native uint16 order)
void st7789_write_rows(const st7789_t *p, const uint16_t *fb, uint16_t y0, uint16_t y1);
void st7789_fill(const st7789_t *p, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void st7789_invert(const st7789_t *p, bool on);
void st7789_display_on(const st7789_t *p, bool on);

// The init command stream (for tests): bytes as { cmd, n, data[n] } entries, ended by cmd 0
extern const uint8_t st7789_init_seq[];

#endif
