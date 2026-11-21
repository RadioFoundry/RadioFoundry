// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000 LCD controller's byte loops (platform/hrc7000/lcdc.c): every byte in order, to the right register, a
// 16-color row through the palette exactly as the RGB565 path sends it, odd widths included
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define PLATFORM_HRC7000_H	// lcdc.c's own declarations only: no SoC header on the host
#define LCDC_BASE		0u
static uint32_t fake_cfg, fake_en = 2u;	// ready at once
#define REG32(a)		(*((a) == 0x10u ? &fake_cfg : &fake_en))

static struct { uint8_t reg, v; } out[4096];
static uint32_t n_out;
#define LCDC_WRITE(r_, v_)	do { if (n_out < 4096u) { out[n_out].reg = (uint8_t)(r_); out[n_out].v = (uint8_t)(v_); } \
	n_out++; } while (0)

void hrc_lcdc_start(void);
void hrc_lcdc_drain(void);
void hrc_lcdc_cmd(uint8_t c);
void hrc_lcdc_data(const uint8_t *d, uint32_t n);
void hrc_lcdc_pixels(const uint16_t *px, uint32_t n);
void hrc_lcdc_pixels4(const uint8_t *row, const uint16_t *palette, uint32_t n);

#include "../../platform/hrc7000/lcdc.c"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_cmd_data(void)
{
	static const uint8_t d[3] = { 0x12, 0x34, 0xFF };
	n_out = 0;
	hrc_lcdc_cmd(0x2C);
	hrc_lcdc_data(d, 3);
	CHECK(n_out == 4);
	CHECK(out[0].reg == 0x00 && out[0].v == 0x2C);
	CHECK(out[1].reg == 0x04 && out[1].v == 0x12 && out[3].v == 0xFF);
	hrc_lcdc_start();
	CHECK(fake_cfg == LCDC_CFG_VALUE && (fake_en & 1u));
}

static void test_pixels4(void)
{
	uint16_t pal[16];
	for (int i = 0; i < 16; i++)
		pal[i] = (uint16_t)(0x1111u * (unsigned)i ^ 0x00F0u);	// high and low bytes differ
	uint8_t row[160];
	for (int i = 0; i < 160; i++)
		row[i] = (uint8_t)(i * 37 + 11);
	for (uint32_t w = 0; w <= 320; w += (w < 8 ? 1 : 79)) {	// 0..8, 87, 166, 245 (odd and even)
		uint16_t px[320];
		for (uint32_t x = 0; x < w; x++)
			px[x] = pal[(row[x >> 1] >> ((x & 1u) ? 0 : 4)) & 0x0Fu];
		n_out = 0;
		hrc_lcdc_pixels(px, w);
		static struct { uint8_t reg, v; } want[4096];
		uint32_t n_want = n_out;
		memcpy(want, out, sizeof want);
		n_out = 0;
		hrc_lcdc_pixels4(row, pal, w);
		CHECK(n_want == 2u * w);
		CHECK(n_out == n_want && memcmp(out, want, n_want * sizeof out[0]) == 0);
		if (n_out != n_want || memcmp(out, want, n_want * sizeof out[0]) != 0)
			printf("  width %u\n", (unsigned)w);
	}
	// The first pixel is the high nibble, high byte first
	uint8_t one = 0xA5;
	n_out = 0;
	hrc_lcdc_pixels4(&one, pal, 2);
	CHECK(out[0].v == pal[10] >> 8 && out[1].v == (pal[10] & 0xFFu) && out[2].v == pal[5] >> 8);
}

int main(void)
{
	test_cmd_data();
	test_pixels4();
	if (failures) {
		printf("hrc7000LcdcTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("hrc7000LcdcTest: all passed\n");
	return 0;
}
