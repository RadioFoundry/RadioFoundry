// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// ILI9341-class 320x240 TFT controllers (ili9341.h). The command-style variants share the window commands (CASET 0x2A,
// RASET 0x2B, RAMWR 0x2C). The register-style panel takes a 16-bit register index and 16-bit values (two bytes each on
// the 8-bit bus) and has its own window (R50-R53) and frame memory address (R20 / R21) registers.
#include <stddef.h>
#include "ili9341.h"

#define CMD_SLPOUT	0x11
#define CMD_DISPOFF	0x28
#define CMD_DISPON	0x29
#define CMD_CASET	0x2A
#define CMD_RASET	0x2B
#define CMD_RAMWR	0x2C

#define REG_H_ADDR	0x20u	// frame memory address, short side (0..239)
#define REG_V_ADDR	0x21u	// frame memory address, long side (0..319)
#define REG_GRAM	0x22u
#define REG_DISP_CTRL	0x07u
#define REG_H_START	0x50u
#define REG_H_END	0x51u
#define REG_V_START	0x52u
#define REG_V_END	0x53u
#define REG_DELAY	0xFFu
#define REG_END		0xFEu

#define RESET_LOW_MS	10u
#define RESET_HIGH_MS	130u
#define SLPOUT_MS	120u

// Variant 3
static const uint8_t seq3[] = {
	0xCF, 3, 0x00, 0x81, 0x30,
	0xED, 4, 0x64, 0x03, 0x12, 0x81,
	0xE8, 3, 0x85, 0x10, 0x7A,
	0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
	0xF6, 2, 0x01, 0x30,
	0xF7, 1, 0x20,
	0xEA, 2, 0x00, 0x00,
	0xC0, 1, 0x21,
	0xC1, 1, 0x11,
	0xC5, 2, 0x3F, 0x3C,
	0x3A, 1, 0x05,
	0xC7, 1, 0xB3,
	0x2A, 4, 0x00, 0x00, 0x01, 0x3F,
	0x2B, 4, 0x00, 0x00, 0x00, 0xEF,
	0x36, 1, 0xC8,
	0xB0, 1, 0x00,
	0xB1, 2, 0x00, 0x1B,
	0x21, 0,
	0xB6, 2, 0x08, 0x82,
	0xF2, 1, 0x00,
	0x26, 1, 0x01,
	0xE0, 15, 0x04, 0x22, 0x07, 0x0A, 0x2E, 0x30, 0x25, 0x2A, 0x28, 0x26, 0x2E, 0x3A, 0x00, 0x01, 0x03,
	0xE1, 15, 0x04, 0x16, 0x06, 0x0D, 0x2D, 0x26, 0x23, 0x27, 0x27, 0x25, 0x2D, 0x3B, 0x00, 0x01, 0x04,
	0x00,
};

// Variant 2. TODO(panel): unknown, needs a radio: its gamma sets (0xE0 / 0xE1) aren't fully known, so the controller's
// own reset gamma is used
static const uint8_t seq2[] = {
	0xEF, 3, 0x03, 0x80, 0x02,
	0xCF, 3, 0x00, 0xC1, 0x30,
	0xED, 4, 0x67, 0x03, 0x12, 0x81,
	0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
	0xEA, 2, 0x00, 0x00,
	0xE8, 3, 0x85, 0x0A, 0x78,
	0xC0, 1, 0x18,
	0xC1, 1, 0x13,
	0xC5, 2, 0x31, 0x3C,
	0xC7, 1, 0xA8,
	0x3A, 1, 0x55,
	0x30, 2, 0x00, 0x0A,
	0x36, 1, 0xC8,
	0x21, 0,
	0xB0, 1, 0x0A,
	0xB1, 2, 0x00, 0x18,
	0xB6, 3, 0x0A, 0x82, 0x27,
	0xF2, 1, 0x00,
	0x26, 1, 0x01,
	0x3A, 1, 0x05,
	0x00,
};

// Variant 0: variant 2 with MADCTL 0x48 and without 0x30 / 0xB0. TODO(panel): unknown, needs a radio: gamma as
// variant 2
static const uint8_t seq0[] = {
	0xEF, 3, 0x03, 0x80, 0x02,
	0xCF, 3, 0x00, 0xC1, 0x30,
	0xED, 4, 0x67, 0x03, 0x12, 0x81,
	0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
	0xEA, 2, 0x00, 0x00,
	0xE8, 3, 0x85, 0x0A, 0x78,
	0xC0, 1, 0x18,
	0xC1, 1, 0x13,
	0xC5, 2, 0x31, 0x3C,
	0xC7, 1, 0xA8,
	0x3A, 1, 0x55,
	0x36, 1, 0x48,
	0x21, 0,
	0xB1, 2, 0x00, 0x18,
	0xB6, 3, 0x0A, 0x82, 0x27,
	0xF2, 1, 0x00,
	0x26, 1, 0x01,
	0x3A, 1, 0x05,
	0x00,
};

// The register-style panel. TODO(panel): unknown, needs a radio: the orientation (R03 0x1030 is taken to scan the
// long side first, so a landscape row runs along R21) and the R00 writes' value
const uint16_t ili9341_reg_seq[] = {
	0x00, 0x0000, 0x00, 0x0000, 0x00, 0x0000, 0x00, 0x0000,
	0xA4, 0x0001,
	0x60, 0x2700, 0x90, 0x0014, 0x08, 0x0404,
	0x30, 0x0C07, 0x31, 0x7C10, 0x32, 0x0A04, 0x33, 0x0D15, 0x34, 0x3333,
	0x35, 0x150D, 0x36, 0x740A, 0x37, 0x100C, 0x38, 0x070C, 0x39, 0x0000,
	0x10, 0x0730, 0x11, 0x0237, 0x12, 0x0198, 0x0E, 0x0020, 0x13, 0x1800, 0x2A, 0x0074, 0x12, 0x01BC,
	REG_DELAY, 120,
	0x50, 0, 0x51, 239, 0x52, 0, 0x53, 319,
	0x61, 0x0001, 0x80, 0, 0x81, 0, 0x82, 0, 0x92, 0x0600, 0x93, 0x0402, 0x6A, 0, 0x0A, 0x0008,
	0x01, 0x0100, 0x02, 0x0700, 0x03, 0x1030, 0x20, 0, 0x21, 0,
	REG_DISP_CTRL, 0x0100,
	REG_DELAY, 40,
	REG_END,
};

static uint16_t line[ILI9341_MAX_LINE];

const uint8_t *ili9341_init_seq(uint8_t variant)
{
	switch (variant) {
	case ILI9341_VARIANT_0: return seq0;
	case ILI9341_VARIANT_2: return seq2;
	case ILI9341_VARIANT_3: return seq3;
	default: return NULL;
	}
}

static void reg_index(const ili9341_bus_t *b, uint8_t r)
{
	b->cmd(0x00);
	b->cmd(r);
}

static void reg_write(const ili9341_bus_t *b, uint8_t r, uint16_t v)
{
	uint8_t d[2] = { (uint8_t)(v >> 8), (uint8_t)v };
	reg_index(b, r);
	b->data(d, 2);
}

static void window(const ili9341_t *p, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	const ili9341_bus_t *b = p->bus;
	if (p->variant == ILI9341_VARIANT_REG) {
		reg_write(b, REG_H_START, y0);
		reg_write(b, REG_H_END, (uint16_t)(y1 - 1));
		reg_write(b, REG_V_START, x0);
		reg_write(b, REG_V_END, (uint16_t)(x1 - 1));
		reg_write(b, REG_H_ADDR, y0);
		reg_write(b, REG_V_ADDR, x0);
		reg_index(b, REG_GRAM);
		return;
	}
	uint8_t d[4];
	b->cmd(CMD_CASET);
	d[0] = (uint8_t)(x0 >> 8); d[1] = (uint8_t)x0; d[2] = (uint8_t)((x1 - 1) >> 8); d[3] = (uint8_t)(x1 - 1);
	b->data(d, 4);
	b->cmd(CMD_RASET);
	d[0] = (uint8_t)(y0 >> 8); d[1] = (uint8_t)y0; d[2] = (uint8_t)((y1 - 1) >> 8); d[3] = (uint8_t)(y1 - 1);
	b->data(d, 4);
	b->cmd(CMD_RAMWR);
}

void ili9341_fill(const ili9341_t *p, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
	const ili9341_bus_t *b = p->bus;
	uint32_t w = (uint32_t)(x1 - x0);
	if (x1 <= x0 || y1 <= y0 || w > ILI9341_MAX_LINE) {
		return;
	}
	for (uint32_t i = 0; i < w; i++) {
		line[i] = color;
	}
	b->begin();
	window(p, x0, y0, x1, y1);
	for (uint16_t y = y0; y < y1; y++) {
		b->pixels(line, w);
	}
	b->end();
}

static void reset_pulse(const ili9341_bus_t *b)
{
	b->reset(false);
	b->delay_ms(RESET_LOW_MS);
	b->reset(true);
	b->delay_ms(RESET_HIGH_MS);
}

void ili9341_init(const ili9341_t *p)
{
	const ili9341_bus_t *b = p->bus;
	reset_pulse(b);
	reset_pulse(b);
	if (p->variant == ILI9341_VARIANT_REG) {
		for (const uint16_t *s = ili9341_reg_seq; s[0] != REG_END; s += 2) {
			if (s[0] == REG_DELAY) {
				b->delay_ms(s[1]);
				continue;
			}
			b->begin();
			reg_write(b, (uint8_t)s[0], s[1]);
			b->end();
		}
		ili9341_fill(p, 0, 0, p->width, p->height, 0x0000);
		return;
	}
	const uint8_t *seq = ili9341_init_seq(p->variant);
	if (seq == NULL) {
		return;
	}
	b->begin();
	b->cmd(CMD_SLPOUT);
	b->end();
	b->delay_ms(SLPOUT_MS);
	b->begin();
	for (const uint8_t *s = seq; s[0] != 0; s += 2 + s[1]) {
		b->cmd(s[0]);
		b->data(s + 2, s[1]);
	}
	b->end();
	ili9341_fill(p, 0, 0, p->width, p->height, 0x0000);
	b->begin();
	b->cmd(CMD_DISPON);
	b->end();
}

void ili9341_write_rows_indexed(const ili9341_t *p, const uint8_t *fb, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	const ili9341_bus_t *b = p->bus;
	uint32_t w = p->width, stride = (w + 1u) / 2u;
	if (y1 > p->height) {
		y1 = p->height;
	}
	if (y0 >= y1 || w > ILI9341_MAX_LINE) {
		return;
	}
	b->begin();
	window(p, 0, y0, (uint16_t)w, y1);
	for (uint32_t y = y0; y < y1; y++) {
		const uint8_t *row = fb + y * stride;
		for (uint32_t x = 0; x < w; x++) {
			line[x] = palette[(row[x >> 1] >> ((x & 1u) ? 0 : 4)) & 0x0Fu];
		}
		b->pixels(line, w);
	}
	b->end();
}

void ili9341_display_on(const ili9341_t *p, bool on)
{
	const ili9341_bus_t *b = p->bus;
	b->begin();
	if (p->variant == ILI9341_VARIANT_REG) {
		reg_write(b, REG_DISP_CTRL, on ? 0x0100u : 0x0000u);
	} else {
		b->cmd(on ? CMD_DISPON : CMD_DISPOFF);
	}
	b->end();
}
