// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Lookup in the generated CJK face: a sorted array of the code points it has, and a glyph per entry, so a character
// costs a binary search and no RAM. cjk_font_data.c is built by tools/cjkfont.py from the catalogs themselves; this
// file is the search, and stays small on purpose.
#include <stddef.h>
#include "cjk_font.h"

const uint8_t *cjk_font_glyph(uint32_t cp)
{
#if CONFIG_CJK_GLYPHS
	// core/ui/cjk_font_data.c, built by tools/cjkfont.py
	extern const uint16_t cjk_font_cp[];
	extern const uint8_t cjk_font_bits[];
	extern const uint16_t cjk_font_count;
	if (cp > 0xFFFF)
		return NULL;			// the table holds the BMP only: every CJK block is below it
	uint16_t lo = 0, hi = cjk_font_count;
	while (lo < hi) {
		uint16_t mid = (uint16_t)((lo + hi) / 2u);
		uint16_t at = cjk_font_cp[mid];
		if (at == (uint16_t)cp)
			return cjk_font_bits + (size_t)mid * CJK_FONT_BYTES;
		if (at < (uint16_t)cp)
			lo = (uint16_t)(mid + 1u);
		else
			hi = mid;
	}
#endif
	return NULL;			// no table built (CONFIG_CJK_GLYPHS off): the text layer draws a box
}
