// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI's fonts. The bitmaps are Spleen 2.1.0 by Frederic Cambus (BSD-2-Clause, see core/ui/fonts/*.inc and
// LICENSES/BSD-2-Clause.txt), converted by tools/fontconv.py.
#include "font.h"

#include "fonts/spleen_5x8.inc"
#include "fonts/spleen_6x12.inc"
#include "fonts/spleen_8x16.inc"
#include "fonts/spleen_12x24.inc"
#if CONFIG_FONT_XL
#include "fonts/spleen_16x32.inc"
#endif

#define FONT(var, n, N) const font_t var = { N##_W, N##_H, N##_ASCENT, N##_MISSING, n##_bits }
FONT(font_5x8, spleen_5x8, SPLEEN_5X8);
FONT(font_6x12, spleen_6x12, SPLEEN_6X12);
FONT(font_8x16, spleen_8x16, SPLEEN_8X16);
FONT(font_12x24, spleen_12x24, SPLEEN_12X24);
#if CONFIG_FONT_XL
FONT(font_16x32, spleen_16x32, SPLEEN_16X32);
#endif

const uint8_t *font_glyph(const font_t *f, uint8_t c)
{
	unsigned slot;
	if (c >= 0x20 && c < 0x7F) {
		slot = c - 0x20u;
	} else if (c >= GLYPH_FIRST_EXTRA && c < GLYPH_FIRST_EXTRA + GLYPH_EXTRA_COUNT) {
		if (f->missing & (1u << (c - GLYPH_FIRST_EXTRA)))
			return NULL;
		slot = 95u + (c - GLYPH_FIRST_EXTRA);
	} else {
		return NULL;
	}
	return f->bits + slot * (unsigned)f->h * ((f->w + 7u) / 8u);
}
