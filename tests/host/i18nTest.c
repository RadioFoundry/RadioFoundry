// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The translation lookup (core/ui/i18n.c) on the tables tools/i18n.py builds from the marks in core/ and
// core/ui/lang/*.po (a key to its translation, else its English; tr_en), the language list and its stored values, and
// the fonts' Latin-1 glyphs with their plain-letter fallback (fonts.c). Built three times: with Spanish, without
// (CONFIG_LANG_ES=0: listed, not built, a mark is its English), and with Spanish the default
#include <stdio.h>
#include <string.h>
#include "ui/i18n.h"
#include "ui/fmt.h"
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
	// A CJK catalog is listed even in a build without its face, under the Latin name that build's font can draw
	uint8_t zh = lang_find('z' << 8 | 'h');
	CHECK(zh != LANG_ENGLISH && strcmp(lang_code(zh), "zh") == 0 && lang_wide(zh));
	CHECK(!lang_wide(es));
#if CONFIG_LANG_ZH
	CHECK(lang_built(zh));
#else
	CHECK(!lang_built(zh));
	CHECK(strcmp(lang_name(zh), "Chinese") == 0);
	for (const char *p = lang_name(zh); *p; p++)
		CHECK((unsigned char)*p < 0x80);	// a Latin font would draw UTF-8 bytes as boxes
#endif
	CHECK(strcmp(lang_code(200), "") == 0 && !lang_built(200) && !lang_set(200) && lang_value(200) == 0);

	// An empty setting (never picked) is the build's default, and so is the language before the settings are applied;
	// English picked is stored as "en", so it stays English whatever the default
#if CONFIG_LANG_DEFAULT_ES
	CHECK(lang_find(0) == es && lang_current() == es);
	CHECK(lang_set(LANG_ENGLISH));
#else
	CHECK(lang_find(0) == LANG_ENGLISH);
#endif

	// English: every key shows its English. A tr_id_t is opaque: the marks, tr(), tr_en() and TR_NONE are all a
	// caller uses, in either build (the keys are tools/i18n.py's, from core/)
	CHECK(lang_current() == LANG_ENGLISH);
	const tr_id_t settings = TRN(TITLE_SETTINGS, "Settings");
	CHECK(strcmp(tr(settings), "Settings") == 0 && strcmp(tr_en(settings), "Settings") == 0);
	CHECK(settings != TR_NONE);
	CHECK(tr(TR_NONE) == NULL && tr_en(TR_NONE) == NULL);	// none: a list without a hint
	static const struct { tr_id_t hint; } zeroed;		// an initializer left out is none
	CHECK(zeroed.hint == TR_NONE);
#if CONFIG_LANG_ES
	CHECK(lang_built(es) && lang_set(es) && lang_current() == es);
	CHECK(strcmp(TR(TITLE_SETTINGS, "Settings"), "Ajustes") == 0);
	CHECK(strcmp(tr(settings), "Ajustes") == 0);
	CHECK(strcmp(tr_en(settings), "Settings") == 0);	// Find matches both
	CHECK(strcmp(TR(SET_LABEL_LANGUAGE, "Language"), "Idioma/Language") == 0);
	CHECK(strcmp(TR(SET_ROW_POWER_SAVING, "Power saving"), "Ahorro de energ\xED" "a") == 0);
	CHECK(strcmp(TR(LABEL_POWER, "Power"), "Potencia") == 0);
	CHECK(strcmp(TR(QUICK_LABEL_POWER, TEXT_ALT_OPEN "Power" TEXT_ALT_NEXT "Pwr" TEXT_ALT_CLOSE),
		     TEXT_ALT_OPEN "Pot" TEXT_ALT_NEXT "Pt" TEXT_ALT_CLOSE) == 0);
	CHECK(strcmp(TR(MSG_BODY_CANNED_ON_MY_WAY, "On my way"), "En camino") == 0);	// a canned message: sent in the language in use
	// Strings at other places in a stride of the table: the lookup steps over the ones before them
	CHECK(strcmp(TR(GPS_TITLE_STATUS, "Status"), "Estado") == 0);
	CHECK(strcmp(TR(DUAL_TITLE_ZONE_FOR, "Zone for"), "Zona para") == 0);
	CHECK(strcmp(TR(USBNET_WHY_NOT_ON_RADIO, "No USB network on this radio"),
		     "Esta radio no tiene red USB") == 0);
	// Text a table marks but no catalog translates (TRX): the English, in any language
	CHECK(strcmp(tr(TRX(CHAN_LABEL_RX, "RX")), "RX") == 0 && strcmp(tr_en(TRX(CHAN_LABEL_RX, "RX")), "RX") == 0);
	CHECK(tr(TR_NONE) == NULL);
	// A key's English, every one of them: the English table is whole and in step with tr_en
	for (tr_id_t id = 1; id < TR_KEYS; id++) {
		CHECK(tr_en(id) != NULL && tr_en(id)[0] != 0);
		CHECK(tr(id) != NULL && tr(id)[0] != 0);
	}
	// A format: the translation's text around the values, the English when there is none
	char line[48];
	sbuf_t sb;
	sb_init(&sb, line, sizeof line);
	TRF(&sb, MSG_BANNER_FROM, "Message from %1$s", "Bob");
	CHECK(strcmp(line, "Mensaje de Bob") == 0);
	sb_init(&sb, line, sizeof line);
	TRF(&sb, ZONE_TOAST_POSITION, "Position %1$u of %2$u", 2u, 9u);
	CHECK(strcmp(line, "Posici\xF3n 2 de 9") == 0);
	CHECK(lang_set(LANG_ENGLISH) && strcmp(TR(TITLE_SETTINGS, "Settings"), "Settings") == 0);
	CHECK(tr(settings) == tr_en(settings));
	sb_init(&sb, line, sizeof line);
	TRF(&sb, MSG_BANNER_FROM, "Message from %1$s", "Bob");
	CHECK(strcmp(line, "Message from Bob") == 0);
#else
	CHECK(!lang_built(es) && !lang_set(es) && lang_current() == LANG_ENGLISH);
	// Without a translation built a mark is its English itself: no table, no lookup
	CHECK(strcmp(TR(TITLE_SETTINGS, "Settings"), "Settings") == 0);
	const char *lit = TRN(TITLE_SETTINGS, "Settings");
	CHECK(tr(lit) == lit && tr_en(lit) == lit);
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
