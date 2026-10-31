// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The translation lookup (core/ui/i18n.c) on the tables tools/i18n.py builds from core/ui/lang/*.po, the language
// list and its stored values, and the fonts' Latin-1 glyphs with their plain-letter fallback (fonts.c). Built twice:
// with Spanish, and without (CONFIG_LANG_ES=0: listed, not built, every lookup the English)
#include <stdio.h>
#include <string.h>
#include "ui/i18n.h"
#include "ui/font.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static bool same_glyph(const font_t *f, uint8_t a, uint8_t b)
{
	int ta, ra, tb, rb;
	const uint8_t *ga = font_glyph(f, a, &ta, &ra), *gb = font_glyph(f, b, &tb, &rb);
	return ga != NULL && gb != NULL && ta == tb && ra == rb && memcmp(ga, gb, (size_t)ra * ((f->w * f->bpp + 7u) / 8u)) == 0;
}

int main(void)
{
	// The list: English first and always built, then each catalog by code
	CHECK(lang_count() >= 2);
	CHECK(strcmp(lang_code(LANG_ENGLISH), "en") == 0 && lang_built(LANG_ENGLISH));
	uint8_t es = lang_find('e' << 8 | 's');
	CHECK(es != LANG_ENGLISH && strcmp(lang_code(es), "es") == 0);
	CHECK(strcmp(lang_name(es), "Espa\xF1" "ol") == 0);
	CHECK(lang_value(es) == ('e' << 8 | 's') && lang_value(LANG_ENGLISH) == ('e' << 8 | 'n'));
	CHECK(lang_find('e' << 8 | 'n') == LANG_ENGLISH && lang_find('x' << 8 | 'x') == LANG_ENGLISH);
	CHECK(strcmp(lang_code(200), "") == 0 && !lang_built(200) && !lang_set(200) && lang_value(200) == 0);

	// An empty setting (never picked) is the build's default, and so is the language before the settings are applied;
	// English picked is stored as "en", so it stays English whatever the default
#if CONFIG_LANG_DEFAULT_ES
	CHECK(lang_find(0) == es && lang_current() == es);
	CHECK(lang_set(LANG_ENGLISH));
#else
	CHECK(lang_find(0) == LANG_ENGLISH);
#endif

	// English: every lookup is the text itself
	CHECK(lang_current() == LANG_ENGLISH);
	const char *en = "Settings";
	CHECK(tr(en) == en);
	CHECK(tr(NULL) == NULL);
#if CONFIG_LANG_ES
	CHECK(lang_built(es) && lang_set(es) && lang_current() == es);
	CHECK(strcmp(TR("Settings"), "Ajustes") == 0);
	CHECK(strcmp(TR("Language"), "Idioma/Language") == 0);
	CHECK(strcmp(TR("On my way"), "En camino") == 0);	// a canned text message: sent in the language in use
	const char *unknown = "Not a string the UI marks";
	CHECK(tr(unknown) == unknown);		// shown as it is
	CHECK(tr("") != NULL && tr("")[0] == 0);
	CHECK(lang_set(LANG_ENGLISH) && strcmp(TR("Settings"), "Settings") == 0);
#else
	CHECK(!lang_built(es) && !lang_set(es) && lang_current() == LANG_ENGLISH);
	CHECK(strcmp(TR("Settings"), "Settings") == 0);
#endif

	// Latin-1: the 8x16 font has the letters when a language is built, else they fall back to the plain letter; the
	// 5x8 font never has them
	CHECK(same_glyph(&font_5x8, 0xF1, 'n') && same_glyph(&font_5x8, 0xC9, 'E') && same_glyph(&font_5x8, 0xBF, '?'));
#if CONFIG_LANG_ANY
	CHECK(!same_glyph(&font_8x16, 0xF1, 'n') && font_8x16.latin1);
	CHECK(!same_glyph(&font_aa_8x16, 0xE1, 'a'));
#else
	CHECK(same_glyph(&font_8x16, 0xF1, 'n') && !font_8x16.latin1);
#endif
	// The glyph bytes below the Latin-1 range keep their meaning
	int t, r;
	CHECK(font_glyph(&font_8x16, 0x82, &t, &r) != NULL);
	CHECK(font_glyph(&font_8x16, 0x90, &t, &r) == NULL);

	printf("%s i18nTest\n", fails ? "# FAILED" : "# PASSED");
	return fails != 0;
}
