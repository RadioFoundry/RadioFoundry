// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui/gfx.h"
#include "ui/i18n.h"

static void flush_indexed(const void *pixels, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	(void)pixels;
	(void)palette;
	(void)y0;
	(void)y1;
}

static void check_text(const font_t *font, const char *label, unsigned cells)
{
	gfx_clear(0);
	assert(gfx_text_width(font, label) == (int)(cells * font->w));
	assert(gfx_text(font, 0, 0, label, 0xffff) == (int)(cells * font->w));
	for (unsigned i = 0; i < cells; i++) {
		bool ink = false;
		for (int y = 0; y < font->h; y++)
			for (unsigned x = i * font->w; x < (i + 1) * font->w; x++)
				ink |= gfx_get_pixel((int)x, y) != 0;
		assert(ink);
	}
}

int main(void)
{
	const hal_display_t mono = { .width = 128, .height = 64, .bpp = 1 };
	const hal_display_t color = { .width = 240, .height = 320, .bpp = 4, .flush_indexed = flush_indexed };
	uint8_t tr_id = lang_find('t' << 8 | 'r');
	uint8_t ru_id = lang_find('r' << 8 | 'u');
	assert(tr_id != LANG_ENGLISH && ru_id != LANG_ENGLISH && tr_id != ru_id);
	assert(lang_built(tr_id) && lang_built(ru_id));
	assert(lang_find(lang_value(tr_id)) == tr_id && lang_find(lang_value(ru_id)) == ru_id);
	assert(lang_set(tr_id));
	const char *turkish = TR(TITLE_SETTINGS, "Settings");
	assert(strcmp(turkish, "Ayarlar") == 0);
	assert(lang_set(ru_id));
	const char *russian = TR(TITLE_SETTINGS, "Settings");
	assert(strcmp(russian, "Settings") != 0 && (uint8_t)russian[0] == CYRILLIC_PREFIX);
	assert(lang_set(LANG_ENGLISH) && strcmp(TR(TITLE_SETTINGS, "Settings"), "Settings") == 0);

	assert(gfx_init(&mono));
	check_text(&font_5x8, turkish, 7);
	check_text(&font_5x8, lang_name(tr_id), 6);
	check_text(&font_5x8, russian, 9);
	assert(gfx_init(&color));
	check_text(&font_aa_9x18, lang_name(ru_id), 7);
	check_text(&font_aa_9x18, russian, 9);

	char fitted[6];
	gfx_text_fit(&font_aa_9x18, russian, 3 * font_aa_9x18.w, fitted, sizeof(fitted));
	assert(strlen(fitted) == 5 && fitted[4] == GLYPH_ELLIPSIS[0]);
	gfx_text_fit(&font_aa_9x18, russian, 200, fitted, sizeof(fitted));
	assert(strlen(fitted) == 4);
	puts("Turkish and Russian menu switching and rendering passed");
	return 0;
}
