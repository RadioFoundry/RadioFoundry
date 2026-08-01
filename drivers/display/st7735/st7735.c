// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// ST7735 / HX8353-class TFT driver and the 0xFE/0xEF unlock variant (docs/spec/hardware-dm1701.md 9.2, 9.3)
#include "st7735.h"

enum {
	CMD_SLPOUT = 0x11,
	CMD_INVOFF = 0x20,
	CMD_INVON = 0x21,
	CMD_DISPOFF = 0x28,
	CMD_DISPON = 0x29,
	CMD_CASET = 0x2A,
	CMD_RASET = 0x2B,
	CMD_RAMWR = 0x2C,
	CMD_MADCTL = 0x36,
	CMD_COLMOD = 0x3A,
};

// Init tables: entries of { command, parameter count, parameters... }. Command 0x00 (NOP on these controllers,
// never needed here) marks a pause instead, its count byte being the pause in milliseconds.
#define WAIT 0x00

static const uint8_t init_st7735[] = {
	CMD_SLPOUT, 0,
	WAIT, 120,
	0xB1, 3, 0x05, 0x3C, 0x3C,			// frame rate, normal mode
	0xB2, 3, 0x05, 0x3C, 0x3C,			// frame rate, idle
	0xB3, 6, 0x05, 0x3C, 0x3C, 0x05, 0x3C, 0x3C,	// frame rate, partial
	0xB4, 1, 0x03,					// inversion control
	0xC0, 3, 0x28, 0x08, 0x04,			// power control 1..5
	0xC1, 1, 0xC0,
	0xC2, 2, 0x0D, 0x00,
	0xC3, 2, 0x8D, 0x2A,
	0xC4, 2, 0x8D, 0xEE,
	0xC5, 1, 0x1A,					// VCOM
	0xE0, 16, 0x04, 0x0C, 0x07, 0x0A, 0x2E, 0x30, 0x25, 0x2A, 0x28, 0x26, 0x2E, 0x3A, 0x00, 0x01, 0x03, 0x13,
	0xE1, 16, 0x04, 0x16, 0x06, 0x0D, 0x2D, 0x26, 0x23, 0x27, 0x27, 0x25, 0x2D, 0x3B, 0x00, 0x01, 0x04, 0x13,
};

// Unlock variant, before and after the one register that differs between types 2 and 3 (0xFD)
static const uint8_t init_unlock_a[] = {
	0xFE, 0,
	0xEF, 0,
	0xB4, 1, 0x00,
	0xFF, 1, 0x16,
};

static const uint8_t init_unlock_b[] = {
	0xA4, 1, 0x70,
	0xE7, 2, 0x94, 0x88,
	0xEA, 1, 0x3A,
	0xED, 1, 0x11,
	0xE4, 1, 0xC5,
	0xE2, 1, 0x80,
	0xA3, 1, 0x12,
	0xE3, 1, 0x07,
	0xE5, 1, 0x10,
	0xF0, 1, 0x00,
	0xF1, 1, 0x55,
	0xF2, 1, 0x05,
	0xF3, 1, 0x53,
	0xF4, 1, 0x00,
	0xF5, 1, 0x00,
	0xF7, 1, 0x27,
	0xF8, 1, 0x22,
	0xF9, 1, 0x77,
	0xFA, 1, 0x35,
	0xFB, 1, 0x00,
	0xFC, 1, 0x00,
	0xFE, 0,
	0xEF, 0,
	0xE9, 1, 0x00,
	WAIT, 20,
};

static const uint8_t madctl_by_type[4] = { 0xA0, 0x60, 0xE0, 0xA0 };

static bool is_bgr(const st7735_t *p)
{
	return (p->type & 3) == 3;
}

static void cmd1(const st7735_bus_t *b, uint8_t c, uint8_t v)
{
	b->cmd(c);
	b->data(&v, 1);
}

// Runs a table inside a session; pauses end the session so the bus is free while the panel settles
static void run_table(const st7735_bus_t *b, const uint8_t *t, uint32_t len)
{
	uint32_t i = 0;
	while (i + 1 < len) {
		uint8_t c = t[i];
		uint8_t n = t[i + 1];
		i += 2;
		if (c == WAIT) {
			b->end();
			b->delay_ms(n);
			b->begin();
			continue;
		}
		b->cmd(c);
		if (n > 0) {
			b->data(&t[i], n);
		}
		i += n;
	}
}

static void put_range(const st7735_bus_t *b, uint8_t c, uint16_t first, uint16_t last)
{
	uint8_t d[4] = { (uint8_t)(first >> 8), (uint8_t)first, (uint8_t)(last >> 8), (uint8_t)last };
	b->cmd(c);
	b->data(d, sizeof(d));
}

// Window of panel addresses, x1 and y1 exclusive, followed by RAMWR
static void set_window(const st7735_bus_t *b, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	put_range(b, CMD_CASET, x0, x1 - 1);
	put_range(b, CMD_RASET, y0, y1 - 1);
	b->cmd(CMD_RAMWR);
}

void st7735_init(const st7735_t *p)
{
	const st7735_bus_t *b = p->bus;
	uint8_t type = p->type & 3;

	b->reset(true);
	b->delay_ms(1);
	b->reset(false);
	b->delay_ms(20);
	b->reset(true);
	b->delay_ms(20);

	b->begin();
	if (type <= 1) {
		run_table(b, init_st7735, sizeof(init_st7735));
	} else {
		run_table(b, init_unlock_a, sizeof(init_unlock_a));
		cmd1(b, 0xFD, type == 2 ? 0x4F : 0x40);
		run_table(b, init_unlock_b, sizeof(init_unlock_b));
	}
	cmd1(b, CMD_MADCTL, madctl_by_type[type] ^ p->madctl_xor);
	cmd1(b, CMD_COLMOD, 0x05);	// 16 bits per pixel
	b->cmd(CMD_SLPOUT);
	b->end();
	b->delay_ms(120);

	// Clear the panel RAM, hidden rows included, before the display turns on
	st7735_fill_raw(p, 0, 0, p->col_offset + p->width, p->row_offset + p->height, 0x0000);
	st7735_display_on(p, true);
}

void st7735_write_rows(const st7735_t *p, const uint16_t *fb, uint16_t y0, uint16_t y1)
{
	const st7735_bus_t *b = p->bus;

	if (y1 > p->height) {
		y1 = p->height;
	}
	if (y0 >= y1) {
		return;
	}
	b->begin();
	set_window(b, p->col_offset, p->row_offset + y0, p->col_offset + p->width, p->row_offset + y1);
	b->pixels(fb + (uint32_t)y0 * p->width, (uint32_t)(y1 - y0) * p->width, is_bgr(p));
	b->end();
}

void st7735_fill_raw(const st7735_t *p, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
	const st7735_bus_t *b = p->bus;
	uint16_t chunk[32];
	uint32_t left;

	if (x0 >= x1 || y0 >= y1) {
		return;
	}
	for (uint32_t i = 0; i < sizeof(chunk) / sizeof(chunk[0]); i++) {
		chunk[i] = color;
	}
	left = (uint32_t)(x1 - x0) * (y1 - y0);
	b->begin();
	set_window(b, x0, y0, x1, y1);
	while (left > 0) {
		uint32_t n = left < 32 ? left : 32;
		b->pixels(chunk, n, is_bgr(p));
		left -= n;
	}
	b->end();
}

void st7735_invert(const st7735_t *p, bool on)
{
	p->bus->begin();
	p->bus->cmd(on ? CMD_INVON : CMD_INVOFF);
	p->bus->end();
}

void st7735_display_on(const st7735_t *p, bool on)
{
	p->bus->begin();
	p->bus->cmd(on ? CMD_DISPON : CMD_DISPOFF);
	p->bus->end();
}
