// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The frame buffer's last bytes, when GFX_FB_BYTES is not a multiple of 4: a board with a safe-area frame buffer sizes
// it from the insets in its config.mk, so a safe area 221 px wide (CONFIG_DISPLAY_INSET_RIGHT=9) needs 111 B rows,
// 34,410 B in all. gfx_init accepts a safe area of exactly GFX_FB_BYTES, so drawing its bottom right corner must stay
// inside the buffer. Built with AddressSanitizer (tests/host/Makefile), which stops at a write past the array: a buffer
// of GFX_FB_BYTES / 4 words (34,408 B) fails here, one rounded up passes
#include <stdio.h>
#include <string.h>
#include "ui/gfx.h"

#if GFX_FB_BYTES % 4u == 0u
#error "this test needs a GFX_FB_BYTES that is not a multiple of 4"
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define PANEL_W		230
#define PANEL_H		320
#define INSET_RIGHT	9
#define INSET_BOTTOM	10
#define SAFE_W		(PANEL_W - INSET_RIGHT)		// 221 px: 111 B rows, one nibble spare
#define SAFE_H		(PANEL_H - INSET_BOTTOM)
#define STRIDE		((SAFE_W + 1) / 2)

static uint8_t last_row[STRIDE];
static int last_row_sent;
static uint16_t last_pal[16];

static void d_init(void) {}
static void d_backlight(uint8_t p) { (void)p; }
static void d_fill_insets(uint16_t c) { (void)c; }
static void d_flush_indexed(const void *fb, const uint16_t *pal, uint16_t y0, uint16_t y1)
{
	if (y1 == SAFE_H) {
		memcpy(last_row, (const uint8_t *)fb + (size_t)(SAFE_H - 1) * STRIDE, STRIDE);
		memcpy(last_pal, pal, sizeof last_pal);
		last_row_sent++;
	}
}

static const hal_display_t disp = {
	.width = PANEL_W, .height = PANEL_H, .bpp = 4, .inset_right = INSET_RIGHT, .inset_bottom = INSET_BOTTOM,
	.fb_safe_area = true, .fill_insets = d_fill_insets, .init = d_init, .flush_indexed = d_flush_indexed,
	.set_backlight = d_backlight,
};

int main(void)
{
	// The safe area fills the buffer to the byte, so gfx_init takes it
	_Static_assert(STRIDE * SAFE_H == GFX_FB_BYTES, "the safe area must be exactly GFX_FB_BYTES");
	CHECK(gfx_init(&disp));
	gfx_fb_rect_t r;
	gfx_fb_rect(&r);
	CHECK(r.safe_area && r.w == SAFE_W && r.h == SAFE_H && r.stride == STRIDE);

	// The whole safe area, then its bottom right pixel: the buffer's last byte, past GFX_FB_BYTES / 4 words
	const color_t bg = RGB565(0, 0, 255), red = RGB565(255, 0, 0);
	gfx_clear(bg);
	gfx_fill(0, 0, SAFE_W, SAFE_H, bg);
	gfx_pixel(SAFE_W - 1, SAFE_H - 1, red);
	gfx_pixel(SAFE_W - 2, SAFE_H - 1, red);
	CHECK(gfx_get_pixel(SAFE_W - 1, SAFE_H - 1) == red && gfx_get_pixel(SAFE_W - 3, SAFE_H - 1) == bg);

	// What the panel gets: the last row's last byte holds the red pixel in its high nibble
	gfx_flush();
	CHECK(last_row_sent == 1);
	CHECK(last_pal[last_row[STRIDE - 1] >> 4] == red && last_pal[last_row[STRIDE - 2] & 15] == red);
	CHECK(last_pal[last_row[STRIDE - 2] >> 4] == bg);

	if (fails) {
		printf("gfxFbEdgeTest: %d failed\n", fails);
		return 1;
	}
	printf("gfxFbEdgeTest: ok\n");
	return 0;
}
