// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// ST7735 driver: rows from a 16-color frame buffer (st7735_write_rows_indexed) against the same rows from RGB565
#include <stdio.h>
#include <string.h>
#include "display/st7735/st7735.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int depth, begins;
static uint8_t last_cmd;
static uint8_t caset[4], raset[4];
static uint16_t px[4096];
static uint32_t n_px;
static int swaps, calls;

static void f_begin(void) { begins++; depth++; }
static void f_end(void) { depth--; }
static void f_cmd(uint8_t c)
{
	CHECK(depth == 1);
	last_cmd = c;
}
static void f_data(const uint8_t *d, uint32_t n)
{
	if (last_cmd == 0x2A && n == 4) {
		memcpy(caset, d, 4);
	} else if (last_cmd == 0x2B && n == 4) {
		memcpy(raset, d, 4);
	}
}
static void f_pixels(const uint16_t *p, uint32_t n, bool swap_rb)
{
	CHECK(last_cmd == 0x2C);
	CHECK(n <= 32);
	calls++;
	swaps += swap_rb;
	for (uint32_t i = 0; i < n && n_px < 4096; i++) {
		px[n_px++] = p[i];
	}
}
static void f_reset(bool level) { (void)level; }
static void f_delay(uint32_t ms) { (void)ms; }

static const st7735_bus_t bus = { f_begin, f_end, f_cmd, f_data, f_pixels, f_reset, f_delay };

static const uint16_t pal[16] = { 0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
				  0x8410, 0x4208, 0xC618, 0x1234, 0x5678, 0x9ABC, 0xDEF0, 0x0F0F };

static void reset(void)
{
	depth = begins = swaps = calls = 0;
	n_px = 0;
	memset(caset, 0, sizeof caset);
	memset(raset, 0, sizeof raset);
}

// Index of pixel (x, y) in a test pattern that touches every palette entry and both nibbles
static uint8_t idx(uint32_t x, uint32_t y) { return (uint8_t)((x * 7u + y * 3u) & 0x0Fu); }

static void run(uint16_t w, uint16_t h, uint16_t y0, uint16_t y1, uint8_t type, uint16_t col_off, uint16_t row_off)
{
	uint8_t fb[80 * 8];
	uint32_t stride = ((uint32_t)w + 1u) / 2u;
	st7735_t p = { .bus = &bus, .type = type, .width = w, .height = h, .col_offset = col_off, .row_offset = row_off };

	memset(fb, 0, sizeof fb);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint8_t *b = &fb[y * stride + (x >> 1)];
			*b = (x & 1u) ? (uint8_t)((*b & 0xF0u) | idx(x, y)) : (uint8_t)((*b & 0x0Fu) | (idx(x, y) << 4));
		}
	}
	reset();
	st7735_write_rows_indexed(&p, fb, pal, y0, y1);
	uint16_t e1 = y1 > h ? h : y1;
	if (y0 >= e1) {
		CHECK(begins == 0 && n_px == 0);
		return;
	}
	CHECK(depth == 0 && begins == 1);
	CHECK(n_px == (uint32_t)w * (e1 - y0));
	CHECK(caset[1] == col_off && caset[3] == col_off + w - 1);
	CHECK(raset[1] == row_off + y0 && raset[3] == row_off + e1 - 1);
	for (uint32_t y = y0; y < e1; y++) {
		for (uint32_t x = 0; x < w; x++) {
			if (px[(y - y0) * w + x] != pal[idx(x, y)]) {
				CHECK(!"pixel");
				return;
			}
		}
	}
	CHECK(swaps == ((type & 3) == 3 ? calls : 0));
}

int main(void)
{
	run(70, 8, 0, 8, 0, 0, 0);	// a row crosses the 32 pixel chunks
	run(33, 8, 2, 5, 0, 0, 0);	// odd width: the last byte holds one pixel
	run(70, 8, 1, 4, 0, 2, 3);	// the panel offsets are in the window
	run(70, 8, 6, 20, 0, 0, 0);	// y1 past the height is clipped
	run(70, 8, 8, 12, 0, 0, 0);	// nothing to send
	run(70, 8, 0, 3, 3, 0, 0);	// BGR panel: the bus swaps red and blue
	if (failures == 0) {
		printf("st7735Test PASSED\n");
	}
	return failures != 0;
}
