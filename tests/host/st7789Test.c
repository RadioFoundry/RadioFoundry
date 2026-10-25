// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ST7789 driver: reset and init command stream (MADCTL after 0x35, inversion, clear, display on), bus begin / end
// pairing, and frame buffer rows sent at 2x scale and from a 16-color frame buffer (docs/spec/hardware-dm32.md 7)
#include <stdio.h>
#include <string.h>
#include "display/st7789/st7789.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Command log: each command with the number of data bytes and pixels that followed it
typedef struct {
	uint8_t cmd;
	uint8_t data[16];
	uint32_t n_data;
	uint32_t n_px;
	uint16_t first_px, last_px;
} entry_t;

static entry_t log_e[256];
static int n_e;
static int depth, max_depth, begins;
static uint32_t delay_total;
static int resets[8], n_resets;
static uint16_t px_log[1024];
static uint32_t n_px_log;

static void f_begin(void) { begins++; if (++depth > max_depth) max_depth = depth; }
static void f_end(void) { depth--; }
static void f_cmd(uint8_t c)
{
	if (depth != 1) {
		failures++;
		printf("FAIL command 0x%02X outside begin / end\n", c);
	}
	if (n_e < 256) {
		memset(&log_e[n_e], 0, sizeof log_e[n_e]);
		log_e[n_e].cmd = c;
	}
	n_e++;
}
static void f_data(const uint8_t *d, uint32_t n)
{
	entry_t *e = &log_e[n_e - 1];
	for (uint32_t i = 0; i < n; i++) {
		if (e->n_data < sizeof e->data) {
			e->data[e->n_data] = d[i];
		}
		e->n_data++;
	}
}
static void f_pixels(const uint16_t *px, uint32_t n)
{
	entry_t *e = &log_e[n_e - 1];
	if (e->n_px == 0 && n) {
		e->first_px = px[0];
	}
	if (n) {
		e->last_px = px[n - 1];
	}
	e->n_px += n;
	for (uint32_t i = 0; i < n && n_px_log < 1024; i++) {
		px_log[n_px_log++] = px[i];
	}
}
static void f_reset(bool level) { if (n_resets < 8) resets[n_resets++] = level; }
static void f_delay(uint32_t ms) { delay_total += ms; }

static const st7789_bus_t bus = { f_begin, f_end, f_cmd, f_data, f_pixels, f_reset, f_delay };

static void clear_log(void)
{
	n_e = 0;
	depth = max_depth = begins = 0;
	delay_total = 0;
	n_resets = 0;
	n_px_log = 0;
}

static int find(uint8_t cmd)
{
	for (int i = 0; i < n_e; i++) {
		if (log_e[i].cmd == cmd) {
			return i;
		}
	}
	return -1;
}

static void test_init(void)
{
	const st7789_t p = { &bus, 240, 320, 0xC0, true, 2 };
	clear_log();
	st7789_init(&p);
	CHECK(n_resets == 3 && resets[0] == 1 && resets[1] == 0 && resets[2] == 1);
	CHECK(delay_total >= 120 * 4);		// three reset waits and the sleep-out wait
	CHECK(depth == 0 && max_depth == 1);
	CHECK(log_e[0].cmd == 0x11);		// sleep out first
	int te = find(0x35), mad = find(0x36);
	CHECK(te > 0 && mad == te + 1 && log_e[mad].n_data == 1 && log_e[mad].data[0] == 0xC0);
	int pix = find(0x3A);
	CHECK(pix > 0 && log_e[pix].data[0] == 0x05);
	int g = find(0xE0);
	CHECK(g > 0 && log_e[g].n_data == 14 && log_e[g].data[0] == 0x70 && log_e[g].data[13] == 0x2E);
	int inv = find(0x21);
	CHECK(inv > g && find(0x20) < 0);
	int ca = find(0x2A), ra = find(0x2B), wr = find(0x2C);
	CHECK(ca > inv && log_e[ca].data[2] == 0x00 && log_e[ca].data[3] == 0xEF);	// columns 0..239
	CHECK(ra == ca + 1 && log_e[ra].data[2] == 0x01 && log_e[ra].data[3] == 0x3F);	// rows 0..319
	CHECK(wr == ra + 1 && log_e[wr].n_px == 240u * 320u && log_e[wr].first_px == 0 && log_e[wr].last_px == 0);
	CHECK(log_e[n_e - 1].cmd == 0x29);	// display on last
	// The stream table is well formed: 14 entries (hardware-dm32.md 7 rows 2-16 less MADCTL), then command 0
	uint32_t i = 0, entries = 0;
	while (st7789_init_seq[i] != 0 && i < 256) {
		i += 2u + st7789_init_seq[i + 1];
		entries++;
	}
	CHECK(entries == 14 && i == 73);
}

static void test_rows(void)
{
	// A 120 x 160 frame buffer on the 240 x 320 panel: rows 10..12 become panel rows 20..25, every pixel doubled
	const st7789_t p = { &bus, 240, 320, 0xC0, true, 2 };
	static uint16_t fb[120 * 160];
	for (int y = 0; y < 160; y++) {
		for (int x = 0; x < 120; x++) {
			fb[y * 120 + x] = (uint16_t)(y << 8 | x);
		}
	}
	clear_log();
	st7789_write_rows(&p, fb, 10, 12);
	CHECK(depth == 0 && begins == 1);
	int ca = find(0x2A), ra = find(0x2B), wr = find(0x2C);
	CHECK(ca >= 0 && log_e[ca].data[0] == 0 && log_e[ca].data[1] == 0 && log_e[ca].data[3] == 239);
	CHECK(ra >= 0 && log_e[ra].data[1] == 20 && log_e[ra].data[3] == 23);
	CHECK(wr >= 0 && log_e[wr].n_px == 240u * 4u);
	// Panel row 20 and 21 are both frame buffer row 10; columns 2k and 2k+1 are pixel k
	CHECK(px_log[0] == (10 << 8 | 0) && px_log[1] == (10 << 8 | 0) && px_log[2] == (10 << 8 | 1));
	CHECK(px_log[239] == (10 << 8 | 119) && px_log[240] == (10 << 8 | 0));
	CHECK(px_log[480] == (11 << 8 | 0) && px_log[959] == (11 << 8 | 119));
	// Rows past the frame buffer are clipped; an empty range sends nothing
	clear_log();
	st7789_write_rows(&p, fb, 158, 200);
	ra = find(0x2B);
	CHECK(ra >= 0 && log_e[ra].data[0] == 0x01 && log_e[ra].data[1] == 0x3C && log_e[ra].data[3] == 0x3F);
	clear_log();
	st7789_write_rows(&p, fb, 5, 5);
	CHECK(n_e == 0 && begins == 0);
	// Scale 1
	const st7789_t q = { &bus, 240, 320, 0xC0, true, 1 };
	clear_log();
	st7789_write_rows(&q, fb, 0, 1);
	wr = find(0x2C);
	CHECK(wr >= 0 && log_e[wr].n_px == 240);
}

static void test_rows_indexed(void)
{
	// A 240 x 320 frame, two pixels a byte, high nibble left, colors through the palette
	const st7789_t p = { &bus, 240, 320, 0xC0, true, 1 };
	static uint8_t fb[120 * 320];
	uint16_t pal[16];
	for (int i = 0; i < 16; i++) {
		pal[i] = (uint16_t)(0x1000u * (unsigned)i + 7u);
	}
	for (int y = 0; y < 320; y++) {
		for (int b = 0; b < 120; b++) {
			fb[y * 120 + b] = (uint8_t)((y & 0x0F) << 4 | (b & 0x0F));
		}
	}
	clear_log();
	st7789_write_rows_indexed(&p, fb, pal, 3, 5);
	CHECK(depth == 0 && begins == 1);
	int ra = find(0x2B), wr = find(0x2C);
	CHECK(ra >= 0 && log_e[ra].data[1] == 3 && log_e[ra].data[3] == 4);
	CHECK(wr >= 0 && log_e[wr].n_px == 240u * 2u);
	// Row 3: byte b holds pixel 2b (index 3) and pixel 2b+1 (index b & 15)
	CHECK(px_log[0] == pal[3] && px_log[1] == pal[0] && px_log[2] == pal[3] && px_log[3] == pal[1]);
	CHECK(px_log[239] == pal[119 & 15] && px_log[240] == pal[4]);
	// Clipped like the RGB565 rows
	clear_log();
	st7789_write_rows_indexed(&p, fb, pal, 318, 400);
	wr = find(0x2C);
	CHECK(wr >= 0 && log_e[wr].n_px == 240u * 2u);
	clear_log();
	st7789_write_rows_indexed(&p, fb, pal, 7, 7);
	CHECK(n_e == 0 && begins == 0);
}

static void test_misc(void)
{
	const st7789_t p = { &bus, 240, 320, 0xC0, true, 2 };
	clear_log();
	st7789_invert(&p, false);
	st7789_display_on(&p, false);
	CHECK(n_e == 2 && log_e[0].cmd == 0x20 && log_e[1].cmd == 0x28 && depth == 0);
	clear_log();
	st7789_fill(&p, 10, 20, 30, 25, 0xF800);
	int wr = find(0x2C);
	CHECK(wr >= 0 && log_e[wr].n_px == 20u * 5u && log_e[wr].first_px == 0xF800);
}

int main(void)
{
	test_init();
	test_rows();
	test_rows_indexed();
	test_misc();
	if (failures) {
		printf("st7789Test: %d failures\n", failures);
		return 1;
	}
	printf("st7789Test: all passed\n");
	return 0;
}
