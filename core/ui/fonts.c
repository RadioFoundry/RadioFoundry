// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI's fonts. The bitmaps are Spleen 2.1.0 by Frederic Cambus (BSD-2-Clause, see core/ui/fonts/spleen_*.inc and
// LICENSES/BSD-2-Clause.txt). CONFIG_FONT_AA replaces the sizes from 6x12 up with JetBrains Mono Bold, anti-aliased,
// for color panels (font.h), and CONFIG_FONT_BIG adds its 9x18 (OFL-1.1, core/ui/fonts/jbmono_*.inc and
// LICENSES/OFL-1.1.txt). Both converted by tools/fontconv.py.
#include "font.h"

#define FONT(var, n, N, bpp) const font_t var = { N##_W, N##_H, N##_ASCENT, N##_MISSING, bpp, n##_bits, n##_index }

#include "fonts/spleen_5x8.inc"
FONT(font_5x8, spleen_5x8, SPLEEN_5X8, 1);

#if CONFIG_FONT_AA
#include "fonts/jbmono_6x12.inc"
#include "fonts/jbmono_8x16.inc"
#include "fonts/jbmono_12x24.inc"
#if CONFIG_FONT_BIG
#include "fonts/jbmono_9x18.inc"
#endif
#if CONFIG_FONT_XL
#include "fonts/jbmono_16x32.inc"
#endif
#ifdef FONT_BOTH
bool font_aa_on = true;
#if CONFIG_FONT_BIG
bool font_big_on = true;
#endif
#define AA(n)	font_aa_##n
#else
#define AA(n)	font_##n
#endif
FONT(AA(6x12), jbmono_6x12, JBMONO_6X12, JBMONO_6X12_BPP);
FONT(AA(8x16), jbmono_8x16, JBMONO_8X16, JBMONO_8X16_BPP);
FONT(AA(12x24), jbmono_12x24, JBMONO_12X24, JBMONO_12X24_BPP);
#if CONFIG_FONT_BIG
FONT(AA(9x18), jbmono_9x18, JBMONO_9X18, JBMONO_9X18_BPP);
#endif
#if CONFIG_FONT_XL
FONT(AA(16x32), jbmono_16x32, JBMONO_16X32, JBMONO_16X32_BPP);
#endif
#endif

#if !CONFIG_FONT_AA || defined(FONT_BOTH)
#include "fonts/spleen_6x12.inc"
#include "fonts/spleen_8x16.inc"
#include "fonts/spleen_12x24.inc"
FONT(font_6x12, spleen_6x12, SPLEEN_6X12, 1);
FONT(font_8x16, spleen_8x16, SPLEEN_8X16, 1);
FONT(font_12x24, spleen_12x24, SPLEEN_12X24, 1);
#if CONFIG_FONT_XL
#include "fonts/spleen_16x32.inc"
FONT(font_16x32, spleen_16x32, SPLEEN_16X32, 1);
#endif
#endif

const uint8_t *font_glyph(const font_t *f, uint8_t c, int *top, int *rows)
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
	const uint8_t *g = f->bits + f->index[slot];
	*top = g[0];
	*rows = g[1];
	return g + 2;
}
