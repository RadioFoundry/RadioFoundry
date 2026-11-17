// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CJK face (CONFIG_LANG_ZH): a 16x16 one-bit glyph for every character core/ui/lang/zh.po uses, which
// tools/cjkfont.py rasterizes from an outline font into cjk_font_data.c. The Latin fonts have no glyph for Chinese, so
// text is UTF-8 and the text layer (gfx.c) looks each character up here. The cell is 16 pixels square, the height of
// the 8x16 font, so a line with Chinese keeps that height (docs/spec/ui.md 2.5).
#ifndef UI_CJK_FONT_H
#define UI_CJK_FONT_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#define CJK_FONT_W		16
#define CJK_FONT_H		16
#define CJK_FONT_ROW_BYTES	2
#define CJK_FONT_BYTES		(CJK_FONT_H * CJK_FONT_ROW_BYTES)

#if CONFIG_LANG_ZH
// The glyph of a code point: CJK_FONT_H rows of CJK_FONT_ROW_BYTES bytes, MSB the left of the 16, or NULL when the
// font has none (the text layer draws a box)
const uint8_t *cjk_font_glyph(uint32_t cp);
#endif

#endif
