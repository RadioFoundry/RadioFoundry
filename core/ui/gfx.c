// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Frame buffer drawing: primitives, text, icons, and a flush that sends only the rows whose content changed
#include <string.h>
#include "gfx.h"

static uint32_t fb_words[GFX_FB_BYTES / 4];
static const hal_display_t *display;
static int width, height, bpp, stride;	// stride in bytes
static int clip_x0, clip_y0, clip_x1, clip_y1;	// x1, y1 exclusive
static uint32_t row_hash[GFX_MAX_ROWS];
static bool all_dirty;

bool gfx_init(const hal_display_t *disp)
{
	display = disp;
	width = disp->width;
	height = disp->height;
	bpp = disp->bpp;
	stride = bpp == 1 ? (width + 7) / 8 : width * 2;
	if ((bpp != 1 && bpp != 16) || (uint32_t)stride * (uint32_t)height > GFX_FB_BYTES || height > (int)GFX_MAX_ROWS) {
		width = height = 0;
		display = NULL;
		return false;
	}
	gfx_clip_off();
	all_dirty = true;
	return true;
}

int gfx_width(void) { return width; }
int gfx_height(void) { return height; }
int gfx_bpp(void) { return bpp; }
const void *gfx_framebuffer(void) { return fb_words; }

void gfx_clip(int x, int y, int w, int h)
{
	clip_x0 = x < 0 ? 0 : x;
	clip_y0 = y < 0 ? 0 : y;
	clip_x1 = x + w > width ? width : x + w;
	clip_y1 = y + h > height ? height : y + h;
}

void gfx_clip_off(void)
{
	clip_x0 = clip_y0 = 0;
	clip_x1 = width;
	clip_y1 = height;
}

// Fill a span of one row, already clipped
static void span(int x0, int x1, int y, color_t c)
{
	uint8_t *row = (uint8_t *)fb_words + y * stride;
	if (bpp == 16) {
		uint16_t *p = (uint16_t *)row + x0;
		for (int x = x0; x < x1; x++)
			*p++ = c;
	} else {
		for (int x = x0; x < x1; x++) {
			uint8_t m = (uint8_t)(0x80u >> (x & 7));
			if (c)
				row[x >> 3] |= m;
			else
				row[x >> 3] &= (uint8_t)~m;
		}
	}
}

void gfx_fill(int x, int y, int w, int h, color_t c)
{
	int x0 = x < clip_x0 ? clip_x0 : x;
	int y0 = y < clip_y0 ? clip_y0 : y;
	int x1 = x + w > clip_x1 ? clip_x1 : x + w;
	int y1 = y + h > clip_y1 ? clip_y1 : y + h;
	for (int yy = y0; yy < y1; yy++)
		if (x0 < x1)
			span(x0, x1, yy, c);
}

void gfx_pixel(int x, int y, color_t c)
{
	if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1)
		span(x, x + 1, y, c);
}

color_t gfx_get_pixel(int x, int y)
{
	if (x < 0 || y < 0 || x >= width || y >= height)
		return 0;
	const uint8_t *row = (const uint8_t *)fb_words + y * stride;
	if (bpp == 16)
		return ((const uint16_t *)row)[x];
	return (row[x >> 3] >> (7 - (x & 7))) & 1u;
}

void gfx_clear(color_t c)
{
	int sx0 = clip_x0, sy0 = clip_y0, sx1 = clip_x1, sy1 = clip_y1;
	gfx_clip_off();
	gfx_fill(0, 0, width, height, c);
	clip_x0 = sx0; clip_y0 = sy0; clip_x1 = sx1; clip_y1 = sy1;
}

void gfx_hline(int x, int y, int w, color_t c) { gfx_fill(x, y, w, 1, c); }
void gfx_vline(int x, int y, int h, color_t c) { gfx_fill(x, y, 1, h, c); }

void gfx_rect(int x, int y, int w, int h, color_t c)
{
	gfx_hline(x, y, w, c);
	gfx_hline(x, y + h - 1, w, c);
	gfx_vline(x, y + 1, h - 2, c);
	gfx_vline(x + w - 1, y + 1, h - 2, c);
}

void gfx_round_fill(int x, int y, int w, int h, color_t c)
{
	gfx_fill(x + 1, y, w - 2, h, c);
	gfx_vline(x, y + 1, h - 2, c);
	gfx_vline(x + w - 1, y + 1, h - 2, c);
}

// Extra glyphs a font lacks, drawn from primitives inside the cell at x, y
static void fallback_glyph(const font_t *f, int x, int y, uint8_t c, color_t fg)
{
	int dot = f->w >= 10 ? 2 : 1;
	int base = y + f->ascent;
	switch (c) {
	case 0x80: {	// ellipsis: three dots on the baseline
		int gap = f->w / 3;
		for (int i = 0; i < 3; i++)
			gfx_fill(x + gap / 2 + i * gap, base - dot, dot, dot, fg);
		break;
	}
	case 0x81:	// middle dot
		gfx_fill(x + (f->w - dot * 2) / 2, y + f->ascent / 2 + 1, dot * 2, dot * 2, fg);
		break;
	case 0x82: {	// right arrow
		int mid = y + f->ascent / 2 + 1;
		int len = f->w - 1;
		gfx_hline(x, mid, len, fg);
		for (int i = 1; i <= f->w / 3; i++) {
			gfx_pixel(x + len - 1 - i, mid - i, fg);
			gfx_pixel(x + len - 1 - i, mid + i, fg);
		}
		break;
	}
	case 0x83:	// degree
		gfx_rect(x + 1, y + 1, dot + 2, dot + 2, fg);
		break;
	default:	// not drawable: a box
		gfx_rect(x + 1, y + 1, f->w - 2, f->ascent - 1, fg);
		break;
	}
}

static int text(const font_t *f, int x, int y, const char *s, color_t fg, bool opaque, color_t bg)
{
	int bytes = (f->w + 7) / 8;
	for (; *s; s++, x += f->w) {
		if (x >= clip_x1)
			break;
		if (x + f->w <= clip_x0 || y >= clip_y1 || y + f->h <= clip_y0)
			continue;
		if (opaque)
			gfx_fill(x, y, f->w, f->h, bg);
		const uint8_t *g = font_glyph(f, (uint8_t)*s);
		if (g == NULL) {
			fallback_glyph(f, x, y, (uint8_t)*s, fg);
			continue;
		}
		for (int r = 0; r < f->h; r++, g += bytes) {
			int yy = y + r;
			if (yy < clip_y0 || yy >= clip_y1)
				continue;
			for (int col = 0; col < f->w; col++) {
				if (g[col >> 3] & (0x80u >> (col & 7))) {
					int xx = x + col;
					if (xx >= clip_x0 && xx < clip_x1)
						span(xx, xx + 1, yy, fg);
				}
			}
		}
	}
	return x;
}

int gfx_text(const font_t *f, int x, int y, const char *s, color_t fg)
{
	return text(f, x, y, s, fg, false, 0);
}

int gfx_text_bg(const font_t *f, int x, int y, const char *s, color_t fg, color_t bg)
{
	return text(f, x, y, s, fg, true, bg);
}

int gfx_text_width(const font_t *f, const char *s)
{
	return (int)strlen(s) * f->w;
}

void gfx_text_fit(const font_t *f, const char *s, int max_w, char *out, size_t out_len)
{
	size_t n = strlen(s);
	size_t fit = max_w <= 0 ? 0 : (size_t)(max_w / f->w);
	if (out_len == 0)
		return;
	if (fit > out_len - 1)
		fit = out_len - 1;
	if (n <= fit) {
		memcpy(out, s, n + 1);
		return;
	}
	if (fit == 0) {
		out[0] = 0;
		return;
	}
	// Keep the start and the end, one glyph for the ellipsis between them (start gets the odd one)
	size_t keep = fit - 1;
	size_t head = (keep + 1) / 2, tail = keep - head;
	memcpy(out, s, head);
	out[head] = GLYPH_ELLIPSIS[0];
	memcpy(out + head + 1, s + n - tail, tail);
	out[fit] = 0;
}

void gfx_icon_battery(int x, int y, int w, int h, uint8_t pct, color_t outline, color_t fill)
{
	int nub = h >= 8 ? 2 : 1;
	int body = w - nub;
	gfx_rect(x, y, body, h, outline);
	gfx_fill(x + body, y + h / 4, nub, h - 2 * (h / 4), outline);
	int inner = body - 4;
	int lit = pct >= 100 ? inner : (inner * pct + 50) / 100;
	if (pct > 0 && lit == 0)
		lit = 1;
	gfx_fill(x + 2, y + 2, lit, h - 4, fill);
}

void gfx_icon_bars(int x, int y, int w, int h, uint8_t count, uint8_t lit, color_t on, color_t off)
{
	if (count == 0)
		return;
	int pitch = (w + 1) / count;	// bar plus one pixel gap
	int bw = pitch - 1 < 1 ? 1 : pitch - 1;
	for (int i = 0; i < count; i++) {
		int bh = h * (i + 1) / count;
		if (bh < 1)
			bh = 1;
		color_t c = i < lit ? on : off;
		if (i < lit || off != on)
			gfx_fill(x + i * pitch, y + h - bh, bw, bh, c);
	}
}

int gfx_pill(const font_t *f, int x, int y, int h, const char *s, color_t fg, color_t bg)
{
	int pad = f->w >= 8 ? 4 : 3;
	int w = gfx_text_width(f, s) + 2 * pad;
	gfx_round_fill(x, y, w, h, bg);
	gfx_text(f, x + pad, y + (h - f->h + 1) / 2, s, fg);
	return w;
}

static uint32_t hash_row(int y)
{
	const uint8_t *p = (const uint8_t *)fb_words + y * stride;
	uint32_t h = 2166136261u;	// FNV-1a
	for (int i = 0; i < stride; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

void gfx_flush(void)
{
	if (display == NULL)
		return;
	int band = -1;
	for (int y = 0; y < height; y++) {
		uint32_t h = hash_row(y);
		bool changed = all_dirty || h != row_hash[y];
		row_hash[y] = h;
		if (changed && band < 0)
			band = y;
		if (!changed && band >= 0) {
			display->flush(fb_words, (uint16_t)band, (uint16_t)y);
			band = -1;
		}
	}
	if (band >= 0)
		display->flush(fb_words, (uint16_t)band, (uint16_t)height);
	all_dirty = false;
}

void gfx_invalidate(void)
{
	all_dirty = true;
}
