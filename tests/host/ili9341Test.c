// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ILI9341-class driver (drivers/display/ili9341): each variant's init stream is well formed and goes out between bus
// begin / end with the reset pulses first, the window commands of both styles, and a 16-color row through the palette
#include <stdio.h>
#include <string.h>
#include "display/ili9341/ili9341.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint16_t words[200000];
static uint32_t n_words;
static int depth, resets, lows;
static uint32_t px_total;
static uint16_t last_px[320];

static void put(uint16_t w)
{
	if (n_words < sizeof(words) / sizeof(words[0])) {
		words[n_words] = w;
	}
	n_words++;
}
static void f_begin(void) { depth++; }
static void f_end(void) { depth--; }
static void f_cmd(uint8_t c)
{
	if (depth != 1) {
		failures++;
		printf("FAIL command 0x%02X outside begin / end\n", c);
	}
	put((uint16_t)(0x100u | c));
}
static void f_data(const uint8_t *d, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		put(d[i]);
	}
}
static void f_pixels(const uint16_t *px, uint32_t n)
{
	px_total += n;
	if (n <= 320) {
		memcpy(last_px, px, n * sizeof(px[0]));
	}
}
static void f_reset(bool level) { resets++; if (!level) lows++; }
static void f_delay(uint32_t ms) { (void)ms; }

static const ili9341_bus_t bus = {
	.begin = f_begin, .end = f_end, .cmd = f_cmd, .data = f_data, .pixels = f_pixels, .reset = f_reset,
	.delay_ms = f_delay,
};

static void clear(void)
{
	n_words = 0;
	depth = resets = lows = 0;
	px_total = 0;
}

// The position of the command c (0x100 | c) at or after from, or -1
static int find_cmd(uint8_t c, uint32_t from)
{
	for (uint32_t i = from; i < n_words; i++) {
		if (words[i] == (0x100u | c)) {
			return (int)i;
		}
	}
	return -1;
}

static void test_command_variants(void)
{
	static const uint8_t vs[] = { ILI9341_VARIANT_0, ILI9341_VARIANT_2, ILI9341_VARIANT_3 };
	for (unsigned k = 0; k < sizeof(vs); k++) {
		const uint8_t *s = ili9341_init_seq(vs[k]);
		CHECK(s != NULL);
		unsigned n = 0, madctl = 0;
		for (; s[0] != 0; s += 2 + s[1]) {
			CHECK(s[1] <= 15);
			if (s[0] == 0x36) {
				madctl = s[2];
			}
			n++;
		}
		CHECK(n > 10);
		CHECK(madctl == (vs[k] == ILI9341_VARIANT_0 ? 0x48u : 0xC8u));
		ili9341_t p = { .bus = &bus, .width = 320, .height = 240, .variant = vs[k] };
		clear();
		ili9341_init(&p);
		CHECK(depth == 0);
		CHECK(lows == 2 && resets == 4);
		int slp = find_cmd(0x11, 0);
		int on = find_cmd(0x29, 0);
		CHECK(slp == 0);
		CHECK(on > slp);
		CHECK(px_total == 320u * 240u);		// cleared to black
		CHECK(last_px[0] == 0 && last_px[319] == 0);
	}
	CHECK(ili9341_init_seq(ILI9341_VARIANT_REG) == NULL);
	CHECK(ili9341_init_seq(7) == NULL);
}

static void test_register_variant(void)
{
	ili9341_t p = { .bus = &bus, .width = 320, .height = 240, .variant = ILI9341_VARIANT_REG };
	clear();
	ili9341_init(&p);
	CHECK(depth == 0);
	CHECK(px_total == 320u * 240u);
	// Every register write is index 0x00, reg (both as commands) and two data bytes: R60 = 0x2700 is in there
	bool r60 = false;
	for (uint32_t i = 0; i + 3 < n_words; i++) {
		if (words[i] == 0x100 && words[i + 1] == 0x160 && words[i + 2] == 0x27 && words[i + 3] == 0x00) {
			r60 = true;
		}
	}
	CHECK(r60);
	// A row write: window registers, then the frame memory index 0x22
	static uint8_t fb[160 * 240];
	static const uint16_t pal[16] = { 0x0000, 0xF800 };
	memset(fb, 0x01, sizeof(fb));
	clear();
	ili9341_write_rows_indexed(&p, fb, pal, 10, 12);
	CHECK(find_cmd(0x22, 0) > 0);
	CHECK(px_total == 2u * 320u);
	CHECK(last_px[0] == 0x0000 && last_px[1] == 0xF800);
}

static void test_rows(void)
{
	ili9341_t p = { .bus = &bus, .width = 320, .height = 240, .variant = ILI9341_VARIANT_3 };
	static uint8_t fb[160 * 240];
	static const uint16_t pal[16] = { 0x1111, 0x2222, [15] = 0xFFFF };
	memset(fb, 0xF1, sizeof(fb));
	clear();
	ili9341_write_rows_indexed(&p, fb, pal, 230, 300);	// clipped at the bottom
	CHECK(depth == 0);
	CHECK(px_total == 10u * 320u);
	CHECK(last_px[0] == 0xFFFF && last_px[1] == 0x2222);
	int ca = find_cmd(0x2A, 0), ra = find_cmd(0x2B, 0);
	CHECK(ca >= 0 && words[ca + 3] == 0x01 && words[ca + 4] == 0x3F);	// columns to 319
	CHECK(ra >= 0 && words[ra + 2] == 230 && words[ra + 4] == 239);
	CHECK(find_cmd(0x2C, 0) > ra);
	clear();
	ili9341_write_rows_indexed(&p, fb, pal, 240, 241);
	CHECK(n_words == 0 && px_total == 0);
}

int main(void)
{
	test_command_variants();
	test_register_variant();
	test_rows();
	if (failures) {
		printf("ili9341Test: %d failure(s)\n", failures);
		return 1;
	}
	printf("ili9341Test: all passed\n");
	return 0;
}
