// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Frame buffer drawing: primitives, text, icons, and a flush that sends only the rows whose content changed
#include <string.h>
#include "gfx.h"

static uint32_t fb_words[GFX_FB_BYTES / 4];
static const hal_display_t *display;
static int width, height, bpp, stride;	// stride in bytes
static int clip_x0, clip_y0, clip_x1, clip_y1;	// panel coordinates, x1, y1 exclusive
// The viewport (the board's safe area): every drawing call takes coordinates relative to it
static int ox, oy, vw, vh;
static uint32_t row_hash[GFX_MAX_ROWS];
static bool all_dirty;
#if CONFIG_NET_WEBCTL
static uint8_t row_new[(GFX_MAX_ROWS + 7u) / 8u];	// bit y: row y changed since the reader took it
static volatile uint32_t frames_begun, frames_done;
#endif
#if GFX_INDEXED
// bpp 4: the colors the indexes stand for. An entry is only given a new color once nothing drawn since the last clear
// uses it, so no pixel on the screen changes color under it
static color_t pal[16], pal_sent[16];	// pal_sent: as the display last got it
static uint8_t pal_n, pal_n_sent;	// entries set
static uint16_t pal_used;		// bit n: entry n drawn since the last gfx_clear
static color_t last_color;		// the last lookup, for runs of one color (text is drawn pixel by pixel)
static uint8_t last_index = 0xFFu;	// 0xFF: none

static inline void put4(uint8_t *row, int x, uint8_t i)
{
	uint8_t *b = row + (x >> 1);
	*b = (x & 1) ? (uint8_t)((*b & 0xF0u) | i) : (uint8_t)((*b & 0x0Fu) | (i << 4));
}

static void pal_reset(color_t c)
{
	pal[0] = c;
	pal_n = 1;
	pal_used = 1;
	last_index = 0xFFu;
}

// Squared distance, each channel scaled to 6 bits and weighted for the eye (green most, blue least)
static uint32_t color_dist(color_t a, color_t b)
{
	int dr = (int)((a >> 11) - (b >> 11)) * 2, dg = (int)((a >> 5) & 0x3Fu) - (int)((b >> 5) & 0x3Fu);
	int db = (int)(a & 0x1Fu) * 2 - (int)(b & 0x1Fu) * 2;
	return (uint32_t)(3 * dr * dr + 4 * dg * dg + 2 * db * db);
}

// Entries a blend may not take: anti-aliased edges must not use up the slots solid colors need. Past this, an edge
// gets the nearest color already there (on a dark theme the grays, which suit it)
#define PAL_SOLID_RESERVE	4

static uint8_t pal_index(color_t c)
{
	if (last_index != 0xFFu && c == last_color)
		return last_index;
	uint8_t i = 0;
	while (i < pal_n && pal[i] != c)
		i++;
	if (i == pal_n) {
		if (pal_n < 16) {
			pal[pal_n++] = c;
		} else {
			for (i = 0; i < 16 && (pal_used >> i) & 1u; i++) {
			}
			if (i < 16) {
				pal[i] = c;	// an entry no pixel uses any more
			} else {	// 16 colors on the screen: the nearest
				uint32_t best = UINT32_MAX;
				for (uint8_t k = 0; k < 16; k++) {
					uint32_t d = color_dist(c, pal[k]);
					if (d < best) {
						best = d;
						i = k;
					}
				}
			}
		}
	}
	pal_used |= (uint16_t)(1u << i);
	last_color = c;
	last_index = i;
	return i;
}

#if CONFIG_FONT_AA
static uint8_t pal_index_blend(color_t c)
{
	int free_n = 16;
	for (uint8_t i = 0; i < pal_n; i++) {
		if (pal[i] == c) {
			pal_used |= (uint16_t)(1u << i);
			return i;
		}
		if ((pal_used >> i) & 1u)
			free_n--;
	}
	if (free_n > PAL_SOLID_RESERVE)
		return pal_index(c);
	uint8_t best_i = 0;
	uint32_t best = UINT32_MAX;
	for (uint8_t k = 0; k < pal_n; k++) {
		if (!((pal_used >> k) & 1u))
			continue;	// free: its color isn't on the screen
		uint32_t d = color_dist(c, pal[k]);
		if (d < best) {
			best = d;
			best_i = k;
		}
	}
	return best_i;
}
#endif
#endif

#if CONFIG_FONT_AA
// a/3 of fg over bg, per channel
static color_t blend(color_t fg, color_t bg, unsigned a)
{
	unsigned r = ((fg >> 11) * a + (bg >> 11) * (3u - a) + 1u) / 3u;
	unsigned g = (((fg >> 5) & 0x3Fu) * a + ((bg >> 5) & 0x3Fu) * (3u - a) + 1u) / 3u;
	unsigned b = ((fg & 0x1Fu) * a + (bg & 0x1Fu) * (3u - a) + 1u) / 3u;
	return (color_t)(r << 11 | g << 5 | b);
}
#endif

bool gfx_init(const hal_display_t *disp)
{
	display = disp;
	width = disp->width;
	height = disp->height;
	bpp = disp->bpp;
	stride = bpp == 1 ? (width + 7) / 8 : bpp == 4 ? (width + 1) / 2 : width * 2;
	if ((bpp != 1 && bpp != 16 && (bpp != 4 || !GFX_INDEXED || disp->flush_indexed == NULL)) ||
	    (uint32_t)stride * (uint32_t)height > GFX_FB_BYTES || height > (int)GFX_MAX_ROWS) {
		width = height = 0;
		display = NULL;
		return false;
	}
	ox = disp->inset_left;
	oy = disp->inset_top;
	vw = width - disp->inset_left - disp->inset_right;
	vh = height - disp->inset_top - disp->inset_bottom;
	if (vw < 32 || vh < 32) {	// insets that leave nothing usable: ignore them
		ox = oy = 0;
		vw = width;
		vh = height;
	}
	gfx_clip_off();
#if GFX_INDEXED
	pal_reset(0);		// the frame buffer starts as zeros
	pal_n_sent = 0;
#endif
	all_dirty = true;
	return true;
}

int gfx_width(void) { return vw; }
int gfx_height(void) { return vh; }
int gfx_bpp(void) { return bpp; }
const void *gfx_framebuffer(void) { return fb_words; }

void gfx_clip(int x, int y, int w, int h)
{
	clip_x0 = x < 0 ? ox : ox + x;
	clip_y0 = y < 0 ? oy : oy + y;
	clip_x1 = x + w > vw ? ox + vw : ox + x + w;
	clip_y1 = y + h > vh ? oy + vh : oy + y + h;
}

void gfx_clip_off(void)
{
	clip_x0 = ox;
	clip_y0 = oy;
	clip_x1 = ox + vw;
	clip_y1 = oy + vh;
}

// Fill a span of one row, already clipped
static void span(int x0, int x1, int y, color_t c)
{
	uint8_t *row = (uint8_t *)fb_words + y * stride;
	if (bpp == 16) {
		uint16_t *p = (uint16_t *)row + x0;
		for (int x = x0; x < x1; x++)
			*p++ = c;
#if GFX_INDEXED
	} else if (bpp == 4) {
		uint8_t i = pal_index(c);
		for (int x = x0; x < x1; x++)
			put4(row, x, i);
#endif
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

// A panel pixel, already clipped: RGB565 (0 / 1 at bpp 1)
static color_t raw_pixel(int x, int y)
{
	const uint8_t *row = (const uint8_t *)fb_words + y * stride;
	if (bpp == 16)
		return ((const uint16_t *)row)[x];
#if GFX_INDEXED
	if (bpp == 4)
		return pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0x0Fu];
#endif
	return (row[x >> 3] >> (7 - (x & 7))) & 1u;
}

#if CONFIG_FONT_AA
// An anti-aliased edge pixel: coverage a (1 or 2 of 3) of fg over what is there. On 1 bpp panels ink from 2 up
static void edge_pixel(int x, int y, color_t fg, unsigned a)
{
	if (bpp == 1) {
		if (a >= 2)
			span(x, x + 1, y, fg);
		return;
	}
	color_t c = blend(fg, raw_pixel(x, y), a);
#if GFX_INDEXED
	if (bpp == 4) {
		put4((uint8_t *)fb_words + y * stride, x, pal_index_blend(c));
		return;
	}
#endif
	span(x, x + 1, y, c);
}
#endif

#ifdef GFX_FIT_TRACE
static void trace(gfx_trace_kind_t kind, int x, int y, int w, int h, const char *s)
{
	const int clip[4] = { clip_x0 - ox, clip_y0 - oy, clip_x1 - ox, clip_y1 - oy };
	gfx_draw_trace(kind, x, y, w, h, clip, s);
}
#endif

void gfx_fill(int x, int y, int w, int h, color_t c)
{
#ifdef GFX_FIT_TRACE
	trace(GFX_TRACE_FILL, x, y, w, h, NULL);
#endif
	x += ox;
	y += oy;
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
	x += ox;
	y += oy;
	if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1)
		span(x, x + 1, y, c);
}

color_t gfx_get_pixel(int x, int y)
{
	if (x < 0 || y < 0 || x >= vw || y >= vh)
		return 0;
	x += ox;
	y += oy;
	return raw_pixel(x, y);
}

void gfx_clear(color_t c)
{
#ifdef GFX_FIT_TRACE
	trace(GFX_TRACE_CLEAR, 0, 0, vw, vh, NULL);
#endif
#if GFX_INDEXED
	if (bpp == 4) {		// nothing drawn before is left: the palette starts over with c
		pal_reset(c);
		memset(fb_words, 0, (size_t)stride * (size_t)height);
		return;
	}
#endif
	for (int y = 0; y < height; y++)	// the whole panel, margins included
		span(0, width, y, c);
}

void gfx_read_rgb565(uint32_t off, uint8_t *out, size_t n)
{
	uint32_t total = (uint32_t)width * (uint32_t)height * 2u;
	for (size_t k = 0; k < n; k++, off++) {
		color_t c = 0;
		if (off < total && bpp != 1)
			c = raw_pixel((int)(off / 2u % (uint32_t)width), (int)(off / 2u / (uint32_t)width));
		out[k] = (uint8_t)((off & 1u) ? c >> 8 : c);
	}
}

color_t gfx_palette(uint8_t i)
{
	if (bpp == 1)
		return i == 0 ? GFX_PAPER : GFX_INK;
#if GFX_INDEXED
	if (bpp == 4 && i < pal_n)
		return pal[i];
#endif
	return 0;
}

void gfx_hline(int x, int y, int w, color_t c) { gfx_fill(x, y, w, 1, c); }
void gfx_vline(int x, int y, int h, color_t c) { gfx_fill(x, y, 1, h, c); }

// gfx_fill directly, not through the line helpers: one frame less on the deepest drawing path (stack-check.md)
void gfx_rect(int x, int y, int w, int h, color_t c)
{
	gfx_fill(x, y, w, 1, c);
	gfx_fill(x, y + h - 1, w, 1, c);
	gfx_fill(x, y + 1, 1, h - 2, c);
	gfx_fill(x + w - 1, y + 1, 1, h - 2, c);
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

// Expands the text alternates in s (font.h): every group gives its alternative number level, or its last one. *more
// says whether a higher level would change the result
static size_t alt_expand(const char *s, int level, char *out, size_t cap, bool *more)
{
	size_t n = 0;
	*more = false;
	while (*s) {
		if (*s != TEXT_ALT_OPEN[0]) {
			if (n + 1 < cap)
				out[n++] = *s;
			s++;
			continue;
		}
		s++;
		for (int alt = 0; *s && *s != TEXT_ALT_CLOSE[0]; alt++) {
			const char *end = s;
			while (*end && *end != TEXT_ALT_NEXT[0] && *end != TEXT_ALT_CLOSE[0])
				end++;
			bool last = *end != TEXT_ALT_NEXT[0];
			if (alt == level || (alt < level && last)) {
				for (; s < end; s++)
					if (n + 1 < cap)
						out[n++] = *s;
			}
			if (alt == level && !last)
				*more = true;
			s = last ? end : end + 1;
		}
		if (*s)
			s++;
	}
	if (cap)
		out[n] = 0;
	return n;
}

#define ALT_TEXT_MAX	96

size_t gfx_text_alt(const char *s, int level, char *out, size_t cap, bool *more)
{
	return alt_expand(s, level, out, cap, more);
}

static int text(const font_t *f, int x, int y, const char *s, color_t fg, bool opaque, color_t bg)
{
	char first[ALT_TEXT_MAX];
	if (strchr(s, TEXT_ALT_OPEN[0])) {
		bool more;
		alt_expand(s, 0, first, sizeof first, &more);
		s = first;
	}
#ifdef GFX_FIT_TRACE
	const char *s0 = s;
	int x0 = x;
#endif
	int bytes = (f->w * f->bpp + 7) / 8;
	for (; *s; s++, x += f->w) {
		int px = x + ox, py = y + oy;	// panel coordinates for the clip tests and spans
		if (px >= clip_x1)
			break;
		if (px + f->w <= clip_x0 || py >= clip_y1 || py + f->h <= clip_y0)
			continue;
		if (opaque)
			gfx_fill(x, y, f->w, f->h, bg);
		int top, rows;
		const uint8_t *g = font_glyph(f, (uint8_t)*s, &top, &rows);
		if (g == NULL) {
			fallback_glyph(f, x, y, (uint8_t)*s, fg);
			continue;
		}
		for (int r = 0; r < rows; r++, g += bytes) {
			int yy = py + top + r;
			if (yy < clip_y0 || yy >= clip_y1)
				continue;
			for (int col = 0; col < f->w; col++) {
#if CONFIG_FONT_AA
				unsigned a = f->bpp == 2 ? (g[col >> 2] >> (6 - 2 * (col & 3))) & 3u :
					     (g[col >> 3] & (0x80u >> (col & 7))) ? 3u : 0;
#else
				unsigned a = (g[col >> 3] & (0x80u >> (col & 7))) ? 3u : 0;
#endif
				int xx = px + col;
				if (a == 0 || xx < clip_x0 || xx >= clip_x1)
					continue;
#if CONFIG_FONT_AA
				if (a != 3) {
					edge_pixel(xx, yy, fg, a);
					continue;
				}
#endif
				span(xx, xx + 1, yy, fg);
			}
		}
	}
#ifdef GFX_FIT_TRACE
	trace(GFX_TRACE_TEXT, x0, y, (int)strlen(s0) * f->w, f->h, s0);	// after the glyph cells gfx_text_bg fills
#endif
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
	if (strchr(s, TEXT_ALT_OPEN[0])) {
		char first[ALT_TEXT_MAX];
		bool more;
		return (int)alt_expand(s, 0, first, sizeof first, &more) * f->w;
	}
	return (int)strlen(s) * f->w;
}

void gfx_text_fit(const font_t *f, const char *src, int max_w, char *out, size_t out_len)
{
	size_t fit = max_w <= 0 ? 0 : (size_t)(max_w / f->w);
	if (out_len == 0)
		return;
	if (fit > out_len - 1)
		fit = out_len - 1;
	// The longest set of alternates that fits; past the last one, shorten that with an ellipsis
	char alt[ALT_TEXT_MAX];
	const char *s = src;
	size_t n = strlen(s);
	if (strchr(src, TEXT_ALT_OPEN[0])) {
		bool more = true;
		for (int level = 0; more; level++) {
			n = alt_expand(src, level, alt, sizeof alt, &more);
			if (n <= fit)
				break;
		}
		s = alt;
	}
	if (n <= fit) {
		memcpy(out, s, n + 1);
		return;
	}
	if (fit == 0) {
		out[0] = 0;
#ifdef GFX_FIT_TRACE
		gfx_fit_trace(s, out);
#endif
		return;
	}
	// Keep the start and the end, one glyph for the ellipsis between them (start gets the odd one)
	size_t keep = fit - 1;
	size_t head = (keep + 1) / 2, tail = keep - head;
	memcpy(out, s, head);
	out[head] = GLYPH_ELLIPSIS[0];
	memcpy(out + head + 1, s + n - tail, tail);
	out[fit] = 0;
#ifdef GFX_FIT_TRACE
	gfx_fit_trace(s, out);
#endif
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

static void send(int y0, int y1)
{
#if GFX_INDEXED
	if (bpp == 4) {
		display->flush_indexed(fb_words, pal_sent, (uint16_t)y0, (uint16_t)y1);
		return;
	}
#endif
	display->flush(fb_words, (uint16_t)y0, (uint16_t)y1);
}

void gfx_flush(void)
{
	if (display == NULL)
		return;
#if GFX_INDEXED
	if (bpp == 4) {		// an entry that changed color changes rows the hashes can't see
		for (int i = 0; i < pal_n_sent; i++)
			if (pal[i] != pal_sent[i])
				all_dirty = true;
		memcpy(pal_sent, pal, sizeof pal);
		pal_n_sent = pal_n;
	}
#endif
	int band = -1;
	for (int y = 0; y < height; y++) {
		uint32_t h = hash_row(y);
		bool changed = all_dirty || h != row_hash[y];
		row_hash[y] = h;
#if CONFIG_NET_WEBCTL
		if (changed)
			row_new[y >> 3] |= (uint8_t)(1u << (y & 7));
#endif
		if (changed && band < 0)
			band = y;
		if (!changed && band >= 0) {
			send(band, y);
			band = -1;
		}
	}
	if (band >= 0)
		send(band, height);
	all_dirty = false;
#if CONFIG_NET_WEBCTL
	frames_done = frames_begun;
#endif
}

#if CONFIG_NET_WEBCTL
void gfx_frame_begin(void)
{
	frames_begun++;
}

bool gfx_frame_idle(uint32_t *frames)
{
	uint32_t b = frames_begun;
	*frames = b;
	return b == frames_done;
}

int gfx_row_next(int from)
{
	for (int y = from < 0 ? 0 : from; y < height; y++)
		if (row_new[y >> 3] & (1u << (y & 7)))
			return y;
	return -1;
}

void gfx_rows_take(int from, int to)
{
	for (int y = from; y <= to && y < height; y++)
		row_new[y >> 3] &= (uint8_t)~(1u << (y & 7));
}

void gfx_rows_mark_all(void)
{
	memset(row_new, 0xFF, sizeof row_new);
}
#endif

void gfx_invalidate(void)
{
	all_dirty = true;
}

static volatile uint32_t hold_until;

void gfx_hold(uint32_t until_ms)
{
	hold_until = until_ms;
}

bool gfx_held(uint32_t now_ms)
{
	return hold_until != 0 && (int32_t)(hold_until - now_ms) > 0;
}
