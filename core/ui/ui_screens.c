// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Home, quick settings and the boot splash, laid out from the display size (docs/ux/information-architecture.md 3, 5;
// docs/ux/wireframes). Regions top to bottom: status strip, place, talk target, activity, hint row.
#include <string.h>
#include "board.h"
#include "ui_screens.h"
#include "fmt.h"
#include "ui_svc.h"
#include "config.h"

ui_layout_t ui_layout;

// Colors carry meaning only (P5); every color cue also has a word. On 1 bpp panels everything is ink on paper
// and pills are inverse video
ui_colors_t ui_col;
static uint8_t ui_theme;

void ui_theme_set(uint8_t theme)
{
	ui_theme = theme;
	if (gfx_bpp() == 1)
		return;		// ink on paper whatever the theme
	if (theme == 1) {
		// Light: dark text on white; the meaning colors darker so they read on white
		ui_col.bg = RGB565(255, 255, 255);
		ui_col.fg = RGB565(0, 0, 0);
		ui_col.dim = RGB565(88, 88, 88);
		ui_col.line = RGB565(176, 176, 176);
		ui_col.tx = RGB565(200, 0, 0);
		ui_col.rx = RGB565(0, 136, 40);
		ui_col.warn = RGB565(176, 96, 0);
		ui_col.idle = RGB565(128, 128, 128);
		ui_col.focus = RGB565(64, 128, 232);
		ui_col.hint_bg = RGB565(224, 224, 224);
		ui_col.priv = RGB565(0, 72, 200);
	} else if (theme == 2) {
		// High contrast: pure black and white, saturated meaning colors, nothing gray
		ui_col.bg = RGB565(0, 0, 0);
		ui_col.fg = ui_col.dim = RGB565(255, 255, 255);
		ui_col.line = RGB565(255, 255, 255);
		ui_col.tx = RGB565(255, 0, 0);
		ui_col.rx = RGB565(0, 255, 0);
		ui_col.warn = RGB565(255, 255, 0);
		ui_col.idle = RGB565(255, 255, 255);
		ui_col.focus = RGB565(0, 0, 255);
		ui_col.hint_bg = RGB565(0, 0, 0);
		ui_col.priv = RGB565(0, 255, 255);
	} else {
		ui_col.bg = RGB565(0, 0, 0);
		ui_col.fg = RGB565(255, 255, 255);
		ui_col.dim = RGB565(168, 168, 168);
		ui_col.line = RGB565(72, 72, 72);
		ui_col.tx = RGB565(224, 32, 32);
		ui_col.rx = RGB565(40, 184, 72);
		ui_col.warn = RGB565(240, 168, 0);
		ui_col.idle = RGB565(96, 96, 96);
		ui_col.focus = RGB565(32, 96, 200);
		ui_col.hint_bg = RGB565(32, 32, 32);
		ui_col.priv = RGB565(64, 140, 255);
	}
	ui_col.pill_fg = RGB565(255, 255, 255);
	ui_col.pill_dark_fg = RGB565(0, 0, 0);
}

void ui_layout_init(void)
{
	int w = gfx_width(), h = gfx_height();
	ui_layout_t *L = &ui_layout;

	if (gfx_bpp() == 1) {
		ui_col.bg = 0; ui_col.fg = ui_col.dim = ui_col.line = ui_col.tx = ui_col.rx = ui_col.warn = ui_col.idle = ui_col.focus = ui_col.priv = 1;
		ui_col.pill_fg = ui_col.pill_dark_fg = 0;
		ui_col.hint_bg = 0;
	}

	ui_theme_set(ui_theme);

	// Display classes (P10): about 128 px wide, about 160 px wide, 240 px and wider. A narrow portrait panel (the
	// DM-32's 120x160) keeps the narrow class's small font, so lists and labels keep their 24 characters a line, and
	// has the rows for the larger channel and talk target fonts of the 160 px class
	if (w >= 220) {
		L->tiny = &font_6x12; L->small = &font_8x16; L->medium = &font_12x24; L->large = FONT_LARGEST;
	} else if (w >= 150) {
		L->tiny = &font_5x8; L->small = &font_6x12; L->medium = &font_8x16; L->large = &font_12x24;
	} else if (h >= 150) {
		L->tiny = &font_5x8; L->small = &font_5x8; L->medium = &font_8x16; L->large = &font_12x24;
	} else {
		L->tiny = &font_5x8; L->small = &font_5x8; L->medium = &font_6x12; L->large = &font_8x16;
	}

	// A pixel between the status strip and what is below it, and a pixel between list rows on displays 100 px and
	// higher (3 on tall ones): easy to read comes before one more row
	bool tall = h >= UI_TALL_PX;
	L->strip_h = L->small->h + 1;
	L->hint_y = h - L->small->h;
	int y = L->strip_h;
	// Rows that are dropped when the display is short: the zone line, then the separator
	int need = L->small->h + L->large->h + L->medium->h + L->small->h + 2 + L->small->h;
	bool zone = L->strip_h + need + L->small->h <= h;
	L->zone_y = zone ? y : -1;
	if (zone)
		y += L->small->h;
	L->channel_y = y;
	y += L->large->h;
	L->talk_y = y;
	y += L->medium->h;
	L->chips_y = y;
	y += L->small->h;
	if (L->hint_y - y >= L->small->h + 2) {
		L->sep_y = y;
		y += 2;
	} else {
		L->sep_y = -1;
	}
	L->activity_y = y;
	L->activity_h = L->hint_y - y;
	L->row_h = L->small->h + (tall ? 3 : h >= 100 ? 1 : 0);
}

// --- shared pieces ---

const char *ui_power_name(uint8_t p)
{
	if (board.power_names != NULL && p < board.power_count)
		return board.power_names[p];
	return "?";
}

static uint8_t channel_power(const radio_view_t *v)
{
	return v->ch.power == CODEPLUG_POWER_DEFAULT ? board.power_default : v->ch.power;
}

void ui_bands_text(char *out, size_t len)
{
	sbuf_t b;
	sb_init(&b, out, len);
	for (int i = 0; i < board.band_count; i++) {
		if (i)
			sb_chr(&b, ' ');
		sb_u32(&b, board.bands[i].lo_hz / 1000000u, 1);
		sb_chr(&b, '-');
		sb_u32(&b, board.bands[i].hi_hz / 1000000u, 1);
	}
}

static uint8_t rssi_bars(int16_t dbm)
{
	if (dbm >= -88) return 4;
	if (dbm >= -98) return 3;
	if (dbm >= -108) return 2;
	if (dbm >= -118) return 1;
	return 0;
}

// Status strip (P2): state pill, mode, monitor, lock, unread messages, signal, battery.
// A titled screen (prefix and / or name) on a display under UI_TALL_PX high merges its title into the strip
// (wireframes 160x128 and 128x64): the pill shows only while not idle, the signal bars and the battery icon give way
// to the title, the battery % stays. The prefix ("Quick") is kept whole; only the name is shortened, from the middle.
// On tall displays the strip stays whole and the title gets its own row below it. Returns the y where content starts.
uint32_t ui_strip_draws;

int ui_draw_top(const radio_view_t *v, const ui_state_t *s, const char *prefix, const char *name)
{
	ui_strip_draws++;
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	int ph = f->h;		// the pill: one text cell high
	bool titled = prefix != NULL || name != NULL;
	bool merged = titled && gfx_height() < UI_TALL_PX;
	const char *word = "IDLE";
	color_t pbg = ui_col.idle, pfg = ui_col.pill_fg;
	switch (v->state) {
	case RS_RX: word = "RX"; pbg = ui_col.rx; pfg = ui_col.pill_dark_fg; break;
	case RS_TX: word = "TX"; pbg = ui_col.tx; break;
	case RS_TX_DENIED: word = "NO TX"; pbg = ui_col.warn; pfg = ui_col.pill_dark_fg; break;
	default: break;
	}
	if (gfx_bpp() == 1)
		pfg = ui_col.pill_fg;
	gfx_fill(0, 0, w, L->strip_h, ui_col.bg);
	int x = 0;
	if (!merged || v->state != RS_IDLE)
		x = gfx_pill(f, 0, 0, ph, word, pfg, pbg) + f->w / 2;
	int ty = (ph - f->h + 1) / 2;

	// Right side, right to left: battery %, battery icon, signal bars, unread messages, lock
	char pct[6];
	sbuf_t b;
	sb_init(&b, pct, sizeof pct);
	sb_u32(&b, s->battery_pct, 1);
	sb_chr(&b, '%');
	int rx = w - gfx_text_width(f, pct);
	uint8_t low = s->low_batt_pct ? s->low_batt_pct : 15;	// Settings > Power > Low battery warning
	color_t bc = s->battery_pct <= low ? ui_col.warn : ui_col.fg;
	gfx_text(f, rx, ty, pct, bc);
	if (!merged) {
		int bh = f->ascent - 2 < 5 ? 5 : f->ascent - 2;
		int bw = bh * 2;
		rx -= bw + 2;
		gfx_icon_battery(rx, ty + f->ascent - bh, bw, bh, s->battery_pct, ui_col.fg, bc);
		int sw = bh + 4;
		rx -= sw + f->w / 2;
		gfx_icon_bars(rx, ty + f->ascent - bh - 1, sw, bh + 1, 4, rssi_bars(v->rssi_dbm), ui_col.fg, ui_col.line);
	}
	// Icons one cell high, drawn from lines: an envelope with the unread count, a padlock while the keys are locked
	int ih = f->ascent - 2 < 5 ? 5 : f->ascent - 2, iy = ty + f->ascent - ih;
	if (s->unread) {
		char m[4];
		sb_init(&b, m, sizeof m);
		sb_u32(&b, s->unread > 9 ? 9 : s->unread, 1);
		rx -= gfx_text_width(f, m) + f->w / 2;
		gfx_text(f, rx, ty, m, ui_col.warn);
		int ew = ih * 3 / 2;
		rx -= ew + 1;
		gfx_rect(rx, iy, ew, ih, ui_col.warn);
		for (int i = 1; i <= ew / 2; i++) {	// the flap: two diagonals meeting in the middle
			gfx_pixel(rx + i, iy + i * (ih - 2) / (ew / 2), ui_col.warn);
			gfx_pixel(rx + ew - 1 - i, iy + i * (ih - 2) / (ew / 2), ui_col.warn);
		}
	}
	if (s->locked) {
		int lw = ih - ih / 4;
		rx -= lw + f->w / 2;
		gfx_fill(rx, iy + ih / 2, lw, ih - ih / 2, ui_col.warn);		// body
		gfx_rect(rx + 1, iy, lw - 2, ih / 2 + 1, ui_col.warn);		// shackle
	}

	if (merged) {
		ui_draw_title(f, x, ty, rx - f->w / 2 - x, prefix, name, ui_col.fg);
		return L->strip_h;
	}
	// Left side: the mode, and one mode word when there is room for it with a space either side
	int lx = gfx_text(f, x, ty, v->ch.mode == RADIO_MODE_DMR ? "DMR" : "FM", ui_col.fg);
	if (s->scanning && lx + 6 * f->w <= rx)
		lx = gfx_text(f, lx + f->w, ty, "SCAN", ui_col.warn);
#if CONFIG_DMR_ROAMING
	else if (v->roam.state == RROAM_STATE_SEARCH && lx + 6 * f->w <= rx)
		lx = gfx_text(f, lx + f->w, ty, "ROAM", ui_col.warn);	// searching the zone's sites
#endif
	else if (v->monitor && lx + 5 * f->w <= rx)
		lx = gfx_text(f, lx + f->w, ty, "MON", ui_col.warn);
	if (!titled)
		return L->strip_h;
	// Tall display: the title on its own row, underlined
	int y = L->strip_h;
	gfx_fill(0, y, w, f->h + 2, ui_col.bg);
	ui_draw_title(f, 0, y + 1, w, prefix, name, ui_col.fg);
	gfx_hline(0, y + f->h + 1, w, ui_col.line);
	return y + f->h + 2;
}

void ui_draw_title(const font_t *f, int x, int y, int max_w, const char *prefix, const char *name, color_t c)
{
	char fit[48];
	if (prefix != NULL) {
		gfx_text_fit(f, prefix, max_w, fit, sizeof fit);
		int nx = gfx_text(f, x, y, fit, name != NULL ? ui_col.dim : c);
		max_w -= nx - x;
		x = nx;
		if (name == NULL || max_w < 3 * f->w)
			return;
		x += f->w;
		max_w -= f->w;
	}
	if (name != NULL) {
		gfx_text_fit(f, name, max_w, fit, sizeof fit);
		gfx_text(f, x, y, fit, c);
	}
}

static const char *wrap_line(const font_t *f, const char *t, int max_w, char *line, size_t cap);
static int wrap_lines(const font_t *f, const char *t, int max_w, int max, char lines[][64], bool preview);

// An overlay's top edge (banner, two-line toast) moved up to a gap between lines of what is under it, so it never
// cuts a line of text in half: the first row at or above y whose row above is all one color (background, or the
// edge of a focus bar), at most one line up
static int overlay_top(int y)
{
	int w = gfx_width();
	for (int t = y; t > 0 && t > y - ui_layout.large->h - 2; t--) {
		color_t c = gfx_get_pixel(0, t - 1);
		int x = 1;
		while (x < w && gfx_get_pixel(x, t - 1) == c)
			x++;
		if (x == w)
			return t;
	}
	return y;
}

// Split a hint into the Back group (it starts with "Back") and the rest, joined by two spaces
static void hint_split(const char *text, char *left, size_t lcap, char *right, size_t rcap)
{
	size_t ln = 0, rn = 0;
	right[0] = 0;
	for (const char *g = text; *g;) {
		const char *end = strstr(g, "  ");
		size_t n = end != NULL ? (size_t)(end - g) : strlen(g);
		if (rn == 0 && n >= 4 && strncmp(g, "Back", 4) == 0 && n < rcap) {
			memcpy(right, g, n);
			right[n] = 0;
			rn = n;
		} else if (ln + n + 3 < lcap) {
			if (ln) {
				left[ln++] = ' ';
				left[ln++] = ' ';
			}
			memcpy(left + ln, g, n);
			ln += n;
		}
		g += n;
		while (*g == ' ')
			g++;
	}
	left[ln] = 0;
}

// The hint row. Hints are groups separated by two spaces ("OK open  # more  Back"); the group for the Back key
// ("Back", "Back keeps", ...) is pinned to the right edge, under the key, and the others run from the left. A hint may
// hold text alternates (font.h): the longest form that fits is used. A toast is centered; one too long for the row
// takes a second line above it, so it is never shortened
void ui_draw_hint(const char *text, bool toast)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	char fit[64];
	color_t bg = toast ? ui_col.fg : ui_col.hint_bg;
	color_t fg = toast ? ui_col.bg : ui_col.dim;
	if (gfx_bpp() == 1 && !toast)
		fg = ui_col.fg;
	if (toast) {
		char first[64], second[64];
		bool more = true;
		// One line in the longest alternate that fits it, else two lines of the first
		for (int level = 0; more; level++) {
			gfx_text_alt(text, level, first, sizeof first, &more);
			if (gfx_text_width(f, first) <= w - 2)
				break;
		}
		if (gfx_text_width(f, first) > w - 2)
			gfx_text_alt(text, 0, first, sizeof first, &more);
		const char *rest = wrap_line(f, first, w - 2, fit, sizeof fit);
		int y = L->hint_y, top = y;
		second[0] = 0;
		if (*rest) {
			// Two lines: the first ends at a word, the rest goes below it
			size_t n = strlen(rest);
			n = n < sizeof second - 1 ? n : sizeof second - 1;
			memcpy(second, rest, n);
			second[n] = 0;
			y -= f->h;
			top = overlay_top(y);
		}
		gfx_fill(0, top, w, gfx_height() - top, bg);
		gfx_text(f, (w - gfx_text_width(f, fit)) / 2, y, fit, fg);
		if (second[0]) {
			gfx_text_fit(f, second, w - 2, fit, sizeof fit);
			gfx_text(f, (w - gfx_text_width(f, fit)) / 2, y + f->h, fit, fg);
		}
		return;
	}
	gfx_fill(0, L->hint_y, w, gfx_height() - L->hint_y, bg);
	if (gfx_bpp() == 1)
		gfx_hline(0, L->hint_y, w, ui_col.line);
	// The longest alternate whose groups fit side by side (or the shortest there is)
	char form[64], left[64], right[24];
	bool more = true;
	int lw = 0, rw = 0;
	for (int level = 0; more; level++) {
		gfx_text_alt(text, level, form, sizeof form, &more);
		hint_split(form, left, sizeof left, right, sizeof right);
		lw = gfx_text_width(f, left);
		rw = gfx_text_width(f, right);
		if (lw + (rw && lw ? f->w : 0) + rw <= w)
			break;
	}
	// Still too wide: the last groups on the left go, whole, before anything is shortened
	while (lw + (rw && lw ? f->w : 0) + rw > w) {
		char *cut = NULL;
		for (char *p = strstr(left, "  "); p != NULL; p = strstr(p + 1, "  "))
			cut = p;
		if (cut == NULL)
			break;
		*cut = 0;
		lw = gfx_text_width(f, left);
	}
	int margin = lw + (rw && lw ? f->w : 0) + rw <= w - 2 ? 1 : 0;
	if (rw > w - 2 * margin)
		rw = w - 2 * margin;
	if (right[0]) {
		gfx_text_fit(f, right, rw, fit, sizeof fit);
		gfx_text(f, w - margin - gfx_text_width(f, fit), L->hint_y, fit, fg);
	}
	if (left[0]) {
		gfx_text_fit(f, left, w - 2 * margin - (right[0] ? rw + f->w : 0), fit, sizeof fit);
		gfx_text(f, margin, L->hint_y, fit, fg);
	}
}

// --- boot splash ---

// The mark: a mast with a station dot on top and three arcs of signal either side, drawn to the size asked for (r =
// outer arc radius; the mark is 2r wide and about 1.4r tall, the dot at cx, cy). Plain pixels, so it scales to every
// display class and stays crisp on a 1 bpp panel
static void draw_mark(int cx, int cy, int r)
{
	static const color_t arc_rgb[3] = { RGB565(110, 170, 255), RGB565(64, 140, 255), RGB565(32, 96, 200) };
	int t = r >= 28 ? 3 : r >= 14 ? 2 : 1;		// arc thickness
	int dot = r / 7 > 1 ? r / 7 : 1;
	for (int i = 0; i < 3; i++) {
		int ro = r * (i + 2) / 4, ri = ro - t;	// radii r/2, 3r/4, r
		color_t c = gfx_bpp() == 1 ? ui_col.fg : arc_rgb[i];
		for (int dy = -ro; dy <= ro; dy++) {
			for (int dx = -ro; dx <= ro; dx++) {
				int d2 = dx * dx + dy * dy, ady = dy < 0 ? -dy : dy, adx = dx < 0 ? -dx : dx;
				// in the ring, within 40 degrees of horizontal (tan 40 = 0.84)
				if (d2 <= ro * ro && d2 > ri * ri && ady * 100 <= adx * 84)
					gfx_pixel(cx + dx, cy + dy, c);
			}
		}
	}
	// The mast: a narrow triangle from under the dot to the base, then the dot
	int top = cy + dot + 1, base = cy + r * 3 / 4, half = r / 5 > 1 ? r / 5 : 1;
	for (int y = top; y <= base; y++) {
		int hw = half * (y - top) / (base - top > 0 ? base - top : 1);
		gfx_hline(cx - hw, y, 2 * hw + 1, gfx_bpp() == 1 ? ui_col.fg : ui_col.dim);
	}
	for (int dy = -dot; dy <= dot; dy++)
		for (int dx = -dot; dx <= dot; dx++)
			if (dx * dx + dy * dy <= dot * dot + dot / 2)
				gfx_pixel(cx + dx, cy + dy, ui_col.fg);
}

// The wordmark: "Open" in the accent color, "DigitalRadio" in the text color, centered; returns its height
static int draw_wordmark(const font_t *f, int y)
{
	int w1 = gfx_text_width(f, "Open"), w = w1 + gfx_text_width(f, "DigitalRadio");
	int x = (gfx_width() - w) / 2;
	gfx_text(f, x, y, "Open", gfx_bpp() == 1 ? ui_col.fg : ui_col.priv);
	gfx_text(f, x + w1, y, "DigitalRadio", ui_col.fg);
	return f->h;
}

void ui_draw_splash(const char *board_name, const char *git, const char *line1, const char *line2)
{
	int w = gfx_width(), h = gfx_height();
	const ui_layout_t *L = &ui_layout;
	gfx_clear(ui_col.bg);

	const char *word = "OpenDigitalRadio";
	const font_t *wf = gfx_text_width(L->large, word) <= w - 4 ? L->large :
		gfx_text_width(L->medium, word) <= w - 4 ? L->medium : L->small;
	// The owner's lines (the CPS boot text): as large as fits, never dropped
	const char *lines[2] = { line1 != NULL ? line1 : "", line2 != NULL ? line2 : "" };
	int n_lines = (lines[0][0] != '\0') + (lines[1][0] != '\0');
	const font_t *lf = L->medium;
	for (int i = 0; i < 2; i++)
		if (gfx_text_width(lf, lines[i]) > w - 2)
			lf = L->small;

	// Footer: board and version, small and dim along the bottom
	char foot[48];
	sbuf_t b;
	sb_init(&b, foot, sizeof foot);
	sb_str(&b, board_name);
	sb_str(&b, "  ");
	sb_str(&b, git);
	const font_t *ff = L->tiny;
	int foot_y = h - ff->h;

	// Stack: mark, wordmark, rule, lines; centered in the space above the footer. The mark takes what is left
	int gap = h >= 200 ? 8 : h >= 100 ? 4 : 2;
	int text_h = wf->h + gap + (n_lines > 0 ? gap + n_lines * lf->h : 0);
	int avail = foot_y - gap - text_h - gap;
	int r = avail * 10 / 14;			// the mark is about 1.4 r tall
	if (r > w / 4)
		r = w / 4;
	if (r > h / 4)
		r = h / 4;
	bool mark = r >= 12;		// smaller than this the arcs merge into noise
	// Too short for the mark above the name (128x64): the mark left of the name instead, at the smallest size that
	// stays clear, with the name in the largest font that leaves it room. The owner's lines keep their size
	int side_r = 0;
	if (!mark) {
		int sr = 12, sw = 2 * sr + 4;
		const font_t *sf = gfx_text_width(L->medium, word) <= w - 4 - sw ? L->medium :
			gfx_text_width(L->small, word) <= w - 4 - sw ? L->small : NULL;
		int row_h = sr * 14 / 10 > (sf != NULL ? sf->h : 0) ? sr * 14 / 10 : (sf != NULL ? sf->h : 0);
		if (sf != NULL && text_h - wf->h + row_h <= foot_y - gap) {
			side_r = sr;
			wf = sf;
			text_h += row_h - wf->h;
		}
	}
	int mark_h = mark ? r * 14 / 10 : 0;
	int total = mark_h + (mark ? gap : 0) + text_h;
	int y = (foot_y - total) / 2;
	if (y < 0)
		y = 0;
	if (mark) {
		draw_mark(w / 2, y + r * 64 / 100 + 1, r);	// the arcs reach 0.64 r above the dot
		y += mark_h + gap;
	}
	if (side_r > 0) {
		int sw = 2 * side_r + 4, ww = gfx_text_width(wf, word);
		int x0 = (w - sw - ww) / 2, row_h = side_r * 14 / 10 > wf->h ? side_r * 14 / 10 : wf->h;
		draw_mark(x0 + side_r, y + (row_h - side_r * 14 / 10) / 2 + side_r * 64 / 100 + 1, side_r);
		gfx_text(wf, x0 + sw, y + (row_h - wf->h) / 2, "Open", gfx_bpp() == 1 ? ui_col.fg : ui_col.priv);
		gfx_text(wf, x0 + sw + gfx_text_width(wf, "Open"), y + (row_h - wf->h) / 2, "DigitalRadio", ui_col.fg);
		y += row_h;
	} else
		y += draw_wordmark(wf, y);
	gfx_hline(w / 2 - w / 6, y + gap / 2, w / 3, gfx_bpp() == 1 ? ui_col.fg : ui_col.focus);
	y += gap;
	if (n_lines > 0) {
		y += gap;
		for (int i = 0; i < 2; i++) {
			if (lines[i][0] == '\0')
				continue;
			gfx_text(lf, (w - gfx_text_width(lf, lines[i])) / 2, y, lines[i], ui_col.fg);
			y += lf->h;
		}
	}
	gfx_text(ff, (w - gfx_text_width(ff, foot)) / 2, foot_y, foot, ui_col.dim);
}

// --- Home ---

#if CONFIG_DMR
static void draw_talk_dmr(const radio_view_t *v);
static void draw_chips_dmr(const radio_view_t *v, const ui_state_t *s);
#endif

static void draw_place(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	char buf[40], fit[40];
	sbuf_t b;

	if (L->zone_y >= 0) {
		// Zone (or VFO and its step) left, position in the zone right
		char right[16];
		sb_init(&b, right, sizeof right);
		if (v->vfo) {
			sb_str(&b, "Step ");
			uint32_t st = v->vfo_step_hz;
			sb_u32(&b, st / 1000u, 1);
			if (st % 1000u) {
				sb_chr(&b, '.');
				sb_u32(&b, (st % 1000u) / 10u, 2);
				// trim a trailing zero: 12.50 -> 12.5
				if (b.p[-1] == '0') { b.p[-1] = 0; b.p--; }
			}
			sb_chr(&b, 'k');
		} else if (v->zone_size) {
			sb_u32(&b, v->zone_pos + 1u, 1);
			sb_chr(&b, '/');
			sb_u32(&b, v->zone_size, 1);
		}
		int rw = gfx_text_width(L->small, right);
		gfx_text(L->small, w - rw, L->zone_y, right, ui_col.dim);
		const char *left = v->vfo ? "VFO" : v->zone_name;
		gfx_text_fit(L->small, left, w - rw - L->small->w, fit, sizeof fit);
		gfx_text(L->small, 0, L->zone_y, fit, v->vfo ? ui_col.warn : ui_col.fg);
	}

	int y = L->channel_y;
	int base = y + L->large->ascent;
	if (v->vfo || s->entry) {
		// The frequency large; while typing, the digits so far and blanks for the rest
		sb_init(&b, buf, sizeof buf);
		if (s->entry) {
			int dec = s->entry_len > UI_ENTRY_MHZ ? s->entry_len - UI_ENTRY_MHZ : 0;
			int show = dec < 3 ? 3 : dec;
			for (int i = 0; i < UI_ENTRY_MHZ + show; i++) {
				if (i == UI_ENTRY_MHZ)
					sb_chr(&b, '.');
				sb_chr(&b, i < s->entry_len ? s->entry_digits[i] : '_');
			}
		} else {
			sb_freq(&b, v->ch.rx_hz);
		}
		int x = gfx_text(L->large, 0, y, buf, s->entry ? ui_col.warn : ui_col.fg);
		if (x + L->small->w / 2 + gfx_text_width(L->small, "MHz") <= w)	// 5 decimals leave no room on 120 px
			gfx_text(L->small, x + L->small->w / 2, base - L->small->ascent, "MHz", ui_col.dim);
		if (L->zone_y < 0 && !s->entry)
			gfx_text(L->small, w - 3 * L->small->w, base - L->small->ascent, "VFO", ui_col.dim);
		return;
	}

	// Channel number (medium, dim) and name, baselines aligned. The name uses the largest font it fits in; only a name
	// too long even for the small font is shortened from the middle
	sb_init(&b, buf, sizeof buf);
	sb_u32(&b, v->channel_number, 1);
	int x = gfx_text(L->medium, 0, base - L->medium->ascent, buf, ui_col.dim) + L->medium->w / 2;
	int avail = w - x;
	if (v->temporary)
		avail -= L->medium->w;
	const font_t *nf = L->large;
	if (gfx_text_width(nf, v->ch.name) > avail)
		nf = L->medium;
	if (gfx_text_width(nf, v->ch.name) > avail)
		nf = L->small;
	gfx_text_fit(nf, v->ch.name, avail, fit, sizeof fit);
	x = gfx_text(nf, x, nf == L->large ? y : base - nf->ascent, fit, ui_col.fg);
	if (v->temporary)
		gfx_text(L->medium, x, base - L->medium->ascent, GLYPH_DOT, ui_col.warn);
}

static void draw_talk(const radio_view_t *v)
{
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR) {
		draw_talk_dmr(v);
		return;
	}
#endif
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->medium;
	char buf[40];
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.fg;
	if (v->ch.mode == RADIO_MODE_DMR) {
		sb_str(&b, "DMR: not in this build");
		c = ui_col.warn;
	} else if (v->ch.rx_only) {
		sb_str(&b, "RX only");
		c = ui_col.warn;
	} else if (v->ch.tx_hz == 0) {
		sb_str(&b, "No TX frequency");
		c = ui_col.warn;
	} else {
		// " MHz" goes before the frequency is cut (font.h alternates)
		sb_str(&b, TEXT_ALT_OPEN);
		sb_freq(&b, v->ch.tx_hz);
		sb_str(&b, " MHz" TEXT_ALT_NEXT);
		sb_freq(&b, v->ch.tx_hz);
		sb_str(&b, TEXT_ALT_CLOSE);
	}
	// Too wide: shorter text, never a smaller font
	int x = gfx_text(f, 0, L->talk_y, "PTT" GLYPH_ARROW " ", ui_col.dim);
	char fit[40];
	gfx_text_fit(f, buf, gfx_width() - x, fit, sizeof fit);
	gfx_text(f, x, L->talk_y, fit, c);
}

// Chips: tone, bandwidth, power, squelch. A dot marks a temporary change
static void draw_chips(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	char buf[48];
	sbuf_t b;
	int x = 0, y = L->chips_y;

#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR) {
		draw_chips_dmr(v, s);
		return;
	}
#endif
	sb_init(&b, buf, sizeof buf);
	bool tone_temp = (v->temp & (RADIO_TEMP_TX_TONE | RADIO_TEMP_RX_TONE)) != 0;
	if (tone_temp)
		x = gfx_text(f, x, y, GLYPH_DOT, ui_col.warn);
	if (v->ch.tx_tone.type == TONE_NONE && v->ch.rx_tone.type == TONE_NONE) {
		sb_str(&b, "No tone");
	} else {
		if (v->ch.tx_tone.type != TONE_NONE) {
			if (v->ch.tx_tone.type == TONE_CTCSS)
				sb_chr(&b, 'T');
			sb_tone(&b, &v->ch.tx_tone);
		}
		bool differ = v->ch.rx_tone.type != v->ch.tx_tone.type || v->ch.rx_tone.value != v->ch.tx_tone.value;
		if (v->ch.rx_tone.type != TONE_NONE && differ) {
			if (v->ch.tx_tone.type != TONE_NONE)
				sb_chr(&b, ' ');
			sb_chr(&b, 'R');
			sb_tone(&b, &v->ch.rx_tone);
		}
	}
	x = gfx_text(f, x, y, buf, tone_temp ? ui_col.warn : ui_col.fg) + f->w;
	bool bw_temp = (v->temp & RADIO_TEMP_BW) != 0;
	if (bw_temp)
		x = gfx_text(f, x, y, GLYPH_DOT, ui_col.warn);
	x = gfx_text(f, x, y, v->ch.bw == RADIO_BW_25K ? "25k" : "12.5k", bw_temp ? ui_col.warn : ui_col.fg) + f->w;
	bool pw_temp = v->power != channel_power(v);
	if (pw_temp)
		x = gfx_text(f, x, y, GLYPH_DOT, ui_col.warn);
	x = gfx_text(f, x, y, ui_power_name(v->power), pw_temp ? ui_col.warn : ui_col.fg) + f->w;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "SQ");
	sb_u32(&b, v->squelch, 1);
	if (x + gfx_text_width(f, buf) <= w)
		gfx_text(f, x, y, buf, ui_col.fg);
}

// RX signal meter: segments for -120 .. -60 dBm
static void draw_meter(int x, int y, int w, int h, int16_t dbm)
{
	int seg = 10;
	int pitch = w / seg;
	int lit = (dbm + 120) * seg / 60;
	if (lit < 0) lit = 0;
	if (lit > seg) lit = seg;
	if (dbm > -120 && lit == 0)
		lit = 1;
	for (int i = 0; i < seg; i++) {
		if (i < lit)
			gfx_fill(x + i * pitch, y, pitch - 1, h, ui_col.rx);
		else if (gfx_bpp() == 1)
			gfx_rect(x + i * pitch, y, pitch - 1, h, ui_col.line);
		else
			gfx_fill(x + i * pitch, y, pitch - 1, h, ui_col.line);
	}
}

static const char *deny_reason(const radio_view_t *v, char *buf, size_t len)
{
	sbuf_t b;
	sb_init(&b, buf, len);
	switch (v->tx_deny) {
	case TXDENY_RX_ONLY: sb_str(&b, "This channel is RX only"); break;
	case TXDENY_BAND: {
		char bands[32];
		ui_bands_text(bands, sizeof bands);
		sb_str(&b, "Bands ");
		sb_str(&b, bands);
		break;
	}
	case TXDENY_TIMEOUT: sb_str(&b, "TX time-out reached"); break;
	case TXDENY_NO_FREQ: sb_str(&b, "No TX frequency set"); break;
	case TXDENY_MODE: sb_str(&b, "Mode: not in this build"); break;
	case TXDENY_NO_CODEC: sb_str(&b, "No codec: voice off"); break;
	case TXDENY_NO_DMR: sb_str(&b, "DMR modem not running"); break;
	case TXDENY_BUSY: sb_str(&b, "Channel busy"); break;
	case TXDENY_NO_REPEATER: sb_str(&b, "No repeater answered"); break;
	case TXDENY_REJECTED: sb_str(&b, "Modem refused to send"); break;
	case TXDENY_HOTSPOT: sb_str(&b, "Hotspot mode: PTT off"); break;
	case TXDENY_NO_ID: sb_str(&b, "No DMR ID in codeplug"); break;
	default: sb_str(&b, "Not allowed here"); break;
	}
	return buf;
}

static const char *deny_action(const radio_view_t *v)
{
	switch (v->tx_deny) {
	case TXDENY_BAND: return v->vfo ? "Tune into a band, then PTT" : "Pick another channel";
	case TXDENY_TIMEOUT: return "Release PTT to talk again";
	case TXDENY_NO_CODEC: return "Add codec with odrflash";
	case TXDENY_BUSY: return "Wait for the call to end";
	case TXDENY_NO_REPEATER: return "Check CC and range";
	case TXDENY_REJECTED:
	case TXDENY_NO_DMR: return "Release PTT, try again";
	case TXDENY_HOTSPOT: return "OK stop hotspot";
	case TXDENY_NO_ID: return "Set your DMR ID in qdmr";
	default: return "Pick another channel";
	}
}

// --- DMR (docs/ux/wireframes: Home DMR, Receiving, Transmitting) ---
#if CONFIG_DMR
// "TG 201 Main Ops", "Joe 3101234", "All call"; a private call without a name shows the ID only
static void sb_target(sbuf_t *b, uint32_t id, bool private_call, const char *name)
{
	if (id == 0xFFFFFFu) {
		sb_str(b, "All call");
		return;
	}
	if (private_call) {
		if (name[0] != '\0') {
			sb_str(b, name);
			sb_chr(b, ' ');
		}
		sb_u32(b, id, 1);
		return;
	}
	sb_str(b, "TG ");
	sb_u32(b, id, 1);
	if (name[0] != '\0') {
		sb_chr(b, ' ');
		sb_str(b, name);
	}
}

// The target's short form, for when the whole doesn't fit: a talkgroup keeps its number ("TG 201"), a private contact
// its name ("Joe"), since the name is what the user picked and the ID alone says nothing at a glance
static void sb_target_short(sbuf_t *b, uint32_t id, bool private_call, const char *name)
{
	if (private_call && id != 0xFFFFFFu && name[0] != '\0')
		sb_str(b, name);
	else
		sb_target(b, id, private_call, "");
}

// The target and its short form as text alternates (font.h), each followed by tail ("TG 91 World (9s)",
// "TG 91 (9s)"), and when tail isn't empty last the short form without it. Without a name, the one form
static void sb_target_alts(sbuf_t *b, uint32_t id, bool private_call, const char *name, const char *tail)
{
	bool named = name[0] != '\0' && id != 0xFFFFFFu;
	if (named || tail[0])
		sb_str(b, TEXT_ALT_OPEN);
	sb_target(b, id, private_call, name);
	sb_str(b, tail);
	if (named) {
		sb_str(b, TEXT_ALT_NEXT);
		sb_target_short(b, id, private_call, name);
		sb_str(b, tail);
	}
	if (tail[0]) {
		sb_str(b, TEXT_ALT_NEXT);
		sb_target_short(b, id, private_call, name);
	}
	if (named || tail[0])
		sb_str(b, TEXT_ALT_CLOSE);
}

// The caller as the big line: the contact name, else the ID
static void sb_caller(sbuf_t *b, const radio_dmr_call_t *c)
{
	if (c->src_name[0] != '\0')
		sb_str(b, c->src_name);
	else
		sb_u32(b, c->src, 1);
}

// Where the call went: "TG 201 Main Ops", "Private to you", "Private to 3101234"
static void sb_call_dst(sbuf_t *b, const radio_dmr_call_t *c)
{
	if (c->group) {
		sb_target(b, c->dst, false, c->dst_name);
	} else if (c->to_us) {
		sb_str(b, "Private to you");
	} else {
		sb_str(b, "Private to ");
		if (c->dst_name[0] != '\0')
			sb_str(b, c->dst_name);
		else
			sb_u32(b, c->dst, 1);
	}
}

static bool dmr_call_screen(const radio_view_t *v)
{
	if (v->ch.mode != RADIO_MODE_DMR || !v->dmr.active || v->hotspot)
		return false;
	if (v->state == RS_TX)
		return true;
	return v->state == RS_RX && (v->dmr.state == RDMR_RX || v->dmr.state == RDMR_RX_OTHER);
}

// Talk target line on Home: "PTT-> TG 201 Main Ops", or "Reply-> TG 9 (8s)" during the reply window
static bool ui_st_dmr_off;		// ui_state_t.dmr_off of the frame being drawn

static void draw_talk_dmr(const radio_view_t *v)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->medium;
	const radio_dmr_view_t *d = &v->dmr;
	char buf[88], fit[48];
	sbuf_t b;
	color_t c = ui_col.fg;

	const char *prefix = d->reply_active ? "Reply" GLYPH_ARROW " " : "PTT" GLYPH_ARROW " ";
	sb_init(&b, buf, sizeof buf);
	if (v->ch.rx_only) {
		sb_str(&b, "RX only");
		c = ui_col.warn;
	} else if (!d->active && ui_st_dmr_off) {
		sb_str(&b, "DMR modem off");
		c = ui_col.warn;
	} else {
		// The target, and its short form as an alternate: "TG 91 World (9s)", "TG 91 (9s)"; "Joe 3101234", "Joe"
		char secs[8] = "";
		if (d->reply_active) {
			sbuf_t sb;
			sb_init(&sb, secs, sizeof secs);
			sb_str(&sb, " (");
			sb_u32(&sb, d->reply_s, 1);
			sb_str(&sb, "s)");
			c = ui_col.warn;
		}
		// and last without the reply seconds (the hint still says the reply is on)
		sb_target_alts(&b, d->target_id, d->target_private, d->target_name, secs);
	}
	// Too wide: the shorter alternate (a talkgroup drops its name, a contact its ID), never a smaller font
	int w = gfx_width();
	int x = gfx_text(f, 0, L->talk_y, prefix, d->reply_active ? ui_col.warn : ui_col.dim);
	gfx_text_fit(f, buf, w - x, fit, sizeof fit);
	gfx_text(f, x, L->talk_y, fit, c);
}

// Chips: timeslot, color code, repeater / direct, roaming, power, hear all
static void draw_chips_dmr(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	char buf[40];
	sbuf_t b;
	// Timeslot and color code, each with the dot when it is a temporary change
	int x = 0;
	for (int k = 0; k < 2; k++) {
		bool temp = (v->temp & (k ? RADIO_TEMP_CC : RADIO_TEMP_TS)) != 0;
		sb_init(&b, buf, sizeof buf);
		if (temp)
			sb_str(&b, GLYPH_DOT);
		sb_str(&b, k ? "CC" : "TS");
		sb_u32(&b, k ? (v->dmr.active ? v->dmr.cc : v->ch.color_code) : (v->dmr.ts ? v->dmr.ts : v->ch.timeslot), 1);
		x = gfx_text(f, x, L->chips_y, buf, temp ? ui_col.warn : ui_col.fg) + f->w;
	}
	if (v->dmr.rmo)
		x = gfx_text(f, x, L->chips_y, v->dmr.repeater_up ? "RPT" : "Rpt", ui_col.fg) + f->w;
#if CONFIG_DMR_ROAMING
	// Roaming here: Roam, Lock when locked to the site, amber Roam? when the site is weak (dmr-roaming.md 6.2)
	if (v->roam.state != RROAM_STATE_OFF) {
		bool lock = v->roam.state == RROAM_STATE_LOCKED;
		x = gfx_text(f, x, L->chips_y, lock ? "Lock" : v->roam.weak ? "Roam?" : "Roam",
			     v->roam.weak ? ui_col.warn : ui_col.fg) + f->w;
	}
#endif
	bool pw_temp = v->power != channel_power(v);
	if (pw_temp)
		x = gfx_text(f, x, L->chips_y, GLYPH_DOT, ui_col.warn);
	x = gfx_text(f, x, L->chips_y, ui_power_name(v->power), pw_temp ? ui_col.warn : ui_col.fg) + f->w;
	if ((v->dmr.hear_all || s->hear_all) && x + 4 * f->w <= gfx_width())
		x = gfx_text(f, x, L->chips_y, "DMon", ui_col.warn) + f->w;	// digital monitor (FM's is "Mon")
	if ((s->privacy || v->dmr.privacy) && x + 4 * f->w <= gfx_width())
		gfx_text(f, x, L->chips_y, "Priv", ui_col.priv);
}

// Activity on Home, DMR idle: the last call heard. Two lines where they fit (the caller, then where and when), else
// one line where the caller's name gives way first: "Joe Smith" / "TG 91, 11s ago", or "Joe Smith  TG 91 11s ago"
static void draw_last_heard(const radio_view_t *v, const ui_state_t *s, int y)
{
	const font_t *sm = ui_layout.small;
	const radio_dmr_view_t *d = &v->dmr;
	int w = gfx_width();
	char who[24], where[48], fit[48];
	sbuf_t b;
	if (d->last_ms == 0) {
		gfx_text(sm, 0, y + (ui_layout.activity_h - sm->h) / 2, "No calls heard yet", ui_col.dim);	// centered
		return;
	}
	sb_init(&b, who, sizeof who);
	sb_caller(&b, &d->last);
	sb_init(&b, where, sizeof where);
	if (d->last.group) {
		sb_str(&b, "TG ");
		sb_u32(&b, d->last.dst, 1);
	} else {
		sb_str(&b, d->last.to_us ? "To you" : "Private");
	}
	sb_chr(&b, ' ');
	sb_age(&b, s->now_ms - d->last_ms);
	if (ui_layout.activity_h >= 2 * sm->h) {
		gfx_text_fit(sm, who, w, fit, sizeof fit);
		gfx_text(sm, 0, y, fit, ui_col.fg);
		gfx_text_fit(sm, where, w, fit, sizeof fit);
		gfx_text(sm, 0, y + sm->h, fit, ui_col.dim);
		return;
	}
	// One line: where and when on the right as the longest form that leaves the name 6 characters, the name before it
	y += (ui_layout.activity_h - sm->h) / 2;
	gfx_text_fit(sm, where, w - 7 * sm->w, fit, sizeof fit);
	int ww = gfx_text_width(sm, fit);
	gfx_text(sm, w - ww, y, fit, ui_col.dim);
	gfx_text_fit(sm, who, w - ww - sm->w, fit, sizeof fit);
	gfx_text(sm, 0, y, fit, ui_col.fg);
}

// One text line of the call screens, if it still fits above the hint row; returns the next y
static int call_line(const font_t *f, int y, const char *text, color_t c)
{
	char fit[48];
	if (y + f->h > ui_layout.hint_y)
		return y;
	gfx_text_fit(f, text, gfx_width(), fit, sizeof fit);
	gfx_text(f, 0, y, fit, c);
	return y + f->h;
}

// Receiving and Transmitting: the call takes the screen below the status strip
static void draw_dmr_call(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	const radio_dmr_view_t *d = &v->dmr;
	int w = gfx_width();
	char buf[48], title[40];
	sbuf_t b;

	sb_init(&b, title, sizeof title);
	if (v->state == RS_TX && d->target_private && d->target_id != 0xFFFFFFu) {
		// A private call says so where a group call has its "TG": "Private 3101234", else "Private"
		sb_str(&b, TEXT_ALT_OPEN "Private ");
		sb_u32(&b, d->target_id, 1);
		sb_str(&b, TEXT_ALT_NEXT "Private" TEXT_ALT_CLOSE);
	} else if (v->state == RS_TX) {
		sb_target(&b, d->target_id, d->target_private, "");
	} else {
		sb_call_dst(&b, &d->call);
	}
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(v, s, NULL, title) + 1;

	if (v->state == RS_TX) {
		const char *word = "TALKING";
		switch (d->state) {
		case RDMR_TX_WAIT: word = d->rmo && !d->repeater_up ? "WAKING RPT" : "WAITING"; break;
		case RDMR_TX_ENDING: word = "ENDING"; break;
		default: break;
		}
		const font_t *big = gfx_text_width(L->large, word) <= w ? L->large : L->medium;
		gfx_text(big, 0, y, word, ui_col.tx);
		y += big->h + 2;
		// The target with its name, else its short form: a talkgroup's number, a private contact's name
		sb_init(&b, buf, sizeof buf);
		sb_target_alts(&b, d->target_id, d->target_private, d->target_name, "");
		y = call_line(L->medium, y, buf, ui_col.fg);
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "TS");
		sb_u32(&b, d->ts, 1);
		sb_str(&b, " CC");
		sb_u32(&b, d->cc, 1);
		sb_str(&b, d->target_private ? " Private" : " Group");
		if (d->rmo)
			sb_str(&b, " RPT");
		if (d->privacy)
			sb_str(&b, " ENC");
		y = call_line(L->small, y, buf, ui_col.dim);
		sb_init(&b, buf, sizeof buf);
		sb_mmss(&b, v->tx_elapsed_ms / 1000u);
		if (v->tot_s) {
			sb_str(&b, " of ");	// the time-out
			sb_mmss(&b, v->tot_s);
		}
		call_line(L->small, y, buf, ui_col.fg);
		ui_draw_hint(d->state == RDMR_TX_ENDING ? "Finishing the call" : "Release PTT to stop", false);
		return;
	}

	// Receiving: the caller large, then ID, alias, talkgroup, signal
	sb_init(&b, buf, sizeof buf);
	sb_caller(&b, &d->call);
	const font_t *big = gfx_text_width(L->large, buf) <= w ? L->large : L->medium;
	char fit[40];
	gfx_text_fit(big, buf, w, fit, sizeof fit);
	gfx_text(big, 0, y, fit, d->call.emergency ? ui_col.warn : (d->state == RDMR_RX ? ui_col.fg : ui_col.dim));
	y += big->h;
	sb_init(&b, buf, sizeof buf);
	if (d->call.src_name[0] != '\0') {
		sb_u32(&b, d->call.src, 1);
		sb_chr(&b, ' ');
	}
	sb_str(&b, "TS");
	sb_u32(&b, d->ts, 1);
	if (d->late_entry)
		sb_str(&b, " late");
	if (d->call.encrypted)
		sb_str(&b, " ENC");	// privacy marker (played only with the key, never after a PI header)
	if (d->call.emergency)
		sb_str(&b, " EMERGENCY");
	y = call_line(L->small, y, buf, d->call.emergency ? ui_col.warn : ui_col.dim);
	if (d->alias[0] != '\0' && s->alias_show) {
		// The talker alias in quotes, on two lines when it needs them and they fit above the signal line
		char lines[2][64];
		sb_init(&b, buf, sizeof buf);
		sb_chr(&b, '"');
		sb_str(&b, d->alias);
		sb_chr(&b, '"');
		int room = (L->hint_y - y) / L->small->h - 2;	// the talkgroup and signal lines stay
		int n = wrap_lines(L->small, buf, w, room >= 2 ? 2 : 1, lines, false);
		for (int i = 0; i < n; i++)
			y = call_line(L->small, y, lines[i], ui_col.fg);
	}
	if (d->call.gps_valid) {	// the caller's ETSI GPS Info: how far and which way, else where
		sb_init(&b, buf, sizeof buf);
		if (d->call.gps_range) {
			sb_range(&b, d->call.gps_dist_m, d->call.gps_bearing);
		} else {
			sb_degrees(&b, d->call.gps_lat);
			sb_chr(&b, ' ');
			sb_degrees(&b, d->call.gps_lon);
		}
		y = call_line(L->small, y, buf, ui_col.fg);
	}
	// Then why it isn't heard, or where the call goes ("TG 91 World")
	const char *why = d->state == RDMR_RX_OTHER ? (d->call.encrypted ? "Encrypted: not played" :
		"Other TG: not played") : !d->codec ? "No codec: voice off" : NULL;
	if (why != NULL) {
		y = call_line(L->small, y, why, ui_col.warn);
	} else {
		sb_init(&b, buf, sizeof buf);
		sb_call_dst(&b, &d->call);
		y = call_line(L->small, y, buf, ui_col.fg);
	}
	if (y + L->small->h <= L->hint_y) {
		int16_t dbm = d->rssi_dbm != 0 ? d->rssi_dbm : v->rssi_dbm;
		sb_init(&b, buf, sizeof buf);
		sb_i32(&b, dbm);
		sb_str(&b, " dBm");
		int tw = gfx_text_width(L->small, buf);
		gfx_text(L->small, w - tw, y, buf, ui_col.fg);
		draw_meter(0, y + L->small->h / 4, w - tw - L->small->w, L->small->h / 2, dbm);
	}
	ui_draw_hint(d->state == RDMR_RX_OTHER ? "MON: digital monitor" : "PTT replies", false);
}
#endif

static void draw_activity(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *big = L->medium, *sm = L->small;
	int w = gfx_width();
	int y = L->activity_y;
	bool two = L->activity_h >= big->h + sm->h;
	if (!two)
		big = sm;
	int y2 = y + big->h;
	char buf[96], fit[48];
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);

	// The monitor open on a quiet channel shows the meter too: what the receiver hears with nothing to open the squelch
	bool meter = v->state == RS_RX || (v->state == RS_IDLE && v->monitor && v->ch.mode != RADIO_MODE_DMR);
	switch (meter ? RS_RX : v->state) {
	case RS_RX: {
		sb_i32(&b, v->rssi_dbm);
		sb_str(&b, " dBm");
		int tw = gfx_text_width(big, buf);
		gfx_text(big, w - tw, y, buf, ui_col.fg);
		draw_meter(0, y + big->h / 4, w - tw - big->w, big->h / 2, v->rssi_dbm);
		if (two)
			gfx_text(sm, 0, y2, v->monitor ? "Squelch open" : "Receiving", ui_col.rx);
		break;
	}
	case RS_TX: {
		sb_mmss(&b, v->tx_elapsed_ms / 1000u);
		int tw = gfx_text_width(big, buf);
		gfx_text(big, w - tw, y, buf, ui_col.fg);
		gfx_text(big, 0, y, "TALKING", ui_col.tx);
		if (two) {
			sb_init(&b, buf, sizeof buf);
			if (v->tot_s) {
				uint32_t el = v->tx_elapsed_ms / 1000u;
				sb_mmss(&b, el < v->tot_s ? v->tot_s - el : 0);
				sb_str(&b, " left of ");	// the time-out
				sb_mmss(&b, v->tot_s);
			} else {
				sb_str(&b, "No time limit");
			}
			gfx_text(sm, 0, y2, buf, ui_col.dim);
		}
		break;
	}
	case RS_TX_DENIED:
		// One line only: the pill already says NO TX, so the line gives the reason
		gfx_text_fit(sm, deny_reason(v, buf, sizeof buf), w, fit, sizeof fit);
		if (two) {
			gfx_text(big, 0, y, "Can't transmit", ui_col.warn);
			gfx_text(sm, 0, y2, fit, ui_col.fg);
		} else {
			gfx_text(sm, 0, y, fit, ui_col.warn);
		}
		break;
	default:
#if CONFIG_DMR
		if (v->ch.mode == RADIO_MODE_DMR && v->dmr.active) {
			draw_last_heard(v, s, y);	// hear all shows as "All" in the chips and in the hint
			break;
		}
#endif
		if (v->last_rx_ms) {
			sb_str(&b, "Last activity ");
			sb_age(&b, s->now_ms - v->last_rx_ms);
		} else {
			sb_str(&b, "No activity yet");
		}
		bool mon = v->monitor && L->activity_h >= 2 * sm->h;
		y += (L->activity_h - (mon ? 2 : 1) * sm->h) / 2;	// centered in the activity area
		gfx_text_fit(sm, buf, gfx_width(), fit, sizeof fit);
		gfx_text(sm, 0, y, fit, ui_col.dim);
		if (mon)
			gfx_text(sm, 0, y + sm->h, "Squelch open", ui_col.warn);
		break;
	}
}

static const char *home_hint(const radio_view_t *v, const ui_state_t *s)
{
	if (v->state == RS_TX_DENIED)
		return deny_action(v);
	if (s->entry)
		return "OK tune  Back delete";
	if (s->locked)
		return TEXT_ALT_OPEN "Keys locked: hold * to unlock" TEXT_ALT_NEXT "Locked: hold * to unlock" TEXT_ALT_CLOSE;
	if (s->scanning)
		return "# skip  Back stop scan";
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR) {
		if (v->dmr.reply_active)
			return "# caller  Back end reply";
		if (!v->dmr.codec)
			return "No codec: voice off";
		if (v->dmr.hear_all)
			return "Digital monitor  Back off";
	}
#endif
	if (v->temporary)
		return "Back undo  Hold OK save";
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR && v->dmr.target_manual)
		return TEXT_ALT_OPEN "Back: channel's talkgroup" TEXT_ALT_NEXT "Back default target" TEXT_ALT_CLOSE;
#endif
	if (v->vfo)
		return "0-9 freq  * Quick  Up/Dn";
	return s->home_hint != NULL ? s->home_hint : "OK Menu  * Quick";
}

// The largest font no taller than room in which text fits the width (at least the medium font), trying each of the
// text's alternates (font.h) from the longest; out gets the form chosen
static const font_t *large_font(const char *text, int room, char *out, size_t cap)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *fonts[] = { FONT_LARGEST, L->large, L->medium };
	bool more = true;
	for (int level = 0; more; level++) {
		gfx_text_alt(text, level, out, cap, &more);
		for (unsigned i = 0; i < sizeof fonts / sizeof fonts[0]; i++)
			if (fonts[i]->h <= room && gfx_text_width(fonts[i], out) <= gfx_width())
				return fonts[i];
	}
	return L->medium;	// out: the shortest form, shortened when drawn
}

// Large-text Home (accessibility): the status strip, the channel and the talk target, each as large as fits
static void draw_home_large(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	char buf[48], fit[48];
	sbuf_t b;
	int y = L->strip_h + 2;
	int room = (L->hint_y - y) / 2;
	char form[48];
	sb_init(&b, buf, sizeof buf);
	if (v->vfo)
		sb_freq(&b, v->ch.rx_hz);
	else
		sb_str(&b, v->ch.name);
	const font_t *f = large_font(buf, room, form, sizeof form);
	gfx_text_fit(f, form, w, fit, sizeof fit);
	gfx_text(f, 0, y, fit, ui_col.fg);
	y += room;
	sb_init(&b, buf, sizeof buf);
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR && v->dmr.active) {
		// The target with its name, or its short form when the whole doesn't fit even the medium font
		bool named = v->dmr.target_name[0] != '\0' && v->dmr.target_id != 0xFFFFFFu;
		if (named)
			sb_str(&b, TEXT_ALT_OPEN);
		sb_str(&b, GLYPH_ARROW);
		sb_target(&b, v->dmr.target_id, v->dmr.target_private, v->dmr.target_name);
		if (named) {
			sb_str(&b, TEXT_ALT_NEXT GLYPH_ARROW);
			sb_target_short(&b, v->dmr.target_id, v->dmr.target_private, v->dmr.target_name);
			sb_str(&b, TEXT_ALT_CLOSE);
		}
	} else
#endif
	{
		sb_str(&b, v->ch.mode == RADIO_MODE_DMR ? "DMR" : "FM");
		sb_chr(&b, ' ');
		sb_str(&b, ui_power_name(v->power));
	}
	f = large_font(buf, room, form, sizeof form);
	gfx_text_fit(f, form, w, fit, sizeof fit);
	gfx_text(f, 0, y, fit, v->state == RS_TX ? ui_col.tx : v->state == RS_RX ? ui_col.rx : ui_col.fg);
}

void ui_draw_home(const radio_view_t *v, const ui_state_t *s)
{
#if CONFIG_DMR
	ui_st_dmr_off = s->dmr_off;
	if (dmr_call_screen(v)) {
		draw_dmr_call(v, s);
		ui_draw_banner(s);
		if (s->toast != NULL)
			ui_draw_hint(s->toast, true);
		return;
	}
#endif
	gfx_clear(ui_col.bg);
	ui_draw_top(v, s, NULL, NULL);
	if (s->large_text) {
		draw_home_large(v, s);
	} else {
		draw_place(v, s);
		draw_talk(v);
		draw_chips(v, s);
		if (ui_layout.sep_y >= 0)
			gfx_hline(0, ui_layout.sep_y, gfx_width(), ui_col.line);
		draw_activity(v, s);
	}
	ui_draw_banner(s);
	ui_draw_bottom(s, home_hint(v, s));
}

void ui_draw_bottom(const ui_state_t *s, const char *hint)
{
	if (s->toast != NULL)
		ui_draw_hint(s->toast, true);
	else if (s->hints && hint != NULL)
		ui_draw_hint(hint, false);
}

static void fit_end(const font_t *f, const char *s, int max_w, char *out, size_t len);

// Up to max lines of t wrapped to max_w: lines[] filled, the count returned. Text left after the last line shortens
// it (at the end for a preview, else from the middle), so nothing is silently dropped
static int wrap_lines(const font_t *f, const char *t, int max_w, int max, char lines[][64], bool preview)
{
	int n = 0;
	while (*t && n < max) {
		const char *next = wrap_line(f, t, max_w, lines[n], sizeof lines[0]);
		if (n == max - 1 && *next) {
			if (preview)
				fit_end(f, t, max_w, lines[n], sizeof lines[0]);
			else
				gfx_text_fit(f, t, max_w, lines[n], sizeof lines[0]);
		}
		t = next;
		n++;
	}
	return n;
}

// Banner: a framed box above the hint row with the title, the text and how to act on it. Title and text take a second
// line each when they need it, so they are shown whole
void ui_draw_banner(const ui_state_t *s)
{
	const ui_banner_t *bn = &s->banner;
	if (bn->kind == UI_BANNER_NONE)
		return;
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	char title[2][64], text[2][64];
	int nt;
	const char *from = strstr(bn->title, " from ");
	if (gfx_text_width(f, bn->title) > w - 4 && from != NULL && (from - bn->title) + 5 < (int)sizeof title[0]) {
		// "Call alert from" / "Jodi Ruiz": the station's name together on the second line
		size_t n = (size_t)(from - bn->title) + 5;
		memcpy(title[0], bn->title, n);
		title[0][n] = 0;
		nt = 1 + wrap_lines(f, from + 6, w - 4, 1, &title[1], false);
	} else {
		nt = wrap_lines(f, bn->title, w - 4, 2, title, false);
	}
	int nx = bn->text[0] ? wrap_lines(f, bn->text, w - 4, 2, text, bn->kind == UI_BANNER_MESSAGE) : 0;
	int pad = gfx_height() >= UI_TALL_PX ? 2 : 1;
	int h = (nt + nx + 1) * f->h + 2 * pad;
	int y = L->hint_y - h;
	int top = overlay_top(y);	// the box grows up to a gap between lines; its text stays at the bottom
	color_t edge = bn->warn ? ui_col.warn : ui_col.focus;
	if (gfx_bpp() == 1) {
		// inverse video box (wireframe 128x64)
		gfx_fill(0, top, w, L->hint_y - top, ui_col.fg);
	} else {
		gfx_fill(0, top, w, L->hint_y - top, ui_col.hint_bg);
		gfx_rect(0, top, w, L->hint_y - top, edge);
	}
	color_t fg = gfx_bpp() == 1 ? ui_col.bg : ui_col.fg;
	int ty = y + pad;
	for (int i = 0; i < nt; i++, ty += f->h)
		gfx_text(f, 2, ty, title[i], gfx_bpp() == 1 ? fg : bn->warn ? ui_col.warn : ui_col.fg);
	for (int i = 0; i < nx; i++, ty += f->h)
		gfx_text(f, 2, ty, text[i], fg);
	const char *act = bn->kind == UI_BANNER_MESSAGE ? "# Open" :
		(bn->kind == UI_BANNER_CALL_ALERT || bn->kind == UI_BANNER_PRIVATE) ? "# Actions" : "Back hide";
	gfx_text(f, 2, ty, act, gfx_bpp() == 1 ? fg : ui_col.dim);
	char sec[8];
	sbuf_t b;
	sb_init(&b, sec, sizeof sec);
	uint32_t left = (int32_t)(bn->until_ms - s->now_ms) > 0 ? bn->until_ms - s->now_ms : 0;
	sb_u32(&b, (left + 999u) / 1000u, 1);
	sb_chr(&b, 's');
	gfx_text(f, w - 2 - gfx_text_width(f, sec), ty, sec, gfx_bpp() == 1 ? fg : ui_col.dim);
}

// --- List screens (Hub, Contacts, Find, Settings, ...): title, optional header and body, rows, hint ---

static int list_top(const radio_view_t *v, const ui_state_t *s, const ui_list_t *l, bool draw)
{
	int y = draw ? ui_draw_top(v, s, l->prefix, l->name) : ui_layout.strip_h +
		((l->prefix || l->name) && gfx_height() >= UI_TALL_PX ? ui_layout.small->h + 2 : 0);
	if (l->header != NULL) {
		if (draw) {
			char fit[48];
			gfx_text_fit(ui_layout.small, l->header, gfx_width(), fit, sizeof fit);
			gfx_text(ui_layout.small, 0, y, fit, ui_col.warn);
		}
		y += ui_layout.small->h;
	}
	return y;
}

static int list_bottom(const ui_state_t *s)
{
	return s == NULL || s->hints || s->toast ? ui_layout.hint_y : gfx_height();
}

// One line of word-wrapped text: copies into line what fits in max_w pixels (breaking at the last space, or inside a
// word longer than the line, never shortening), returns where the next line starts
static const char *wrap_line(const font_t *f, const char *t, int max_w, char *line, size_t cap)
{
	int n = 0, cut = 0;
	while (t[n] && t[n] != '\n' && n < (int)cap - 1) {
		line[n] = t[n];
		line[n + 1] = 0;
		if (gfx_text_width(f, line) > max_w)
			break;
		if (t[n] == ' ')
			cut = n;
		n++;
	}
	bool fits_all = t[n] == 0 || t[n] == '\n';
	if (!fits_all && cut > 0)
		n = cut;
	if (n == 0 && t[0] && t[0] != '\n')
		n = 1;		// narrower than one glyph: still move on
	line[n] = 0;
	t += n;
	if (*t == '\n')
		t++;		// an empty line ("\n\n") stays one line
	else
		while (*t == ' ')
			t++;
	return t;
}

// Word wrap into lines of the screen's width; draws them (when draw) and returns the y after the last line
static int wrap_text(const font_t *f, const char *t, int y, int y_end, bool draw, color_t c)
{
	char line[64];
	while (*t && y + f->h <= y_end) {
		const char *next = wrap_line(f, t, gfx_width(), line, sizeof line);
		if (*next && y + 2 * f->h > y_end)
			fit_end(f, t, gfx_width(), line, sizeof line);	// the last line that fits: never drop text silently
		t = next;
		if (draw)
			gfx_text(f, 0, y, line, c);
		y += f->h;
	}
	return y;
}

int ui_draw_text_page(const radio_view_t *v, const ui_state_t *s, const char *title, const char *text,
	uint16_t *first)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->tiny;
	int w = gfx_width();
	char line[64];
	gfx_clear(ui_col.bg);
	// No status strip while idle: the page gets the whole height. A call or a transmission brings it back (P2: the
	// TX / RX pill is never hidden)
	int y = v->state != RS_IDLE ? ui_draw_top(v, s, NULL, title) : 0;
	int y_end = list_bottom(s);
	int rows = (y_end - y) / f->h;
	if (rows < 1)
		rows = 1;
	int total = 0;
	for (const char *t = text; *t; total++)
		t = wrap_line(f, t, w, line, sizeof line);
	int max_first = total > rows ? total - rows : 0;
	if (*first > max_first)
		*first = (uint16_t)max_first;
	const char *t = text;
	for (int i = 0; *t; i++) {
		t = wrap_line(f, t, w, line, sizeof line);
		int r = i - *first;
		if (r < 0)
			continue;
		if (r >= rows)
			break;
		// "Label: value" lines show the label dim
		const char *colon = strstr(line, ": ");
		int x = 0;
		if (colon != NULL && colon - line < 20) {
			char label[24];
			size_t n = (size_t)(colon - line) + 1;
			memcpy(label, line, n);
			label[n] = 0;
			x = gfx_text(f, 0, y + r * f->h, label, gfx_bpp() == 1 ? ui_col.fg : ui_col.dim);
			gfx_text(f, x, y + r * f->h, colon + 1, ui_col.fg);
		} else {
			gfx_text(f, 0, y + r * f->h, line, ui_col.fg);
		}
	}
	if (*first > 0)
		gfx_fill(w - 3, y, 3, 2, ui_col.dim);
	if (*first < max_first)
		gfx_fill(w - 3, y + rows * f->h - 2, 3, 2, ui_col.dim);
	ui_draw_banner(s);
	ui_draw_bottom(s, max_first > 0 ? "Up/Dn more  Back" : "Back");
	return max_first;
}

// s shortened at the end, at a word if one ends late enough, with an ellipsis: for a preview of longer text (a
// message), where the start is what matters
static void fit_end(const font_t *f, const char *s, int max_w, char *out, size_t len)
{
	size_t n = strlen(s), fit = max_w <= 0 ? 0 : (size_t)(max_w / f->w);
	if (fit >= len)
		fit = len - 1;
	if (n <= fit) {
		memcpy(out, s, n + 1);
		return;
	}
	if (fit == 0) {
		out[0] = 0;
		return;
	}
	size_t keep = fit - 1;
	size_t cut = keep;
	while (cut > 0 && s[cut] != ' ')
		cut--;
	if (cut >= keep / 2)
		keep = cut;		// end at a word unless that throws away more than half
	while (keep > 0 && s[keep - 1] == ' ')
		keep--;
	memcpy(out, s, keep);
	out[keep] = GLYPH_ELLIPSIS[0];
	out[keep + 1] = 0;
#ifdef GFX_FIT_TRACE
	gfx_fit_trace(s, out);
#endif
}

// A row's label (left) and value (right, may hold text alternates). The label says what the row is, so it is kept
// whole whenever it can be: the value gets the room that is left, as the longest of its alternates that fits (an
// alternate may be empty: the value is left out). Only a label too long even next to the shortest value is shortened:
// at the end for a preview, else from the middle (gfx_text_fit). Margins shrink to nothing before anything is cut
static void row_text(const font_t *f, int x, int y, int w, const char *label, const char *value, color_t fg,
	color_t vfg, bool preview)
{
	char lab[48], val[48];
	bool more = true;
	int gap = f->w / 2 + 1;
	int lw = gfx_text_width(f, label);
	int vw = 0;
	val[0] = 0;
	// The longest value alternate that fits next to the whole label (or the shortest one there is)
	for (int level = 0; value[0] && more; level++) {
		size_t n = gfx_text_alt(value, level, val, sizeof val, &more);
		vw = (int)n * f->w;
		if (x + lw + (vw ? gap + vw : 0) <= w)
			break;
	}
	int room = w - x - (vw ? vw + gap : 0);
	if (lw <= room) {
		gfx_text(f, x, y, label, fg);
	} else {
		if (preview && !strchr(label, TEXT_ALT_OPEN[0]))
			fit_end(f, label, room, lab, sizeof lab);
		else
			gfx_text_fit(f, label, room, lab, sizeof lab);
		gfx_text(f, x, y, lab, fg);
	}
	if (vw) {
		int right = x + (lw < room ? lw : room) + gap + vw < w ? 1 : 0;	// a pixel of margin if it is free
		gfx_text(f, w - vw - right, y, val, vfg);
	}
}

int ui_list_rows(const ui_list_t *l)
{
	int y = list_top(NULL, NULL, l, false);
	if (l->body != NULL)
		y = wrap_text(ui_layout.small, l->body, y, y + (ui_layout.hint_y - y) / 2 + ui_layout.small->h, false, 0) + 2;
	int n = (ui_layout.hint_y - y) / ui_layout.row_h;
	return n < 1 ? 1 : n;
}

void ui_draw_list(const radio_view_t *v, const ui_state_t *s, const ui_list_t *l)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	gfx_clear(ui_col.bg);
	int y = list_top(v, s, l, true);
	int y_end = list_bottom(s);
	if (l->body != NULL) {
		int body_end = l->count ? y + (y_end - y) / 2 + f->h : y_end;
		y = wrap_text(f, l->body, y, body_end, true, ui_col.fg);
		if (l->count) {
			gfx_hline(0, y, w, ui_col.line);
			y += 2;
		}
	}
	int rows = (y_end - y) / L->row_h;
	if (rows < 1)
		rows = 1;
	uint16_t top = l->top != NULL ? *l->top : 0;
	if (l->focus < top)
		top = l->focus;
	if (l->focus >= top + rows)
		top = (uint16_t)(l->focus - rows + 1);
	if (top + rows > l->count)
		top = (uint16_t)(l->count > rows ? l->count - rows : 0);
	if (l->top != NULL)
		*l->top = top;
	if (l->count == 0 && l->empty != NULL)
		wrap_text(f, l->empty, y + 1, y_end, true, ui_col.dim);
	int pad = (L->row_h - f->h) / 2;
	for (int r = 0; r < rows && top + r < l->count; r++) {
		uint16_t i = (uint16_t)(top + r);
		ui_item_t it;
		it.label[0] = it.value[0] = 0;
		it.num = 0;
		it.flags = 0;
		l->item(i, &it);
		int ry = y + r * L->row_h;
		bool focus = i == l->focus && !(it.flags & UI_ITEM_INFO);
		color_t fg = (it.flags & UI_ITEM_DIM) ? ui_col.line : (it.flags & UI_ITEM_WARN) ? ui_col.warn : ui_col.fg;
		if (gfx_bpp() == 1)
			fg = ui_col.fg;
		if ((it.flags & UI_ITEM_INFO) && gfx_bpp() != 1)
			fg = ui_col.dim;
		color_t vfg = (it.flags & UI_ITEM_DIM) && gfx_bpp() != 1 ? ui_col.line : ui_col.dim;
		if (it.flags & UI_ITEM_TEMP)
			vfg = ui_col.warn;
		if (focus) {
			color_t bg = (it.flags & UI_ITEM_EDIT) ? ui_col.warn : ui_col.focus;
			gfx_fill(0, ry, w, L->row_h, bg);
			fg = gfx_bpp() == 1 ? ui_col.bg : (it.flags & UI_ITEM_EDIT) ? ui_col.pill_dark_fg : ui_col.fg;
			vfg = fg;
		}
		int x = 1;
		if (gfx_bpp() == 1 && !focus)
			x = gfx_text(f, 0, ry + pad, " ", fg);
		if (it.num) {
			char n[3] = { it.num, ' ', 0 };
			x = gfx_text(f, x, ry + pad, n, focus ? fg : (gfx_bpp() == 1 ? fg : ui_col.dim));
		}
		char val[48];
		if (it.flags & UI_ITEM_EDIT) {
			sbuf_t b;
			sb_init(&b, val, sizeof val);
			sb_chr(&b, '<');
			sb_str(&b, it.value);
			sb_chr(&b, '>');
		} else {
			strcpy(val, it.value);
		}
		row_text(f, x, ry + pad, w, it.label, val, fg, vfg, (it.flags & UI_ITEM_PREVIEW) != 0);
	}
	// Scroll marks: more above / below
	if (top > 0)
		gfx_fill(w - 3, y, 3, 2, ui_col.dim);
	if (top + rows < l->count)
		gfx_fill(w - 3, y + rows * L->row_h - 2, 3, 2, ui_col.dim);
	ui_draw_banner(s);
	ui_draw_bottom(s, l->hint);
}

// Received emergency: the whole screen, a word label and the caller (P5: never color alone)
void ui_draw_emergency(const radio_view_t *v, const ui_state_t *s, uint32_t id, const char *name)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	char buf[32];
	sbuf_t b;
	gfx_clear(gfx_bpp() == 1 ? ui_col.bg : ui_col.tx);
	int y = ui_draw_top(v, s, "Emergency", NULL) + 2;
	const font_t *big = gfx_text_width(L->large, "EMERGENCY") <= w ? L->large : L->medium;
	if (gfx_bpp() == 1) {
		gfx_fill(0, y, w, big->h, ui_col.fg);
		gfx_text(big, (w - gfx_text_width(big, "EMERGENCY")) / 2, y, "EMERGENCY", ui_col.bg);
	} else {
		gfx_text(big, (w - gfx_text_width(big, "EMERGENCY")) / 2, y, "EMERGENCY", ui_col.fg);
	}
	y += big->h + 2;
	sb_init(&b, buf, sizeof buf);
	if (name != NULL && name[0])
		sb_str(&b, name);
	else
		sb_u32(&b, id, 1);
	char fit[40];
	gfx_text_fit(L->medium, buf, w, fit, sizeof fit);
	gfx_text(L->medium, (w - gfx_text_width(L->medium, fit)) / 2, y, fit, ui_col.fg);
	y += L->medium->h;
	if (name != NULL && name[0] && y + L->small->h <= L->hint_y) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "ID ");
		sb_u32(&b, id, 1);
		gfx_text(L->small, (w - gfx_text_width(L->small, buf)) / 2, y, buf, ui_col.fg);
	}
	ui_draw_hint(s->toast != NULL ? s->toast : "OK acknowledge  Back hide", s->toast != NULL);
}

// --- Hotspot ---
#if CONFIG_HOTSPOT
void ui_draw_hotspot(const radio_view_t *v, const hotspot_view_t *h, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	char buf[48];
	sbuf_t b;
	radio_view_t pill = *v;		// the strip's pill shows what the hotspot does
	pill.state = h->transmitting ? RS_TX : h->receiving ? RS_RX : RS_IDLE;

	gfx_clear(ui_col.bg);
	int y = ui_draw_top(&pill, s, "Hotspot", NULL) + 1;
	const char *word = !h->running ? "STARTING" : h->transmitting ? "TX NET" : h->receiving ? "RX RF" : "READY";
	const font_t *big = gfx_text_width(L->large, word) <= w ? L->large : L->medium;
	gfx_text(big, 0, y, word, h->transmitting ? ui_col.tx : h->receiving ? ui_col.rx : ui_col.fg);
	y += big->h + 1;

	sb_init(&b, buf, sizeof buf);
	if (h->rx_hz == 0) {
		sb_str(&b, "No frequency yet");
	} else if (h->rx_hz == h->tx_hz) {
		sb_freq(&b, h->rx_hz);
		sb_str(&b, " MHz");
	} else {
		sb_str(&b, "RX ");
		sb_freq(&b, h->rx_hz);
		sb_str(&b, " TX ");
		sb_freq(&b, h->tx_hz);
	}
	y = call_line(L->medium, y, buf, ui_col.fg);
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "CC");
	sb_u32(&b, h->cc, 1);
	sb_str(&b, " ");
	sb_str(&b, ui_power_name(v->power));
	y = call_line(L->small, y, buf, ui_col.dim);
	sb_init(&b, buf, sizeof buf);
	if (h->src != 0) {
		sb_str(&b, h->from_net ? "Net " : "RF ");
		sb_u32(&b, h->src, 1);
		sb_str(&b, " " GLYPH_ARROW " ");
		if (h->group)
			sb_str(&b, "TG ");
		sb_u32(&b, h->dst, 1);
	} else {
		sb_str(&b, "No calls yet");
	}
	call_line(L->small, y, buf, ui_col.fg);
	ui_draw_hint(s->toast != NULL ? s->toast : "OK stop hotspot", s->toast != NULL);
}
#endif

// --- Quick settings (3x3 grid, positions match the keypad; a numbered list on 128x64) ---

uint8_t ui_quick_kind(int pos, const radio_view_t *v)
{
	static const uint8_t fm[UI_QUICK_TILES] = {
		QT_POWER, QT_SQUELCH, QT_TONE, QT_MODE, QT_MONITOR, QT_VOX, QT_SCAN, QT_BANDWIDTH, QT_BRIGHTNESS,
	};
	static const uint8_t dmr[UI_QUICK_TILES] = {
		QT_POWER, QT_TIMESLOT, QT_CC, QT_MODE, QT_HEAR_ALL, QT_PRIVACY, QT_SCAN, QT_TALK_TO, QT_BRIGHTNESS,
	};
	if (pos < 0 || pos >= UI_QUICK_TILES)
		return QT_POWER;
	if (v->ch.mode == RADIO_MODE_DMR)
		return dmr[pos];
	if (v->vfo && pos == 7)
		return QT_STEP;		// the VFO's step replaces bandwidth (task-flows.md 12)
	return fm[pos];
}

static bool feat(uint32_t f)
{
	return (ui_svc_features() & f) != 0;
}

bool ui_quick_tile(int tile, const radio_view_t *v, const ui_state_t *s, char *label, size_t label_len,
	char *value, size_t value_len)
{
	// The whole word, and an abbreviation of 6 characters at most for a tile (font.h alternates), so a tile's name
	// never runs into the next tile's digit in the label font
#define QT_NAME(full, abbr) TEXT_ALT_OPEN full TEXT_ALT_NEXT abbr TEXT_ALT_CLOSE
	static const char *const names[] = {
		[QT_POWER] = "Power", [QT_SQUELCH] = QT_NAME("Squelch", "SQL"), [QT_TONE] = "Tone", [QT_MODE] = "Mode",
		[QT_MONITOR] = QT_NAME("Monitor", "MON"), [QT_VOX] = "VOX", [QT_SCAN] = "Scan", [QT_BANDWIDTH] = "Width",
		[QT_BRIGHTNESS] = "Bright", [QT_TIMESLOT] = QT_NAME("Timeslot", "Slot"), [QT_CC] = "Color",
		[QT_HEAR_ALL] = QT_NAME("Digital monitor", "DMon"), [QT_PRIVACY] = QT_NAME("Privacy", "Priv"),
		[QT_TALK_TO] = QT_NAME("Talk to", "Talk"), [QT_STEP] = "Step",
	};
#undef QT_NAME
	uint8_t kind = ui_quick_kind(tile, v);
	bool editing = s->quick_edit && s->quick_focus == tile;
	sbuf_t b;
	sb_init(&b, label, label_len);
	sb_chr(&b, (char)('1' + tile));
	sb_str(&b, names[kind]);
	sb_init(&b, value, value_len);
	switch (kind) {
	case QT_POWER:
		if (v->power != channel_power(v))
			sb_str(&b, GLYPH_DOT);
		sb_str(&b, ui_power_name(v->power));
		return true;
	case QT_SQUELCH:
		if (v->temp & RADIO_TEMP_SQUELCH)
			sb_str(&b, GLYPH_DOT);
		sb_u32(&b, v->squelch, 1);
		return true;
	case QT_TONE: {
		if (!CONFIG_SUBTONE)
			return false;
		if (editing && s->tone_target != RADIO_TONE_BOTH) {
			sbuf_t lb;
			sb_init(&lb, label, label_len);
			sb_chr(&lb, (char)('1' + tile));
			sb_str(&lb, s->tone_target == RADIO_TONE_TX ? "Tone TX" : "Tone RX");
		}
		if (!editing && (v->temp & (RADIO_TEMP_TX_TONE | RADIO_TEMP_RX_TONE)))
			sb_str(&b, GLYPH_DOT);
		const subtone_t *t = &v->ch.tx_tone;
		bool same = v->ch.tx_tone.type == v->ch.rx_tone.type &&
			(v->ch.tx_tone.type == TONE_NONE || v->ch.tx_tone.value == v->ch.rx_tone.value);
		if (editing) {
			t = &s->tone_edit;
		} else if (!same && v->ch.tx_tone.type != TONE_NONE) {
			sb_chr(&b, 'T');	// TX only, or TX and RX differ: the TX tone, marked
		} else if (!same) {
			sb_chr(&b, 'R');
			t = &v->ch.rx_tone;
		}
		if (t->type == TONE_NONE)
			sb_str(&b, "None");
		else
			sb_tone(&b, t);
		return v->ch.mode == RADIO_MODE_FM;
	}
	case QT_MODE:
		sb_str(&b, v->ch.mode == RADIO_MODE_DMR ? GLYPH_ARROW "FM" : GLYPH_ARROW "DMR");
		return feat(UI_FEAT_MODE) && CONFIG_FM && CONFIG_DMR && !v->vfo;	// the VFO is FM only for now
	case QT_MONITOR:
		sb_str(&b, v->monitor ? "On" : "Off");
		return feat(UI_FEAT_MONITOR_LATCH);
	case QT_VOX:
		sb_str(&b, v->vox_on ? "On" : "Off");
		return feat(UI_FEAT_VOX) && v->ch.mode == RADIO_MODE_FM;
	case QT_SCAN:
		sb_str(&b, s->scanning ? "On" : "Off");
		return feat(UI_FEAT_SCAN);
	case QT_BANDWIDTH:
		if (v->temp & RADIO_TEMP_BW)
			sb_str(&b, GLYPH_DOT);
		sb_str(&b, v->ch.bw == RADIO_BW_25K ? "25k" : "12.5k");
		return v->ch.mode == RADIO_MODE_FM;
	case QT_STEP: {
		uint32_t st = v->vfo_step_hz;
		sb_u32(&b, st / 1000u, 1);
		if (st % 1000u) {
			sb_chr(&b, '.');
			sb_u32(&b, (st % 1000u) / 100u, 1);
			if ((st % 100u) != 0)
				sb_u32(&b, (st % 100u) / 10u, 1);
		}
		sb_chr(&b, 'k');
		return false;	// the radio core has no step request yet
	}
	case QT_BRIGHTNESS:
		sb_u32(&b, s->brightness, 1);
		sb_chr(&b, '%');
		return true;
#if CONFIG_DMR
	case QT_TIMESLOT:
		sb_str(&b, "TS");
		sb_u32(&b, v->dmr.ts ? v->dmr.ts : v->ch.timeslot, 1);
		return feat(UI_FEAT_TIMESLOT);
	case QT_CC:
		sb_str(&b, "CC");
		sb_u32(&b, editing ? s->cc_edit : v->dmr.active ? v->dmr.cc : v->ch.color_code, 1);
		return feat(UI_FEAT_COLOR_CODE);
	case QT_HEAR_ALL:
		sb_str(&b, s->hear_all ? "On" : "Off");
		return feat(UI_FEAT_HEAR_ALL);
	case QT_PRIVACY:
		sb_str(&b, s->privacy ? "On" : "Off");
		return feat(UI_FEAT_PRIVACY) && CONFIG_DMR_PRIVACY;
	case QT_TALK_TO:
		if (v->dmr.target_private)
			sb_u32(&b, v->dmr.target_id, 1);
		else if (v->dmr.target_id == 0xFFFFFFu)
			sb_str(&b, "All");
		else
			sb_u32(&b, v->dmr.target_id, 1);
		return true;
#endif
	default:
		return false;
	}
}

static uint16_t quick_list_focus;
static const radio_view_t *quick_v;
static const ui_state_t *quick_s;

static void quick_list_item(uint16_t i, ui_item_t *it)
{
	char label[24];
	bool ok = ui_quick_tile(i, quick_v, quick_s, label, sizeof label, it->value, sizeof it->value);
	it->num = label[0];
	strcpy(it->label, label + 1);	// the whole word: a list row has room for it
	if (!ok)
		it->flags |= UI_ITEM_DIM;
	if (quick_s->quick_edit && quick_s->quick_focus == i)
		it->flags |= UI_ITEM_EDIT;
}

void ui_draw_quick(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width(), h = gfx_height();
	const char *name = v->vfo ? "VFO" : v->ch.name;

	if (w < 150) {
		// 128x64: a numbered list with the same numbers (wireframe 128x64)
		static uint16_t top;
		quick_v = v;
		quick_s = s;
		quick_list_focus = s->quick_focus;
		ui_list_t l = { "Quick", name, NULL, NULL, UI_QUICK_TILES, quick_list_focus, &top, quick_list_item, NULL,
				"1-9 change  Hold OK save" };
		ui_draw_list(v, s, &l);
		return;
	}
	gfx_clear(ui_col.bg);
	// Title row keeps the state pill (P2)
	int top = ui_draw_top(v, s, "Quick", name);
	int bottom = h;
	if (s->toast != NULL || h >= UI_TALL_PX)
		bottom = L->hint_y;
	int gh = bottom - top;
	const font_t *lf = L->small;
	const font_t *vf = L->medium;
	if (gh / 3 < lf->h + vf->h + 2)
		vf = lf;
	if (gh / 3 < lf->h + vf->h)
		lf = L->tiny;
	for (int i = 0; i < UI_QUICK_TILES; i++) {
		int cx = i % 3, cy = i / 3;
		int x0 = w * cx / 3, x1 = w * (cx + 1) / 3 - (cx == 2 ? 1 : 0);
		int y0 = top + gh * cy / 3, y1 = top + gh * (cy + 1) / 3 - (cy == 2 ? 1 : 0);
		char label[24], value[12], fit[12];	// label: "5" and the name's alternates
		bool ok = ui_quick_tile(i, v, s, label, sizeof label, value, sizeof value);
		bool focus = i == s->quick_focus;
		color_t fg = ok ? ui_col.fg : ui_col.line, bg = ui_col.bg;
		if (gfx_bpp() == 1 && !ok)
			fg = ui_col.fg;
		if (focus) {
			bg = s->quick_edit ? ui_col.warn : ui_col.focus;
			fg = gfx_bpp() == 1 ? ui_col.bg : s->quick_edit ? ui_col.pill_dark_fg : ui_col.fg;
			gfx_fill(x0 + 1, y0 + 1, x1 - x0 - 1, y1 - y0 - 1, bg);
		}
		gfx_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, ui_col.line);
		gfx_clip(x0 + 1, y0 + 1, x1 - x0 - 1, y1 - y0 - 1);
		// The key's digit, half a character, then the name (it ends before the next tile's digit)
		char key[2] = { label[0], 0 };
		int nx = gfx_text(lf, x0 + 2, y0 + 1, key, fg) + lf->w / 2;
		gfx_text_fit(lf, label + 1, x1 - nx - (cx < 2 ? lf->w / 2 : 1), fit, sizeof fit);	// a gap before the next tile
		gfx_text(lf, nx, y0 + 1, fit, focus ? fg : ok ? ui_col.dim : fg);
		if (focus && s->quick_edit) {
			// Editing: arrows around the value say Up/Dn changes it
			sbuf_t vb;
			char tmp[16];
			sb_init(&vb, tmp, sizeof tmp);
			sb_chr(&vb, '<');
			sb_str(&vb, value);
			sb_chr(&vb, '>');
			memcpy(value, tmp, sizeof value - 1);
			value[sizeof value - 1] = 0;
		}
		const font_t *tf = vf;	// a value too wide for the tile ("<D023N>") drops to the label font
		if (gfx_text_width(tf, value) > x1 - x0 - 3)
			tf = lf;
		gfx_text_fit(tf, value, x1 - x0 - 3, fit, sizeof fit);
		int vx = x0 + (x1 - x0 + 1 - gfx_text_width(tf, fit)) / 2;
		int vy = y0 + 1 + lf->h + (y1 - y0 - 1 - lf->h - tf->h) / 2;
		gfx_text(tf, vx, vy, fit, fg);
		gfx_clip_off();
	}
	ui_draw_banner(s);
	if (s->toast != NULL)
		ui_draw_hint(s->toast, true);
	else if (h >= UI_TALL_PX)
		ui_draw_hint("0 All settings  Hold OK save", false);
}

#if CONFIG_DIAG
static uint8_t diag_lines_per_page;

uint8_t ui_diag_next_page(uint8_t first_line)
{
	char buf[40];
	uint8_t next = (uint8_t)(first_line + (diag_lines_per_page ? diag_lines_per_page : 1));
	if (board.diag_line == NULL || !board.diag_line(next, buf, sizeof buf))
		return 0;
	return next;
}

void ui_draw_diag(const radio_view_t *v, uint32_t keys, int32_t enc_total, uint8_t first_line)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->tiny != NULL ? L->tiny : L->small;	// small print: as many lines as fit
	char buf[40];
	sbuf_t b;
	int y = 0;

	gfx_clear(ui_col.bg);
	gfx_text(f, 0, y, "Diagnostics", ui_col.warn);
	y += f->h;

	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "keys ");
	for (int i = 5; i >= 0; i--)
		sb_chr(&b, "0123456789ABCDEF"[(keys >> (4 * i)) & 0xF]);
	sb_str(&b, " enc ");
	if (enc_total < 0) {
		sb_chr(&b, '-');
		enc_total = -enc_total;
	}
	sb_u32(&b, (uint32_t)enc_total, 1);
	gfx_text(f, 0, y, buf, ui_col.fg);
	y += f->h;

	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "rx ");
	if (v->rssi_dbm < 0) {
		sb_chr(&b, '-');
		sb_u32(&b, (uint32_t)-v->rssi_dbm, 1);
	} else {
		sb_u32(&b, (uint32_t)v->rssi_dbm, 1);
	}
	sb_str(&b, "dBm nz ");
	sb_u32(&b, v->noise, 1);
	sb_str(&b, v->state == RS_RX ? " open" : v->state == RS_TX ? " TX" : " closed");
	if (v->ch.rx_tone.type != TONE_NONE)
		sb_str(&b, v->tone_ok ? " tone" : " -tone");
	gfx_text(f, 0, y, buf, ui_col.fg);
	y += f->h;

	diag_lines_per_page = 0;
	for (uint8_t i = first_line; board.diag_line != NULL && y + f->h <= gfx_height() - f->h; i++) {
		diag_lines_per_page++;
		if (!board.diag_line(i, buf, sizeof buf))
			break;
		gfx_text(f, 0, y, buf, ui_col.dim);
		y += f->h;
	}
	gfx_text(f, 0, gfx_height() - f->h, "* more  Back", ui_col.idle);
}
#endif
