// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ILI9341-class 320x240 color TFT controllers on an 8-bit 8080 bus, in the variants one radio may be fitted with
// (docs/spec/hardware-gd88.md 5): three command-style (ILI9341) init sets and one register-style (ILI932x) panel.
// Generic: the board supplies the bus and picks the variant. The frame buffer is 16 colors (4 bits a pixel) with a
// palette, sent as RGB565.
#ifndef ILI9341_H
#define ILI9341_H

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
} ili9341_bus_t;

#define ILI9341_MAX_LINE	320u	// longest panel row in pixels

// The init sets by the variant number the panel record gives (0..3). 1 is the register-style panel
enum {
	ILI9341_VARIANT_0 = 0,
	ILI9341_VARIANT_REG = 1,
	ILI9341_VARIANT_2 = 2,
	ILI9341_VARIANT_3 = 3,
	ILI9341_VARIANTS
};

typedef struct {
	const ili9341_bus_t *bus;
	uint16_t width, height;		// panel pixels as drawn (320 x 240 landscape)
	uint8_t variant;		// ILI9341_VARIANT_*
} ili9341_t;

// Reset, the variant's init set, sleep out, the frame memory cleared to black, display on
void ili9341_init(const ili9341_t *p);
// Send rows y0 .. y1-1 of a width x height 16-color frame buffer: two pixels a byte (high nibble left), palette of
// 16 RGB565 colors
void ili9341_write_rows_indexed(const ili9341_t *p, const uint8_t *fb, const uint16_t *palette, uint16_t y0, uint16_t y1);
void ili9341_fill(const ili9341_t *p, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void ili9341_display_on(const ili9341_t *p, bool on);

// The command-style init streams (for tests): { cmd, n, data[n] } entries, ended by cmd 0. NULL for the register style
const uint8_t *ili9341_init_seq(uint8_t variant);
// The register-style init (for tests): { reg, value } pairs; reg 0xFF is a delay of value ms; ended by reg 0xFE
extern const uint16_t ili9341_reg_seq[];

#endif
