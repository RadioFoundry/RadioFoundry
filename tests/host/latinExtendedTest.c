// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <assert.h>
#include <stdio.h>
#include "ui/gfx.h"

static void flush_indexed(const void *pixels, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	(void)pixels;
	(void)palette;
	(void)y0;
	(void)y1;
}

static void check_pair(const font_t *font, char byte, char plain, bool want_difference)
{
	char sample[] = {byte, plain, 0};
	assert(font_fold(byte) == plain);
	gfx_clear(0);
	assert(gfx_text_width(font, sample) == 2 * font->w);
	assert(gfx_text(font, 0, 0, sample, 0xffff) == 2 * font->w);
	bool drawn = false, different = false;
	for (int y = 0; y < font->h; ++y) {
		for (int x = 0; x < font->w; ++x) {
			color_t c = gfx_get_pixel(x, y);
			drawn |= c != 0;
			different |= c != gfx_get_pixel(x + font->w, y);
		}
	}
	if (!drawn || different != want_difference)
		fprintf(stderr, "Byte 0x%02x in %ux%u at %d bpp: drawn=%d different=%d\n",
			(unsigned)(uint8_t)byte, font->w, font->h, gfx_bpp(), drawn, different);
	assert(drawn && different == want_difference);
}

static void check_face(const font_t *font)
{
	for (unsigned i = 0; i < FONT_LATIN_EXTENDED_COUNT; ++i)
		check_pair(font, (char)(FONT_LATIN_EXTENDED_FIRST + i), "GgIiSs"[i], CONFIG_FONT_LATIN_EXTENDED);
}

int main(void)
{
	const hal_display_t mono = {.width = 128, .height = 64, .bpp = 1};
	const hal_display_t color = {.width = 240, .height = 320, .bpp = 4, .flush_indexed = flush_indexed};
	const font_t *mono_faces[] = {&font_5x8, &font_6x12, &font_8x16, &font_12x24, FONT_LARGEST};
	const font_t *color_faces[] = {&font_aa_6x12, &font_aa_8x16, &font_aa_9x18, &font_aa_12x24, FONT_AA_LARGEST};
	assert(gfx_init(&mono));
	for (unsigned i = 0; i < sizeof(mono_faces) / sizeof(mono_faces[0]); ++i)
		check_face(mono_faces[i]);
	const char latin1[] = "\xc7\xe7\xd6\xf6\xdc\xfc";
	for (unsigned i = 0; i < sizeof(latin1) - 1; ++i)
		check_pair(&font_5x8, latin1[i], "CcOoUu"[i], CONFIG_FONT_LATIN_EXTENDED);
	assert(gfx_init(&color));
	for (unsigned i = 0; i < sizeof(color_faces) / sizeof(color_faces[0]); ++i)
		check_face(color_faces[i]);
	puts("Extended Latin rendering checks passed");
	return 0;
}
