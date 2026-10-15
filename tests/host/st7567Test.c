// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// ST7567 driver: the init stream, bus begin / end pairing, and the page transposition of a 1 bpp frame buffer
// (docs/spec/hardware-dm1801pro.md 3)
#include <stdio.h>
#include <string.h>
#include "display/st7567/st7567.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint8_t cmds[512];
static int n_cmds;
static uint8_t pages[ST7567_PAGES][ST7567_WIDTH];
static int page_sent[ST7567_PAGES];
static int cur_page = -1, data_calls, depth, resets[4], n_resets;

static void f_begin(void) { depth++; }
static void f_end(void) { depth--; }
static void f_cmd(uint8_t c)
{
	CHECK(depth == 1);
	if (n_cmds < 512) {
		cmds[n_cmds++] = c;
	}
	if ((c & 0xF0u) == 0xB0u) {
		cur_page = c & 0x0F;
	}
}
static void f_data(const uint8_t *d, uint32_t n)
{
	CHECK(depth == 1 && n == ST7567_WIDTH && cur_page >= 0 && cur_page < (int)ST7567_PAGES);
	memcpy(pages[cur_page], d, n);
	page_sent[cur_page]++;
	data_calls++;
}
static void f_reset(bool l) { if (n_resets < 4) resets[n_resets++] = l; }
static void f_delay(uint32_t ms) { (void)ms; }

static const st7567_bus_t bus = { f_begin, f_end, f_cmd, f_data, f_reset, f_delay };
static const st7567_t panel = { .bus = &bus, .col_offset = 4, .seg_reverse = false, .com_reverse = true,
	.regulator = 4, .contrast = 0x20, .bias = 0 };

static void clear_log(void)
{
	n_cmds = 0;
	data_calls = 0;
	memset(page_sent, 0, sizeof page_sent);
	cur_page = -1;
}

static void test_transpose(void)
{
	uint8_t fb[ST7567_HEIGHT * ST7567_WIDTH / 8];
	uint8_t out[ST7567_WIDTH];
	memset(fb, 0, sizeof fb);
	// pixel (0,0): top row, leftmost: page 0 column 0 bit 0
	fb[0] = 0x80;
	// pixel (127, 7): bottom row of page 0, rightmost column: bit 7
	fb[7 * 16 + 15] = 0x01;
	// pixel (9, 8): page 1, column 9, bit 0
	fb[8 * 16 + 1] = 0x40;
	st7567_page_bytes(fb, 0, out);
	CHECK(out[0] == 0x01 && out[127] == 0x80 && out[9] == 0);
	st7567_page_bytes(fb, 1, out);
	CHECK(out[9] == 0x01 && out[0] == 0);
}

static void test_init(void)
{
	clear_log();
	st7567_init(&panel);
	CHECK(depth == 0 && n_resets == 2 && resets[0] == 0 && resets[1] == 1);
	CHECK(cmds[0] == 0xE2);
	CHECK(cmds[1] == 0xA2 && cmds[2] == 0xA0 && cmds[3] == 0xC8);	// bias 1/9, SEG normal, COM reversed
	CHECK(cmds[4] == 0x40 && cmds[5] == 0x24 && cmds[6] == 0x81 && cmds[7] == 0x20 && cmds[8] == 0x2F);
	CHECK(cmds[n_cmds - 1] == 0xAF);
	CHECK(data_calls == (int)ST7567_PAGES);
	// every page starts at column 4
	int col4 = 0;
	for (int i = 0; i + 2 < n_cmds; i++) {
		if ((cmds[i] & 0xF0) == 0xB0 && cmds[i + 1] == 0x10 && cmds[i + 2] == 0x04) {
			col4++;
		}
	}
	CHECK(col4 == (int)ST7567_PAGES);
}

static void test_rows(void)
{
	uint8_t fb[ST7567_HEIGHT * ST7567_WIDTH / 8];
	memset(fb, 0xFF, sizeof fb);
	clear_log();
	st7567_write_rows(&panel, fb, 9, 17);	// rows 9..16 touch pages 1 and 2
	CHECK(depth == 0 && data_calls == 2 && page_sent[1] == 1 && page_sent[2] == 1 && page_sent[0] == 0);
	CHECK(pages[1][0] == 0xFF && pages[2][127] == 0xFF);
	clear_log();
	st7567_write_rows(&panel, fb, 0, 64);
	CHECK(data_calls == (int)ST7567_PAGES);
	clear_log();
	st7567_write_rows(&panel, fb, 5, 5);
	st7567_write_rows(&panel, fb, 60, 200);	// clamped to the panel: page 7 only
	CHECK(data_calls == 1 && page_sent[7] == 1);
	clear_log();
	st7567_set_contrast(&panel, 0x7F);
	CHECK(n_cmds == 2 && cmds[0] == 0x81 && cmds[1] == 0x3F);
}

int main(void)
{
	test_transpose();
	test_init();
	test_rows();
	if (failures) {
		printf("st7567Test: %d failures\n", failures);
		return 1;
	}
	printf("st7567Test: all passed\n");
	return 0;
}
