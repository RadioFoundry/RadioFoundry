// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Fixed-cell bitmap fonts: ASCII 0x20..0x7E plus a few extra glyphs reached through the byte codes below
#ifndef UI_FONT_H
#define UI_FONT_H

#include <stdint.h>
#include <stddef.h>

// Extra glyphs, as bytes inside ordinary C strings ("PTT" GLYPH_ARROW "TG 91")
#define GLYPH_ELLIPSIS	"\x80"
#define GLYPH_DOT	"\x81"	// middle dot: marks a temporary change
#define GLYPH_ARROW	"\x82"	// right arrow
#define GLYPH_DEGREE	"\x83"
#define GLYPH_FIRST_EXTRA	0x80
#define GLYPH_EXTRA_COUNT	4

typedef struct {
	uint8_t w, h;		// cell size in pixels; every glyph advances w
	uint8_t ascent;		// baseline, rows from the top of the cell
	uint8_t missing;	// bit n: extra glyph n is not in the font (drawn from primitives instead)
	const uint8_t *bits;	// glyph after glyph, h rows of (w + 7) / 8 bytes, MSB = left
} font_t;

extern const font_t font_5x8, font_6x12, font_8x16, font_12x24, font_16x32;

// Bitmap of a glyph, or NULL for a byte the font can't draw (shown as a box) or a missing extra
const uint8_t *font_glyph(const font_t *f, uint8_t c);

#endif
