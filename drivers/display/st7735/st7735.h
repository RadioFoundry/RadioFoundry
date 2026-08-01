// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// ST7735 / HX8353-class 8080-bus color TFT controllers, and the variant that needs the 0xFE/0xEF inter-command
// unlock (docs/spec/hardware-dm1701.md 9.2, 9.3). Generic: the board supplies the bus and knows the pins.
#ifndef ST7735_H
#define ST7735_H

#include <stdint.h>
#include <stdbool.h>

// Bus operations. Every transfer happens between begin() and end(); the driver never sleeps inside a session
typedef struct {
	void (*begin)(void);			// take the bus and select the panel
	void (*end)(void);			// deselect the panel and release the bus
	void (*cmd)(uint8_t c);			// one byte with D/C low
	void (*data)(const uint8_t *d, uint32_t n);	// bytes with D/C high
	// RGB565 pixels with D/C high, each sent high byte first; swap_rb exchanges the red and blue fields
	void (*pixels)(const uint16_t *px, uint32_t n, bool swap_rb);
	void (*reset)(bool level);		// reset pin level (low = in reset)
	void (*delay_ms)(uint32_t ms);
} st7735_bus_t;

// Panel types as the factory stores them (low two bits):
//   0: ST7735-class, normal          1: ST7735-class, rotated 180 degrees
//   2: unlock variant, flipped, RGB   3: unlock variant, normal, BGR
typedef struct {
	const st7735_bus_t *bus;
	uint8_t type;
	uint16_t width, height;		// visible area
	uint16_t col_offset, row_offset;	// panel address of visible pixel (0, 0)
	uint8_t madctl_xor;			// board correction applied on top of the type's MADCTL (0x40: flip vertically in landscape, 0x80: horizontally)
	bool force_rgb;				// RGB order even for type 3 (which is BGR by default)
} st7735_t;

// Reset the panel, run the init table for its type, clear the whole panel (hidden rows included) and turn it on
void st7735_init(const st7735_t *p);

// Send rows y0 .. y1-1 of a width x height RGB565 frame buffer (native uint16 order)
void st7735_write_rows(const st7735_t *p, const uint16_t *fb, uint16_t y0, uint16_t y1);

// Fill a rectangle of panel addresses (offsets not applied: for clearing hidden rows); x1, y1 exclusive
void st7735_fill_raw(const st7735_t *p, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);

void st7735_invert(const st7735_t *p, bool on);
void st7735_display_on(const st7735_t *p, bool on);

#endif
