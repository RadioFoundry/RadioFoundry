// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ST7789-class TFT driver (docs/spec/hardware-dm32.md 7)
#include <stddef.h>
#include "st7789.h"

#define CMD_SLPOUT	0x11
#define CMD_INVOFF	0x20
#define CMD_INVON	0x21
#define CMD_DISPOFF	0x28
#define CMD_DISPON	0x29
#define CMD_CASET	0x2A
#define CMD_RASET	0x2B
#define CMD_RAMWR	0x2C
#define CMD_MADCTL	0x36
#define RESET_MS	120
#define SLPOUT_MS	120

// After sleep out and before the window: { cmd, count, data... }, 0 ends. MADCTL (0x36) is sent from the panel
// description instead of from here
const uint8_t st7789_init_seq[] = {
	0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33,	// porch control
	0x35, 1, 0x00,				// tearing effect line on
	0x3A, 1, 0x05,				// 16 bits per pixel
	0xB7, 1, 0x56,				// gate control
	0xBB, 1, 0x17,				// VCOM
	0xC0, 1, 0x2C,				// LCM control
	0xC2, 1, 0x01,				// VDV / VRH from commands
	0xC3, 1, 0x0B,				// VRH
	0xC6, 1, 0xEF,				// frame rate, normal mode
	0xD0, 1, 0xA7,				// power control 1, short form first, as the working firmware
	0xD0, 2, 0xA4, 0xA1,			// power control 1
	0xD6, 1, 0xA1,
	0xE0, 14, 0x70, 0x03, 0x08, 0x07, 0x08, 0x15, 0x30, 0x54, 0x47, 0x3A, 0x15, 0x14, 0x2B, 0x2E,
	0xE1, 14, 0x70, 0x06, 0x0A, 0x09, 0x06, 0x03, 0x2F, 0x44, 0x47, 0x38, 0x12, 0x15, 0x29, 0x2F,
	0x00,
};

static uint16_t line[ST7789_MAX_LINE];

static void window(const st7789_t *p, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	const st7789_bus_t *b = p->bus;
	uint8_t d[4];
	b->cmd(CMD_CASET);
	d[0] = (uint8_t)(x0 >> 8); d[1] = (uint8_t)x0; d[2] = (uint8_t)((x1 - 1) >> 8); d[3] = (uint8_t)(x1 - 1);
	b->data(d, 4);
	b->cmd(CMD_RASET);
	d[0] = (uint8_t)(y0 >> 8); d[1] = (uint8_t)y0; d[2] = (uint8_t)((y1 - 1) >> 8); d[3] = (uint8_t)(y1 - 1);
	b->data(d, 4);
	b->cmd(CMD_RAMWR);
}

void st7789_fill(const st7789_t *p, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
	const st7789_bus_t *b = p->bus;
	uint32_t w = (uint32_t)(x1 - x0);
	if (x1 <= x0 || y1 <= y0 || w > ST7789_MAX_LINE) {
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

void st7789_init(const st7789_t *p)
{
	const st7789_bus_t *b = p->bus;
	b->reset(true);
	b->delay_ms(RESET_MS);
	b->reset(false);
	b->delay_ms(RESET_MS);
	b->reset(true);
	b->delay_ms(RESET_MS);

	b->begin();
	b->cmd(CMD_SLPOUT);
	b->end();
	b->delay_ms(SLPOUT_MS);

	b->begin();
	for (const uint8_t *s = st7789_init_seq; *s != 0; s += 2 + s[1]) {
		b->cmd(s[0]);
		b->data(s + 2, s[1]);
		if (s[0] == 0x35) {
			b->cmd(CMD_MADCTL);	// in the working firmware's place: after the tearing effect command
			b->data(&p->madctl, 1);
		}
	}
	b->cmd(p->invert ? CMD_INVON : CMD_INVOFF);
	b->end();

	st7789_fill(p, 0, 0, p->width, p->height, 0x0000);

	b->begin();
	b->cmd(CMD_DISPON);
	b->end();
}

void st7789_write_rows(const st7789_t *p, const uint16_t *fb, uint16_t y0, uint16_t y1)
{
	const st7789_bus_t *b = p->bus;
	uint8_t s = p->scale != 0 ? p->scale : 1;
	uint32_t fbw = p->width / s, fbh = p->height / s;
	if (y1 > fbh) {
		y1 = (uint16_t)fbh;
	}
	if (y0 >= y1 || p->width > ST7789_MAX_LINE) {
		return;
	}
	b->begin();
	window(p, 0, (uint16_t)(y0 * s), (uint16_t)(fbw * s), (uint16_t)(y1 * s));
	for (uint32_t y = y0; y < y1; y++) {
		const uint16_t *row = fb + y * fbw;
		if (s == 1) {
			b->pixels(row, fbw);
			continue;
		}
		for (uint32_t x = 0; x < fbw; x++) {
			for (uint32_t k = 0; k < s; k++) {
				line[x * s + k] = row[x];
			}
		}
		for (uint32_t k = 0; k < s; k++) {
			b->pixels(line, fbw * s);
		}
	}
	b->end();
}

void st7789_write_rows_indexed(const st7789_t *p, const uint8_t *fb, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	const st7789_bus_t *b = p->bus;
	uint8_t s = p->scale != 0 ? p->scale : 1;
	uint32_t fbw = p->width / s, fbh = p->height / s, stride = (fbw + 1u) / 2u;
	if (y1 > fbh) {
		y1 = (uint16_t)fbh;
	}
	if (y0 >= y1 || p->width > ST7789_MAX_LINE) {
		return;
	}
	b->begin();
	window(p, 0, (uint16_t)(y0 * s), (uint16_t)(fbw * s), (uint16_t)(y1 * s));
	for (uint32_t y = y0; y < y1; y++) {
		const uint8_t *row = fb + y * stride;
		for (uint32_t x = 0; x < fbw; x++) {
			uint16_t c = palette[(row[x >> 1] >> ((x & 1u) ? 0 : 4)) & 0x0Fu];
			for (uint32_t k = 0; k < s; k++) {
				line[x * s + k] = c;
			}
		}
		for (uint32_t k = 0; k < s; k++) {
			b->pixels(line, fbw * s);
		}
	}
	b->end();
}

void st7789_invert(const st7789_t *p, bool on)
{
	p->bus->begin();
	p->bus->cmd(on ? CMD_INVON : CMD_INVOFF);
	p->bus->end();
}

void st7789_display_on(const st7789_t *p, bool on)
{
	p->bus->begin();
	p->bus->cmd(on ? CMD_DISPON : CMD_DISPOFF);
	p->bus->end();
}
