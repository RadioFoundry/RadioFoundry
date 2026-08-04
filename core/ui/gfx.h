// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Drawing into the UI's frame buffer and sending changed rows to the display (hal_display_t).
// Works on any panel whose frame buffer fits GFX_FB_BYTES: RGB565 (bpp 16) or 1 bit per pixel (bpp 1, MSB left).
#ifndef UI_GFX_H
#define UI_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hal_display.h"
#include "font.h"

// Frame buffer size, static in main SRAM (DMA capable). 160 x 128 x 16 bpp = 40 KB. A 240 x 320 color panel
// (150 KB) does not fit the STM32F405's 128 KB SRAM and will need band rendering instead of a whole frame.
#define GFX_FB_BYTES	(160u * 128u * 2u)
#define GFX_MAX_ROWS	160u

// RGB565. On a 1 bpp panel any nonzero color is ink (pixel set), zero is paper
typedef uint16_t color_t;
#define RGB565(r, g, b)	((color_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

// Take over the display. False if its frame buffer doesn't fit (then nothing is drawn)
bool gfx_init(const hal_display_t *disp);
int gfx_width(void);
int gfx_height(void);
int gfx_bpp(void);
const void *gfx_framebuffer(void);

// Drawing is clipped to this rectangle (and always to the screen)
void gfx_clip(int x, int y, int w, int h);
void gfx_clip_off(void);

void gfx_pixel(int x, int y, color_t c);
color_t gfx_get_pixel(int x, int y);	// RGB565, or 0 / 1 on a 1 bpp panel
void gfx_fill(int x, int y, int w, int h, color_t c);
void gfx_clear(color_t c);
void gfx_hline(int x, int y, int w, color_t c);
void gfx_vline(int x, int y, int h, color_t c);
void gfx_rect(int x, int y, int w, int h, color_t c);	// outline
void gfx_round_fill(int x, int y, int w, int h, color_t c);	// corners cut by one pixel

// Text with its top-left corner at x, y. Returns the x after the last glyph
int gfx_text(const font_t *f, int x, int y, const char *s, color_t fg);
// The same with each glyph cell filled with bg first
int gfx_text_bg(const font_t *f, int x, int y, const char *s, color_t fg, color_t bg);
int gfx_text_width(const font_t *f, const char *s);
// Copy s into out, shortened from the middle with an ellipsis so it is at most max_w pixels wide
void gfx_text_fit(const font_t *f, const char *s, int max_w, char *out, size_t out_len);

// Icons drawn from primitives
// Battery outline w x h with a fill proportional to pct (0..100)
void gfx_icon_battery(int x, int y, int w, int h, uint8_t pct, color_t outline, color_t fill);
// Signal bars: count bars of rising height in a w x h box, lit = how many are on
void gfx_icon_bars(int x, int y, int w, int h, uint8_t count, uint8_t lit, color_t on, color_t off);
// A filled pill with text, padded; returns its width
int gfx_pill(const font_t *f, int x, int y, int h, const char *s, color_t fg, color_t bg);

// Send the rows that changed since the last flush (compared by a hash per row) to the display
void gfx_flush(void);
// Freeze the frame buffer for a screen capture: the UI skips redraws until ms (hal_millis) or gfx_hold(0)
void gfx_hold(uint32_t until_ms);
bool gfx_held(uint32_t now_ms);
// Make the next flush send every row
void gfx_invalidate(void);

#endif
