// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fixed-cell fonts: ASCII 0x20..0x7E plus a few extra glyphs reached through the byte codes below. Glyphs are 1 bit a
// pixel (ink or not) or 2 (anti-aliased: coverage 0..3, blended with what is under them on color panels)
#ifndef UI_FONT_H
#define UI_FONT_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include <stddef.h>

// Extra glyphs, as bytes inside ordinary C strings ("PTT" GLYPH_ARROW "TG 91")
#define GLYPH_ELLIPSIS	"\x80"
#define GLYPH_DOT	"\x81"	// middle dot: marks a temporary change
#define GLYPH_ARROW	"\x82"	// right arrow
#define GLYPH_DEGREE	"\x83"
// Text alternates: TEXT_ALT_OPEN "14min ago" TEXT_ALT_NEXT "14m ago" ... TEXT_ALT_CLOSE offers the same text at
// falling lengths. Drawing and measuring use the first; gfx_text_fit uses the longest that fits (gfx.c)
#define TEXT_ALT_OPEN	"\x1d"
#define TEXT_ALT_NEXT	"\x1f"
#define TEXT_ALT_CLOSE	"\x1e"
#define GLYPH_FIRST_EXTRA	0x80
#define GLYPH_EXTRA_COUNT	4

typedef struct {
	uint8_t w, h;		// cell size in pixels; every glyph advances w
	uint8_t ascent;		// baseline, rows from the top of the cell
	uint8_t missing;	// bit n: extra glyph n is not in the font (drawn from primitives instead)
	uint8_t bpp;		// 1 or 2
	// Glyph after glyph: its first inked row, the number of rows from there, then those rows of (w * bpp + 7) / 8
	// bytes, MSB = left (the blank rows above and below aren't stored). index: where each glyph starts in bits
	const uint8_t *bits;
	const uint16_t *index;
} font_t;

extern const font_t font_5x8, font_6x12, font_8x16, font_12x24;
#if CONFIG_FONT_XL
extern const font_t font_16x32;
#define FONT_LARGEST	(&font_16x32)
#else
#define FONT_LARGEST	(&font_12x24)	// built without the 16x32 font (CONFIG_FONT_XL)
#endif
// CONFIG_FONT_AA: the sizes from 6x12 up are anti-aliased, for color panels. A firmware build links only those (the
// font_NxM names are them); the host simulator (FONT_BOTH) keeps both sets for its mono display class and picks the
// font_aa_ ones on color panels. Otherwise font_aa_ names the same font as font_
#if CONFIG_FONT_AA && defined(FONT_BOTH)
extern bool font_aa_on;		// the simulator's switch: a display class whose radio is built without FONT_AA
extern const font_t font_aa_6x12, font_aa_8x16, font_aa_12x24;
#if CONFIG_FONT_BIG
extern bool font_big_on;	// the simulator's switch: a display class whose radio is built without FONT_BIG
extern const font_t font_aa_9x18;
#endif
#if CONFIG_FONT_XL
extern const font_t font_aa_16x32;
#define FONT_AA_LARGEST	(&font_aa_16x32)
#else
#define FONT_AA_LARGEST	(&font_aa_12x24)
#endif
#else
#define font_aa_6x12	font_6x12
#define font_aa_8x16	font_8x16
#define font_aa_12x24	font_12x24
#define FONT_AA_LARGEST	FONT_LARGEST
#if CONFIG_FONT_BIG
extern const font_t font_9x18;
#define font_aa_9x18	font_9x18
#endif
#endif

// Rows of a glyph from row *top of the cell, *rows of them (0 for a space), or NULL for a byte the font can't draw
// (shown as a box) or a missing extra
const uint8_t *font_glyph(const font_t *f, uint8_t c, int *top, int *rows);

#endif
