// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Home, quick settings and the boot splash, laid out from the display size (docs/ux/information-architecture.md 3, 5;
// docs/ux/wireframes). Regions top to bottom: status strip, place, talk target, activity, hint row.
#include <string.h>
#include "board.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "ui_svc.h"
#include "cjk_font.h"	// CJK_FONT_H: the line height a CJK language needs (CONFIG_LANG_ZH)
#include "config.h"
#include "codeplug/codeplug_modes.h"	// channel kinds: the mode word
#if CONFIG_DMR_TRUNK
#include "dmr/trunk/t3_ms.h"	// the engine's states, for the trunked status words
#endif

ui_layout_t ui_layout;
static const font_t *font_largest;	// the large text setting's font (large_font)

// Colors carry meaning only (P5); every color cue also has a word. On 1 bpp panels everything is ink on paper
// and pills are inverse video
ui_colors_t ui_col;
static uint8_t ui_theme;

static uint8_t ui_accent;	// UI_ACCENT_*
static bool ui_strip_fill;

// The highlight color over the theme's own (Settings > Display), one shade per color for both the filled status strip
// (reverse video: the color as background, text and icons black) and the focus highlight (black text), so the two
// match; also Home's channel number. All are light shades: black on each is at least 7.5:1 after RGB565 rounding.
// The red is lighter and pinker than TX's (the TX pill also says TX); there is no green, which would hide the RX
// pill. Default keeps the theme's blue focus; its filled strip is light gray (white in high contrast). Built without CONFIG_UI_COLOR (radios
// with only a monochrome screen) the theme's own colors stay. Night keeps its brightest red for all of them whatever the
// highlight color, so nothing but red reaches the eye
static void accent_apply(void)
{
	ui_col.focus_fg = ui_col.fg;
	ui_col.chnum = ui_col.dim;
	if (!CONFIG_UI_COLOR)
		return;
	if (ui_theme == UI_THEME_NIGHT) {
		ui_col.strip_bg = ui_col.focus = ui_col.chnum = ui_col.fg;
		ui_col.strip_warn = ui_col.focus_fg = ui_col.bg;
		return;
	}
	static const color_t acc[UI_ACCENT_COUNT] = {
		[UI_ACCENT_DEFAULT] = RGB565(216, 216, 216),
		[UI_ACCENT_RED] = RGB565(240, 112, 112),
		[UI_ACCENT_ORANGE] = RGB565(255, 160, 64),
		[UI_ACCENT_YELLOW] = RGB565(255, 208, 0),
		[UI_ACCENT_TEAL] = RGB565(72, 210, 210),
		[UI_ACCENT_BLUE] = RGB565(104, 176, 255),
		[UI_ACCENT_PURPLE] = RGB565(184, 150, 255),
		[UI_ACCENT_PINK] = RGB565(255, 136, 200),
	};
	// Warnings on the filled strip (SCAN, MON, ROAM, unread, the lock, a low battery): dark amber where it reads, dark
	// blue on the warm strips where amber can't; at least 5:1 on its strip after RGB565 rounding
	static const color_t warn[UI_ACCENT_COUNT] = {
		[UI_ACCENT_DEFAULT] = RGB565(136, 68, 0),
		[UI_ACCENT_RED] = RGB565(0, 32, 120),
		[UI_ACCENT_ORANGE] = RGB565(0, 48, 168),
		[UI_ACCENT_YELLOW] = RGB565(128, 64, 0),
		[UI_ACCENT_TEAL] = RGB565(112, 56, 0),
		[UI_ACCENT_BLUE] = RGB565(88, 44, 0),
		[UI_ACCENT_PURPLE] = RGB565(80, 40, 0),
		[UI_ACCENT_PINK] = RGB565(0, 40, 152),
	};
	uint8_t a = ui_accent < UI_ACCENT_COUNT ? ui_accent : UI_ACCENT_DEFAULT;
	color_t c = acc[a];
	if (a == UI_ACCENT_DEFAULT && ui_theme == UI_THEME_HIGH_CONTRAST)
		c = RGB565(255, 255, 255);	// high contrast has no gray: its default strip is white
	ui_col.strip_bg = c;
	ui_col.strip_warn = warn[a];
	if (a != UI_ACCENT_DEFAULT) {
		ui_col.focus = c;
		ui_col.focus_fg = RGB565(0, 0, 0);
		ui_col.chnum = ui_theme == UI_THEME_LIGHT ? ui_col.fg : c;	// light shades don't read as digits on white
	}
}

// Night's four reds, brightest first (red only: green and blue are 0, black is the background). Text is only ever the
// two brightest; the darkest is a background under the brightest (the idle pill, the hint row), never a text color.
// Pure red on black reaches 5.2:1 at most, so what must be read at a glance is the brightest on black or black on it
#define NIGHT_R4	RGB565(255, 0, 0)
#define NIGHT_R3	RGB565(176, 0, 0)
#define NIGHT_R2	RGB565(112, 0, 0)
#define NIGHT_R1	RGB565(56, 0, 0)

void ui_theme_set(uint8_t theme)
{
	if (!CONFIG_UI_COLOR || theme >= UI_THEME_COUNT)
		theme = UI_THEME_DARK;	// without color settings: the dark theme only
	ui_theme = theme;
	if (gfx_bpp() == 1)
		return;		// ink on paper whatever the theme
	ui_col.pill_fg = RGB565(255, 255, 255);
	ui_col.pill_dark_fg = RGB565(0, 0, 0);
	if (theme == UI_THEME_NIGHT) {
		// Night: the meanings that were hues become brightness and the word (P5): TX and warnings brightest, RX
		// and privacy one step down, the idle pill dark with a bright word. The highlight is its brightest red too
		// (accent_apply)
		ui_col.bg = RGB565(0, 0, 0);
		ui_col.fg = ui_col.tx = ui_col.warn = NIGHT_R4;
		ui_col.dim = ui_col.rx = ui_col.priv = NIGHT_R3;
		ui_col.line = NIGHT_R2;
		ui_col.idle = ui_col.hint_bg = NIGHT_R1;
		ui_col.pill_fg = RGB565(0, 0, 0);	// on the brightest red
		ui_col.idle_fg = NIGHT_R4;
		accent_apply();
		return;
	}
	if (theme == UI_THEME_LIGHT) {
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
	} else if (theme == UI_THEME_HIGH_CONTRAST) {
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
	ui_col.idle_fg = ui_col.pill_fg;
	accent_apply();
}

uint8_t ui_theme_get(void)
{
	return ui_theme;
}

void ui_accent_set(uint8_t accent)
{
	ui_accent = accent;
	ui_theme_set(ui_theme);
}

void ui_strip_set(bool filled)
{
	ui_strip_fill = filled;
	ui_theme_set(ui_theme);
}

void ui_layout_init(void)
{
	int w = gfx_width(), h = gfx_height();
	ui_layout_t *L = &ui_layout;

	if (gfx_bpp() == 1) {
		ui_col.bg = 0; ui_col.fg = ui_col.dim = ui_col.line = ui_col.tx = ui_col.rx = ui_col.warn = ui_col.idle = ui_col.focus = ui_col.priv = 1;
		ui_col.pill_fg = ui_col.pill_dark_fg = ui_col.idle_fg = 0;
		ui_col.hint_bg = 0;
	}

	ui_theme_set(ui_theme);

	// Display classes (P10): about 128 px wide, about 160 px wide, 240 px and wider. A narrow portrait panel (120x160)
	// keeps the narrow class's small font, so lists and labels keep their 24 characters a line, and has the rows for
	// the larger channel and talk target fonts of the 160 px class
	// Anti-aliased fonts on color panels (CONFIG_FONT_AA, font.h); a mono panel keeps the bitmap ones
	bool aa = gfx_bpp() != 1;
#if CONFIG_FONT_AA && defined(FONT_BOTH)
	aa = aa && font_aa_on;
#endif
	const font_t *f6 = aa ? &font_aa_6x12 : &font_6x12, *f8 = aa ? &font_aa_8x16 : &font_8x16;
	const font_t *f12 = aa ? &font_aa_12x24 : &font_12x24;
	font_largest = aa ? FONT_AA_LARGEST : FONT_LARGEST;
	if (w >= 220) {
		L->tiny = f6; L->small = f8; L->medium = f12; L->large = font_largest;
#if CONFIG_FONT_BIG
		// One size up for lists, labels and the status strip (CONFIG_FONT_BIG): 9 px characters instead of 8
		bool big = aa;
#ifdef FONT_BOTH
		big = big && font_big_on;
#endif
		if (big) {
			L->tiny = f8; L->small = &font_aa_9x18;
		}
#endif
	} else if (w >= 150) {
		L->tiny = &font_5x8; L->small = f6; L->medium = f8; L->large = f12;
	} else if (h >= 150) {
		L->tiny = &font_5x8; L->small = &font_5x8; L->medium = f8; L->large = f12;
	} else {
		L->tiny = &font_5x8; L->small = &font_5x8; L->medium = f6; L->large = f8;
	}

#if CONFIG_LANG_ZH
	// A CJK character is 16 px tall (core/ui/cjk_font.h), so a line that may hold Chinese needs a 16 px font. Where the
	// layout already draws its words in one (the 240 px class), only the smallest font, which text pages use, moves up to
	// it - and only while the language in use is a wide one, so English and Spanish keep their layout in the same build.
	// Where the layout draws its words in the 5x8 or 6x12 font (the 128x64 and 160x128 classes) Chinese cannot be shown
	// at all: there is no room for 16 px lines, so the layout stays as it is and the board must not build the language
	// in (BOARD_UNSUPPORTED, docs/build-config.md)
	if (L->small->h >= CJK_FONT_H && L->tiny->h < CJK_FONT_H && lang_wide(lang_current()))
		L->tiny = L->small;
#endif

	// A pixel between the status strip and what is below it, and a pixel between list rows on displays 100 px and
	// higher (3 on tall ones, 6 in the 240 px class): easy to read comes before one more row
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
	L->row_h = L->small->h + (w >= 220 ? 6 : tall ? 3 : h >= 100 ? 1 : 0);
}

// --- shared pieces ---

const char *ui_power_name(uint8_t p, uint32_t tx_hz)
{
	return board_power_name(p, tx_hz);
}

static uint8_t channel_power(const radio_view_t *v)
{
	return v->ch.power == CODEPLUG_POWER_DEFAULT ? board.power_default : v->ch.power;
}

void ui_bands_text(char *out, size_t len, bool tx)
{
	sbuf_t b;
	sb_init(&b, out, len);
	for (int i = 0; i < board_band_count(); i++) {
		if (tx && !board.bands[i].tx)
			continue;	// a TX refusal lists only where it can transmit, not the receive-only ranges
		if (b.p != out)
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

// The mode word: AM where an FM channel or the VFO is received in AM (the air band); a channel of another kind
// (docs/spec/channel-modes.md) its kind's short name
static const char *mode_word(const radio_view_t *v)
{
	if (v->ch.kind != CODEPLUG_KIND_NONE) {
		return codeplug_kind_name(v->ch.kind);
	}
	return v->ch.mode == RADIO_MODE_FM && v->am ? "AM" : ui_mode_name(v->ch.mode);
}

#if CONFIG_DUAL_WATCH
// The same for a dual watch pane, from the channel picked for it (the receiver may be on the other one)
static const char *dual_mode_word(const ui_dual_pane_t *p)
{
	return p->am ? "AM" : ui_mode_name(p->mode);
}
#endif

// Status strip (P2): state pill, mode, monitor, lock, unread messages, signal, battery.
// A titled screen (prefix and / or name) on a display under UI_TALL_PX high merges its title into the strip
// (wireframes 160x128 and 128x64): the pill shows only while not idle, the signal bars and the battery give way to the
// title (Home shows the battery; the low battery banner warns on any screen). The prefix ("Quick") is kept whole; only
// the name is shortened, from the middle. On tall displays the strip stays whole and the title gets its own row below
// it. Either way the title is centered. Returns the y where content starts.
uint32_t ui_strip_draws;
uint32_t ui_hint_draws;

// The GPS icon: a satellite dish, its rim up and to the right. Hollow while the GPS is off, solid while it searches;
// with a fix the feed arm and two waves join it. The shape, not a color, tells them apart on every panel. Drawn by
// hand at the heights of the strip's signal bars, since a dish drawn from lines muddies below 13 pixels. The width
// keeps the waves' room, so the strip doesn't move when a fix comes
#if CONFIG_GPS
// The rows, bit x of a row is pixel x: each dish's bowl (h rows), then its feed arm and waves from the top row down,
// only as many rows as they reach
static const uint32_t gps_rows[] = {
	// 6 px: bowl, feed, waves
	0x0, 0x1, 0x3, 0x7, 0xf, 0x1e,
	0x20, 0x10, 0x8,
	0x280, 0x500, 0x500, 0x280,
	// 8 px: bowl, feed, waves
	0x0, 0x3, 0x7, 0xf, 0x1f, 0x3f, 0x7e, 0x3c,
	0xc0, 0x20, 0x10,
	0xa00, 0x500, 0xa00, 0xa00, 0x500,
	// 11 px: bowl, feed, waves
	0x0, 0x0, 0x2, 0x7, 0xf, 0x1f, 0x3f, 0x7f, 0xfe, 0xfc, 0x30,
	0x600, 0x300, 0x80, 0x40, 0x20,
	0x9000, 0x12000, 0x12000, 0x12000, 0x9000,
	// 13 px: bowl, feed, waves
	0x0, 0x0, 0x0, 0x2, 0x7, 0xf, 0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3fe, 0x1fc,
	0x0, 0xc00, 0xe00, 0x300, 0x80, 0x40, 0x20,
	0x0, 0x12000, 0x24000, 0x24000, 0x48000, 0x24000, 0x24000, 0x12000,
};
typedef struct {
	uint8_t h, w;		// height; width, the waves' room kept
	uint8_t feed, waves;	// rows of each in gps_rows after the bowl
	uint8_t at;		// the bowl's first row in gps_rows
} gps_glyph_t;
static const gps_glyph_t gps_glyphs[] = {
	{ 6, 11, 3, 4, 0 },
	{ 8, 12, 3, 5, 13 },
	{ 11, 17, 5, 5, 29 },
	{ 13, 19, 7, 8, 50 },
};

// The tallest dish no taller than h, the bars' height, so it keeps the strip's top margin (the smallest below that)
static const gps_glyph_t *gps_glyph(int h)
{
	const gps_glyph_t *g = &gps_glyphs[0];
	for (size_t i = 1; i < sizeof gps_glyphs / sizeof gps_glyphs[0]; i++)
		if (gps_glyphs[i].h <= h)
			g = &gps_glyphs[i];
	return g;
}

// Hollow: only the bowl's edge, the pixels with a side open
static void gps_icon(int x, int y, const gps_glyph_t *g, uint8_t state, color_t fg)
{
	const uint32_t *bowl = &gps_rows[g->at], *feed = bowl + g->h, *waves = feed + g->feed;
	for (int r = 0; r < g->h; r++) {
		uint32_t row = bowl[r];
		if (state == UI_GPS_OFF) {
			uint32_t up = r > 0 ? bowl[r - 1] : 0, down = r + 1 < g->h ? bowl[r + 1] : 0;
			row &= ~(up & down & (row << 1) & (row >> 1));
		}
		if (state == UI_GPS_FIX)
			row |= (r < g->feed ? feed[r] : 0) | (r < g->waves ? waves[r] : 0);
		for (int c = 0; c < g->w; c++)
			if (row & (1u << c))
				gfx_pixel(x + c, y + r, fg);
	}
}
#endif

// A word after the mode, with a space either side, where the strip has room for it: measured, since a translation may
// be longer than the English
static bool strip_fits(const font_t *f, int lx, int rx, const char *word)
{
	return lx + gfx_text_width(f, word) + 2 * f->w <= rx;
}

// The pill shows st instead of v->state: the hotspot screen shows what the hotspot does without a copy of the whole
// view on the UI task's stack (the UI task's deepest path, stack check)
static int draw_top_as(const radio_view_t *v, radio_state_t st, const ui_state_t *s, const char *prefix, const char *name)
{
	ui_strip_draws++;
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	int ph = f->h;		// the pill: one text cell high
	bool titled = prefix != NULL || name != NULL;
	bool merged = titled && gfx_height() < UI_TALL_PX;
	const char *word = TR(TOP_STATE_IDLE, "IDLE");
	color_t pbg = ui_col.idle, pfg = ui_col.idle_fg;
	// FM with the monitor holding the squelch open shows RX, as the green LED does (radio_sm.c outputs()); MON stays
	// beside the mode
	if (st == RS_IDLE && v->monitor && v->ch.mode != RADIO_MODE_DMR)
		st = RS_RX;
#if CONFIG_DUAL_WATCH
	if (st == RS_IDLE && s->dual.on && s->dual.rx != UI_DUAL_NONE)
		st = RS_RX;	// a channel's own receiver (two receivers) hears a call the view's one doesn't show
#endif
	switch (st) {
	case RS_RX: word = "RX"; pbg = ui_col.rx; pfg = ui_col.pill_dark_fg; break;
	case RS_TX: word = v->aprs_tx ? "TX APRS" : "TX"; pbg = ui_col.tx; break;
	case RS_TX_DENIED: word = TR(TOP_STATE_NO_TX, "NO TX"); pbg = ui_col.warn; pfg = ui_col.pill_dark_fg; break;
	default: break;
	}
	if (gfx_bpp() == 1)
		pfg = ui_col.pill_fg;
	else if (pfg == pbg)	// high contrast: the idle pill is white, so its word is black
		pfg = ui_col.pill_dark_fg;
	// A filled strip (Settings > Display) draws on its own background in black, warnings in a dark shade that reads on
	// it (accent_apply); the idle pill turns inverse on it, RX / TX / NO TX keep their colors and words (P5)
	bool filled = CONFIG_UI_COLOR && ui_strip_fill && gfx_bpp() != 1;
	color_t sbg = filled ? ui_col.strip_bg : ui_col.bg, sfg = filled ? ui_col.pill_dark_fg : ui_col.fg;
	color_t swarn = filled ? ui_col.strip_warn : ui_col.warn;
	if (filled && st == RS_IDLE) {
		pbg = sfg;
		pfg = sbg;
	}
	gfx_fill(0, 0, w, L->strip_h, sbg);
	int x = 0;
	if (!merged || st != RS_IDLE) {
		if (filled) {
			// On the filled strip the pill is the strip's full height (a one-cell pill leaves a line of the
			// strip's color under it) against the bezel, its corners rounded by two pixels. Left of its middle the
			// strip is the screen's background, so no strip color shows beside the left end. On the right a black
			// edge follows the curve: RX, TX and NO TX are close in lightness to every strip color (NO TX on orange
			// nearly equal)
			int pad = f->w >= 8 ? 4 : 3, h = L->strip_h;
			int pw = gfx_text_width(f, word) + 2 * pad;
			gfx_fill(0, 0, pw / 2, h, ui_col.bg);
			gfx_fill(2, 0, pw - 4, h, pbg);
			for (int e = 0; e < 2; e++) {	// the rounded ends, and the edge: the two corner steps and the end
				gfx_vline(1 - e, 1 + e, h - 2 - 2 * e, pbg);
				gfx_vline(pw - 2 + e, 1 + e, h - 2 - 2 * e, pbg);
				gfx_pixel(pw - 2 + e, e, sfg);
				gfx_pixel(pw - 2 + e, h - 1 - e, sfg);
			}
			gfx_vline(pw, 2, h - 4, sfg);
			gfx_text(f, pad, (ph - f->h + 1) / 2, word, pfg);
			x = pw + 1 + f->w / 2;
		} else {
			x = gfx_pill(f, 0, 0, ph, word, pfg, pbg) + f->w / 2;
		}
	}
	int ty = (ph - f->h + 1) / 2;

	// Right side, right to left: battery %, battery icon, signal bars, unread messages, lock
	sbuf_t b;
	int rx = w;
	if (!merged) {
		color_t bc = sfg;
		if (!s->no_battery) {
			char pct[6];
			sb_init(&b, pct, sizeof pct);
			sb_u32(&b, s->battery_pct, 1);
			sb_chr(&b, '%');
			rx -= gfx_text_width(f, pct);
			// Settings > Power saving > Low battery warning
			uint8_t low = s->low_batt_pct ? s->low_batt_pct : 15;
			bc = s->battery_pct <= low ? swarn : sfg;
			gfx_text(f, rx, ty, pct, bc);
		}
		int bh = f->ascent - 2 < 5 ? 5 : f->ascent - 2;
		int bw = bh * 2;
		if (!s->no_battery) {
			rx -= bw + 2;
			gfx_icon_battery(rx, ty + f->ascent - bh, bw, bh, s->battery_pct, sfg, bc);
		}
		int sw = bh + 4;
		rx -= sw + f->w / 2;
		gfx_icon_bars(rx, ty + f->ascent - bh - 1, sw, bh + 1, 4, rssi_bars(v->rssi_dbm), sfg, filled ? ui_col.idle : ui_col.line);
#if CONFIG_GPS
		if (s->gps != UI_GPS_NONE) {
			const gps_glyph_t *g = gps_glyph(bh + 1);
			rx -= g->w + f->w / 2;
			gps_icon(rx, ty + f->ascent - g->h, g, s->gps, sfg);
		}
#endif
	}
	// Icons one cell high, drawn from lines: an envelope with the unread count, a padlock while the keys are locked
	int ih = f->ascent - 2 < 5 ? 5 : f->ascent - 2, iy = ty + f->ascent - ih;
	if (s->unread) {
		char m[4];
		sb_init(&b, m, sizeof m);
		sb_u32(&b, s->unread > 9 ? 9 : s->unread, 1);
		rx -= gfx_text_width(f, m) + f->w / 2;
		gfx_text(f, rx, ty, m, swarn);
		int ew = ih * 3 / 2;
		rx -= ew + 1;
		gfx_rect(rx, iy, ew, ih, swarn);
		for (int i = 1; i <= ew / 2; i++) {	// the flap: two diagonals meeting in the middle
			gfx_pixel(rx + i, iy + i * (ih - 2) / (ew / 2), swarn);
			gfx_pixel(rx + ew - 1 - i, iy + i * (ih - 2) / (ew / 2), swarn);
		}
	}
	if (s->locked) {
		int lw = ih - ih / 4;
		rx -= lw + f->w / 2;
		gfx_fill(rx, iy + ih / 2, lw, ih - ih / 2, swarn);		// body
		gfx_rect(rx + 1, iy, lw - 2, ih / 2 + 1, swarn);		// shackle
	}

	if (merged) {
		color_t dim = ui_col.dim;
		if (filled)
			ui_col.dim = sfg;	// the title's prefix, normally dim, in the strip's one text color
		ui_draw_title(f, x, ty, (rx < w ? rx - f->w / 2 : w) - x, prefix, name, sfg);
		ui_col.dim = dim;
		return L->strip_h;
	}
	// Left side: the mode, and one mode word when there is room for it with a space either side. In dual watch the
	// mode is the selected channel's (the tuned one alternates) and the word is DUAL
	const char *mw = mode_word(v);
#if CONFIG_DUAL_WATCH
	if (s->dual.on && !v->aprs_tx)
		mw = dual_mode_word(&s->dual.pane[s->dual.sel]);
#endif
	if (v->aprs_tx)
		mw = "FM";
	int lx = gfx_text(f, x, ty, mw, sfg);
#if CONFIG_DUAL_WATCH
	if (s->dual.on && !v->aprs_tx && strip_fits(f, lx, rx, TR(TOP_CHIP_DUAL, "DUAL")))
		lx = gfx_text(f, lx + f->w, ty, TR(TOP_CHIP_DUAL, "DUAL"), swarn);
	else
#endif
	if (s->scanning && strip_fits(f, lx, rx, TR(TOP_CHIP_SCAN, "SCAN")))
		lx = gfx_text(f, lx + f->w, ty, TR(TOP_CHIP_SCAN, "SCAN"), swarn);
#if CONFIG_DMR_ROAMING
	else if (v->roam.state == RROAM_STATE_SEARCH && strip_fits(f, lx, rx, TR(TOP_CHIP_ROAM, "ROAM")))
		lx = gfx_text(f, lx + f->w, ty, TR(TOP_CHIP_ROAM, "ROAM"), swarn);	// searching the zone's sites
#endif
	else if (v->monitor && !v->aprs_tx && strip_fits(f, lx, rx, "MON"))
		lx = gfx_text(f, lx + f->w, ty, "MON", swarn);
#if CONFIG_USB_NETWORK && CONFIG_HOSTLINK
	// USB as a network adapter (Tools > USB network): the serial tools are off. IP, and once the PC has the link up an
	// up and a down arrow right after it, as a phone shows its data link. The arrows stand close, each head beside the
	// other's shaft, so the shafts stop short of the other arrow's head: 3 * hw + 3 columns, at least 6 rows (on the
	// smallest font a shorter shaft reads as a cross). Room as for the other words, a space either side
	else if (s->usb_net != UI_USB_NET_OFF) {
		int hw = f->w >= 8 ? f->w / 4 : 1;	// a barb's width
		int aw = s->usb_net == UI_USB_NET_UP ? 3 * hw + 3 : 0;	// the arrows; the glyph cell's own margin before them
		if (lx + gfx_text_width(f, "IP") + aw + 2 * f->w <= rx) {
			lx = gfx_text(f, lx + f->w, ty, "IP", swarn);
			if (aw) {
				int ah = ih < 6 ? 6 : ih, ay = iy + ih - ah;	// the arrows' height and top, on the icons' baseline
				int ax = lx + hw, bx = ax + hw + 2, sh = ah - hw - 1;	// the shafts, up and down; shaft length
				gfx_vline(ax, ay, sh, swarn);
				gfx_vline(bx, ay + ah - sh, sh, swarn);
				for (int i = 1; i <= hw; i++) {
					gfx_pixel(ax - i, ay + i, swarn);
					gfx_pixel(ax + i, ay + i, swarn);
					gfx_pixel(bx - i, ay + ah - 1 - i, swarn);
					gfx_pixel(bx + i, ay + ah - 1 - i, swarn);
				}
				lx += aw;
			}
		}
	}
#endif
	if (!titled)
		return L->strip_h;
	// Tall display: the title on its own row, underlined
	int y = L->strip_h;
	gfx_fill(0, y, w, f->h + 2, ui_col.bg);
	ui_draw_title(f, 0, y + 1, w, prefix, name, ui_col.fg);
	gfx_hline(0, y + f->h + 1, w, ui_col.line);
	return y + f->h + 2;
}

int ui_draw_top(const radio_view_t *v, const ui_state_t *s, const char *prefix, const char *name)
{
	return draw_top_as(v, v->state, s, prefix, name);
}

// The prefix (dim when a name follows) and the name, centered on the screen and moved as little as needed to stay
// within x .. x + max_w (the pill, the lock and the unread count share the merged strip). Both are fitted first, so a
// shortened title fills its space
void ui_draw_title(const font_t *f, int x, int y, int max_w, const char *prefix, const char *name, color_t c)
{
	char pfit[48] = "", nfit[48] = "";
	int pw = 0, gap = 0, nw = 0, room = max_w;
	if (prefix != NULL) {
		gfx_text_fit(f, prefix, room, pfit, sizeof pfit);
		pw = gfx_text_width(f, pfit);
		room -= pw + f->w;
		if (room < 3 * f->w)
			name = NULL;	// no room for the name after the prefix
		else if (name != NULL)
			gap = f->w;
	}
	if (name != NULL) {
		gfx_text_fit(f, name, room, nfit, sizeof nfit);
		nw = gfx_text_width(f, nfit);
	}
	int tw = pw + gap + nw;
	int tx = (gfx_width() - tw) / 2;
	if (tx + tw > x + max_w)
		tx = x + max_w - tw;
	if (tx < x)
		tx = x;
	if (prefix != NULL)
		tx = gfx_text(f, tx, y, pfit, name != NULL ? ui_col.dim : c) + gap;
	if (name != NULL)
		gfx_text(f, tx, y, nfit, c);
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

// Split a hint into the Back group (it starts with the Back key's name, "Back" or its translation) and the rest,
// joined by two spaces
static void hint_split(const char *text, char *left, size_t lcap, char *right, size_t rcap)
{
	size_t ln = 0, rn = 0;
	const char *back = TR(HINT_BACK, "Back");
	size_t bn = strlen(back);
	right[0] = 0;
	for (const char *g = text; *g;) {
		const char *end = strstr(g, "  ");
		size_t n = end != NULL ? (size_t)(end - g) : strlen(g);
		if (rn == 0 && n >= bn && strncmp(g, back, bn) == 0 && n < rcap) {
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

static bool alt_mark(char c)
{
	return c == TEXT_ALT_OPEN[0] || c == TEXT_ALT_NEXT[0] || c == TEXT_ALT_CLOSE[0];
}

static bool board_has_digits(void)
{
	for (uint8_t i = 0; board.keymap != NULL && i < board.keymap_len; i++) {
		if (board.keymap[i].short_action >= ACT_DIGIT_0 && board.keymap[i].short_action <= ACT_DIGIT_9)
			return true;
	}
	return false;
}

// Hints and toasts are written for a keypad: "*" and "#" name the QUICK and CONTEXT keys, and a hint group may start
// with a digit ("0-9 find"). On a radio without them (ui.md 1) a "*" or "#" standing alone becomes the board's own
// key name (board.quick_label / context_label: "P1 Quick"), and in a hint (not a toast, whose numbers are values) a
// group that starts with a digit goes, so no hint names a key the radio doesn't have. Text for a keypad radio comes
// back unchanged
static const char *key_names(const char *text, char *out, size_t cap, bool hint)
{
	bool digits = board_has_digits() || !hint;
	if (text == NULL || cap == 0 || (digits && board.quick_label == NULL && board.context_label == NULL))
		return text;
	size_t n = 0;
	for (const char *p = text; *p && n + 1 < cap; p++) {
		bool start = p == text || alt_mark(p[-1]) || (p - text >= 2 && p[-1] == ' ' && p[-2] == ' ');
		if (start && !digits && *p >= '0' && *p <= '9') {
			// Drop the group and one of the two-space separators around it
			while (p[1] && !alt_mark(p[1]) && !(p[1] == ' ' && p[2] == ' '))
				p++;
			if (n >= 2 && out[n - 1] == ' ' && out[n - 2] == ' ')
				n -= 2;
			else if (p[1] == ' ' && p[2] == ' ')
				p += 2;
			continue;
		}
		bool alone = (p == text || p[-1] == ' ' || alt_mark(p[-1])) && (p[1] == 0 || p[1] == ' ' || alt_mark(p[1]));
		const char *sub = !alone ? NULL : *p == '*' ? board.quick_label : *p == '#' ? board.context_label : NULL;
		if (sub == NULL) {
			out[n++] = *p;
			continue;
		}
		while (*sub && n + 1 < cap)
			out[n++] = *sub++;
	}
	out[n] = 0;
	return out;
}

// The hint row. Hints are groups separated by two spaces ("OK open  # more  Back"); the group for the Back key
// ("Back", "Back keeps", ...) is pinned to the right edge, under the key, and the others run from the left. A hint may
// hold text alternates (font.h): the longest form that fits is used. A toast is centered; one too long for the row
// takes a second line above it, so it is never shortened
void ui_draw_hint(const char *text, bool toast)
{
#if CONFIG_VOLUME_BAR
	ui_hint_draws++;
#endif
	// The key names go into each alternate once it is picked, in buffers this function has anyway: no static (RAM is
	// tight on some radios) and no more stack (the UI task's deepest chain runs through here, docs/dev/stack-check.md)
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	char fit[64];
	color_t bg = toast ? ui_col.fg : ui_col.hint_bg;
	color_t fg = toast ? ui_col.bg : ui_col.dim;
	if (gfx_bpp() == 1 && !toast)
		fg = ui_col.fg;
	if (toast) {
		char first[64], second[64];	// second holds the raw alternate until the key names are in first
		const char *line = first;	// the alternate with the key names: first, or second when there were none
		bool more = true;
		// One line in the longest alternate that fits it, else two lines of the first
		for (int level = 0; more; level++) {
			gfx_text_alt(text, level, second, sizeof second, &more);
			line = key_names(second, first, sizeof first, false);
			if (gfx_text_width(f, line) <= w - 2)
				break;
		}
		if (gfx_text_width(f, line) > w - 2) {
			gfx_text_alt(text, 0, second, sizeof second, &more);
			line = key_names(second, first, sizeof first, false);
		}
		if (line != first) {
			memcpy(first, second, sizeof first);	// the rest below may point into it, and second is reused
		}
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
		hint_split(key_names(form, fit, sizeof fit, true), left, sizeof left, right, sizeof right);
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

// The mark: a hexagon with a skyline behind a handheld radio, signal arcs either side of its antenna. It is drawn
// per pixel from shapes laid out on a 1024 grid (hex radius 482, center 512,512), so it scales to every display class
// with no bitmap. Six flat colors fit the 16-entry palette of a 4 bpp panel. On a 1 bpp panel the hexagon is solid ink
// with the radio and arcs cut out of it, and a small mark has one arc a side, so the strokes stay a pixel or more

static const color_t ORANGE = RGB565(242, 96, 31), SKY = RGB565(203, 74, 24), CREAM = RGB565(255, 243, 220),
	BROWN = RGB565(59, 23, 9), AMBER = RGB565(255, 179, 71), PEACH = RGB565(255, 217, 168);

static bool in_rrect(int px, int py, int x, int y, int w, int h, int r)
{
	if (px < x || px >= x + w || py < y || py >= y + h)
		return false;
	int cx = px < x + r ? x + r : px >= x + w - r ? x + w - r - 1 : px;
	int cy = py < y + r ? y + r : py >= y + h - r ? y + h - r - 1 : py;
	return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r;
}

// The color at one point of the 1024 grid, or -1 outside the hexagon
static int mark_color(int X, int Y, bool mono, bool small)
{
	color_t ink = ui_col.fg, paper = ui_col.bg;
	int ax = X > 512 ? X - 512 : 512 - X, ay = Y > 512 ? Y - 512 : 512 - Y;
	if (ay * 1000 + ax * 577 > 482000 || ax > 417)
		return -1;
	// Radio: antenna and side knob, body, display, speaker slits and the key
	color_t body = mono ? paper : CREAM, det = mono ? ink : BROWN, scr = mono ? ink : AMBER;
	if (in_rrect(X, Y, 434, 193, 37, 311, 18) || in_rrect(X, Y, 553, 441, 46, 74, 14) || in_rrect(X, Y, 397, 482, 230, 370, 46)) {
		if (in_rrect(X, Y, 448, 532, 129, 63, 7))
			return scr;
		if (in_rrect(X, Y, 436, 519, 152, 89, 11))
			return det;
		for (int i = 0; i < 3; i++)
			if (in_rrect(X, Y, 436, 652 + 37 * i, 152, 15, 7))
				return det;
		if ((X - 512) * (X - 512) + (Y - 800) * (Y - 800) <= 23 * 23)
			return det;
		return body;
	}
	// Signal arcs, within 42 degrees of horizontal either side of the antenna, centered 452,286
	static const short ring[2][3][2] = { { { 76, 22 }, { 122, 20 }, { 168, 18 } }, { { 120, 60 }, { 0, 0 }, { 0, 0 } } };
	int dx = X - 452, dy = Y - 286, d2 = dx * dx + dy * dy, ady = dy < 0 ? -dy : dy, adx = dx < 0 ? -dx : dx;
	if (ady * 100 <= adx * 90) {
		for (int i = 0; i < 3; i++) {
			int r = ring[small][i][0], hw = ring[small][i][1] / 2;
			if (r && d2 >= (r - hw) * (r - hw) && d2 <= (r + hw) * (r + hw))
				return mono ? paper : PEACH;
		}
	}
	if (mono)
		return ink;
	// Skyline: a sawtooth roof, two chimneys, in a darker orange than the hexagon
	if (X >= 30 && X < 994) {
		int roof = 490 - 120 * ((X - 30) % 241) / 241;
		if (Y >= roof || (X >= 678 && X < 762 && Y >= 230 && Y < 260) || (X >= 690 && X < 750 && Y >= 250) ||
		    (X >= 223 && X < 267 && Y >= 270))
			return SKY;
	}
	return ORANGE;
}

// r = the hexagon's radius in pixels (the mark is 1.73 r wide and 2 r tall), centered on cx, cy
static void draw_mark(int cx, int cy, int r)
{
	bool mono = gfx_bpp() == 1, small = mono && r < 26;
	for (int dy = -r; dy < r; dy++) {
		for (int dx = -r; dx < r; dx++) {
			int c = mark_color(512 + (2 * dx + 1) * 241 / r, 512 + (2 * dy + 1) * 241 / r, mono, small);
			if (c >= 0)
				gfx_pixel(cx + dx, cy + dy, (color_t)c);
		}
	}
}

// The wordmark: "Radio" in the accent color, "Foundry" in the text color, centered; returns its height
static int draw_wordmark(const font_t *f, int y)
{
	int w1 = gfx_text_width(f, "Radio") + 1, w = w1 + gfx_text_width(f, "Foundry");
	int x = (gfx_width() - w) / 2;
	gfx_text(f, x, y, "Radio", gfx_bpp() == 1 ? ui_col.fg : AMBER);
	gfx_text(f, x + w1, y, "Foundry", ui_col.fg);
	return f->h;
}

void ui_draw_splash(const char *board_name, const char *version, const char *line1, const char *line2)
{
	int w = gfx_width(), h = gfx_height();
	const ui_layout_t *L = &ui_layout;
	gfx_clear(ui_col.bg);

	const char *word = "RadioFoundry";
	const font_t *wf = gfx_text_width(L->large, word) <= w - 4 ? L->large :
		gfx_text_width(L->medium, word) <= w - 4 ? L->medium : L->small;
	// The owner's lines (the CPS boot text): as large as fits, never dropped
	const char *lines[2] = { line1 != NULL ? line1 : "", line2 != NULL ? line2 : "" };
	int n_lines = (lines[0][0] != '\0') + (lines[1][0] != '\0');
	const font_t *lf = L->medium;
	for (int i = 0; i < 2; i++)
		if (gfx_text_width(lf, lines[i]) > w - 2)
			lf = L->small;

	// Footer: board and version, small and dim along the bottom. Too wide (a beta's name on a 128 px display): one
	// space between them, then the version alone
	const font_t *ff = L->tiny;
	char foot[48];
	static const char *const seps[] = { "  ", " ", NULL };
	for (int i = 0; i < 3; i++) {
		sbuf_t b;
		sb_init(&b, foot, sizeof foot);
		if (seps[i] != NULL) {
			sb_str(&b, board_name);
			sb_str(&b, seps[i]);
		}
		sb_str(&b, version);
		if (gfx_text_width(ff, foot) <= w)
			break;
	}
	int foot_y = h - ff->h;

	// Stack: mark, wordmark, rule, lines; centered in the space above the footer. The mark takes what is left
	int gap = h >= 200 ? 8 : h >= 100 ? 4 : 2;
	int text_h = wf->h + gap + (n_lines > 0 ? gap + n_lines * lf->h : 0);
	int avail = foot_y - gap - text_h - gap;
	int r = avail / 2;				// the mark is 2 r tall
	if (r > w * 3 / 10)
		r = w * 3 / 10;
	if (r > h * 3 / 10)
		r = h * 3 / 10;
	bool mark = r >= 20;		// smaller than this the radio is a smudge
	// Too short for the mark above the name (128x64): the mark left of the name instead, at the smallest size that
	// stays clear, with the name in the largest font that leaves it room. The owner's lines keep their size
	int side_r = 0, side_w = 0;
	if (!mark) {
		// The largest mark (14 px radius down to 10) whose row, with the name beside it, fits above the footer
		for (int sr = 14; sr >= 10 && side_r == 0; sr--) {
			int sw = sr * 173 / 100 + 4;
			const font_t *sf = gfx_text_width(L->medium, word) <= w - 4 - sw ? L->medium :
				gfx_text_width(L->small, word) <= w - 4 - sw ? L->small : NULL;
			int row_h = 2 * sr > (sf != NULL ? sf->h : 0) ? 2 * sr : (sf != NULL ? sf->h : 0);
			if (sf != NULL && text_h - wf->h + row_h <= foot_y - gap) {
				text_h += row_h - wf->h;
				side_r = sr;
				side_w = sw;
				wf = sf;
			}
		}
	}
	int mark_h = mark ? 2 * r : 0;
	int total = mark_h + (mark ? gap : 0) + text_h;
	int y = (foot_y - total) / 2;
	if (y < 0)
		y = 0;
	if (mark) {
		draw_mark(w / 2, y + r, r);
		y += mark_h + gap;
	}
	if (side_r > 0) {
		int sw = side_w, ww = gfx_text_width(wf, word);
		int x0 = (w - sw - ww) / 2, row_h = 2 * side_r > wf->h ? 2 * side_r : wf->h;
		draw_mark(x0 + (sw - 4) / 2, y + row_h / 2, side_r);
		gfx_text(wf, x0 + sw, y + (row_h - wf->h) / 2, "Radio", gfx_bpp() == 1 ? ui_col.fg : AMBER);
		gfx_text(wf, x0 + sw + gfx_text_width(wf, "Radio") + 1, y + (row_h - wf->h) / 2, "Foundry", ui_col.fg);
		y += row_h;
	} else
		y += draw_wordmark(wf, y);
	gfx_hline(w / 2 - w / 6, y + gap / 2, w / 3, gfx_bpp() == 1 ? ui_col.fg : ORANGE);
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

// The frequency being typed on Home: MHz digits, then decimals, with blanks for the next digits.
static void home_entry_text(const ui_state_t *s, char *buf, size_t len)
{
	sbuf_t b;
	sb_init(&b, buf, len);
	int dec = s->entry_len > UI_ENTRY_MHZ ? s->entry_len - UI_ENTRY_MHZ : 0;
	int show = dec < 3 ? 3 : dec;
	for (int i = 0; i < UI_ENTRY_MHZ + show; i++) {
		if (i == UI_ENTRY_MHZ)
			sb_chr(&b, '.');
		sb_chr(&b, i < s->entry_len ? s->entry_digits[i] : '_');
	}
}

#if CONFIG_DMR
static void draw_talk_dmr(const radio_view_t *v, bool label_first);
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
			sb_str(&b, TR(HOME_LABEL_STEP, "Step "));
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
		const char *left = v->vfo ? "VFO" : v->zone_name;
		if (v->vfo && v->ch.mode == RADIO_MODE_DMR && !v->ch.rx_only && v->ch.tx_hz != v->ch.rx_hz) {
			// A split VFO on DMR: the talk row names the target, so the TX frequency goes here (FM's talk row
			// already shows it). A frequency is never cut short: the step gives way, then "VFO"
			sb_init(&b, buf, sizeof buf);
			sb_str(&b, "VFO  TX ");
			sb_freq(&b, v->ch.tx_hz);
			left = buf;
			if (gfx_text_width(L->small, buf) + L->small->w + rw > w)
				rw = 0;
			if (gfx_text_width(L->small, buf) > w)
				left = buf + 5;	// "TX 433.5375"
		}
		if (rw)
			gfx_text(L->small, w - rw, L->zone_y, right, ui_col.dim);
		gfx_text_fit(L->small, left, w - rw - L->small->w, fit, sizeof fit);
		gfx_text(L->small, 0, L->zone_y, fit, v->vfo ? ui_col.warn : ui_col.fg);
	}

	int y = L->channel_y;
	int base = y + L->large->ascent;
	if (v->vfo || s->entry) {
		// The frequency large, the TX frequency in red while transmitting; while typing, the digits so far and
		// blanks for the rest
		bool tx = v->state == RS_TX && v->vfo;
		sb_init(&b, buf, sizeof buf);
		if (s->entry)
			home_entry_text(s, buf, sizeof buf);
		else
			sb_freq(&b, tx ? v->ch.tx_hz : v->ch.rx_hz);
		int x = gfx_text(L->large, 0, y, buf, s->entry ? ui_col.warn : tx ? ui_col.tx : ui_col.fg);
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
	int x = gfx_text(L->medium, 0, base - L->medium->ascent, buf, gfx_bpp() == 1 ? ui_col.dim : ui_col.chnum) +
		L->medium->w / 2;
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

static void draw_talk(const radio_view_t *v, bool label_first)
{
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR) {
		draw_talk_dmr(v, label_first);
		return;
	}
#else
	(void)label_first;
#endif
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->medium;
	char buf[40];
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.fg;
	if (v->ch.mode == RADIO_MODE_DMR) {
		sb_str(&b, TR(WHY_DMR_NOT_IN_BUILD, "DMR: not in this build"));
		c = ui_col.warn;
#if CONFIG_P25_TRUNK
	} else if (v->ch.mode == RADIO_MODE_P25 && v->ch.kind == CODEPLUG_KIND_P25_TRUNK &&
		   v->p25.trunk != RADIO_P25_TRUNK_OFF) {
		// What the follower does, and with the transmit side on, how far it is from talking: registering, the
		// talk group once PTT can ask for it, a request's state
		bool txs = (v->p25.flags & RADIO_P25_TRUNK_TX) != 0;
		if (v->p25.trunk == RADIO_P25_TRUNK_HUNT) {
			sb_str(&b, TR(HOME_STATE_P25T_HUNT, "Finding system"));
			c = ui_col.dim;
		} else if (v->p25.trunk == RADIO_P25_TRUNK_CONTROL && txs && v->p25.treq == P25_REQ_WAIT) {
			sb_str(&b, TR(HOME_STATE_P25T_WAIT, "Wait"));
		} else if (v->p25.trunk == RADIO_P25_TRUNK_CONTROL && txs && v->p25.treq == P25_REQ_QUEUED) {
			sb_str(&b, TR(HOME_STATE_P25T_QUEUED, "Queued"));
		} else if (v->p25.trunk == RADIO_P25_TRUNK_CONTROL && txs && v->p25.treq == P25_REQ_DENIED) {
			sb_str(&b, TR(HOME_STATE_P25T_DENIED, "Denied"));
			c = ui_col.warn;
		} else if (v->p25.trunk == RADIO_P25_TRUNK_CONTROL && txs && v->p25.treg == P25_REG_DENIED) {
			sb_str(&b, TR(HOME_STATE_P25T_REG_DENIED, "Reg. denied"));
			c = ui_col.warn;
		} else if (v->p25.trunk == RADIO_P25_TRUNK_CONTROL && txs && v->p25.treg != P25_REG_READY &&
			   v->p25.treg != P25_REG_OFF && v->p25.treg != P25_REG_NO_TX) {
			sb_str(&b, TR(HOME_STATE_P25T_REG, "Registering"));
			c = ui_col.dim;
		} else if (v->p25.trunk == RADIO_P25_TRUNK_CONTROL && txs && v->p25.treg != P25_REG_NO_TX) {
			sb_str(&b, "TG ");
			sb_u32(&b, v->p25.talk, 1);
		} else if (v->p25.trunk == RADIO_P25_TRUNK_CONTROL) {
			sb_str(&b, TR(HOME_STATE_P25T_CONTROL, "Control channel"));
		} else if (v->p25.treq == P25_REQ_TALK) {
			sb_str(&b, "TG ");
			sb_u32(&b, v->p25.talk, 1);
		} else {
			sb_str(&b, TR(HOME_STATE_P25T_FOLLOW, "Following"));
			sb_str(&b, (v->p25.flags & RADIO_P25_UNIT) != 0 ? " ID " : " TG ");
			sb_u32(&b, v->p25.dst, 1);
		}
#endif
	} else if (v->ch.rx_only) {
		sb_str(&b, TR(WORD_RX_ONLY, "RX only"));
		c = ui_col.warn;
	} else if (v->ch.tx_hz == 0) {
		sb_str(&b, TR(HOME_STATE_NO_TX_FREQ, "No TX frequency"));
		c = ui_col.warn;
#if CONFIG_P25
	} else if (v->ch.mode == RADIO_MODE_P25) {
		// What PTT calls, as on DMR's talk line: "TG 1", or the unit's ID
		if ((v->p25.flags & RADIO_P25_TALK_UNIT) == 0)
			sb_str(&b, "TG ");
		sb_u32(&b, v->p25.talk, 1);
#endif
#if CONFIG_NXDN
	} else if (v->ch.mode == RADIO_MODE_NXDN) {
		// The same for NXDN: the group, or the unit's ID
		if ((v->nxdn.flags & RADIO_NXDN_TALK_UNIT) == 0)
			sb_str(&b, "TG ");
		sb_u32(&b, v->nxdn.talk, 1);
#endif
	} else {
		// " MHz" goes before the frequency is cut (font.h alternates)
		sb_str(&b, TEXT_ALT_OPEN);
		sb_freq(&b, v->ch.tx_hz);
		sb_str(&b, " MHz" TEXT_ALT_NEXT);
		sb_freq(&b, v->ch.tx_hz);
		sb_str(&b, TEXT_ALT_CLOSE);
	}
	// Too wide: shorter text, never a smaller font
	int x = gfx_text(f, 0, L->talk_y, GLYPH_ARROW, ui_col.dim);	// no space, on purpose: as on DMR's talk line
	char fit[40];
	gfx_text_fit(f, buf, gfx_width() - x, fit, sizeof fit);
	gfx_text(f, x, L->talk_y, fit, c);
}

// Chips: tone, bandwidth, power, squelch. A dot marks a temporary change
#if CONFIG_DMR_TRUNK
static void draw_chips_t3(const radio_view_t *v);
#endif

static void draw_chips(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	char buf[48];
	sbuf_t b;
	int x = 0, y = L->chips_y;

#if CONFIG_DMR_TRUNK
	if (v->t3.sys != 0) {
		draw_chips_t3(v);
		return;
	}
#endif
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR) {
		draw_chips_dmr(v, s);
		return;
	}
#endif
	if (v->am || v->bcast) {
		// No tone, width or power here: the receiver is fixed. The air band squelch is real, the broadcast one isn't used
		x = gfx_text(f, x, y, v->am ? TR(HOME_CHIP_AIR_BAND_AM, "Air Band AM") :
			TR(HOME_CHIP_BROADCAST_FM, "Broadcast FM"), ui_col.fg) + f->w;
		if (v->am) {
			sb_init(&b, buf, sizeof buf);
			sb_str(&b, "SQ");
			sb_u32(&b, v->squelch, 1);
			if (x + gfx_text_width(f, buf) <= w)
				gfx_text(f, x, y, buf, ui_col.fg);
		}
		return;
	}
	bool pw_temp = v->power != channel_power(v);
	if (v->ch.mode == RADIO_MODE_P25) {
		// No sub-audio tone, always 12.5 kHz, no FM squelch: the power, then the NAC heard (F7E: any)
		if (pw_temp)
			x = gfx_text(f, x, y, GLYPH_DOT, ui_col.warn);
		x = gfx_text(f, x, y, ui_power_name(v->power, v->ch.tx_hz), pw_temp ? ui_col.warn : ui_col.fg) + f->w;
#if CONFIG_P25
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "NAC ");
		sb_hex(&b, v->p25.rx_nac, 3);
		if (x + gfx_text_width(f, buf) <= w)
			gfx_text(f, x, y, buf, ui_col.fg);
#endif
		return;
	}
	if (v->ch.mode == RADIO_MODE_NXDN) {
		// The same for NXDN: the power, the RAN sent and the rate
		if (pw_temp)
			x = gfx_text(f, x, y, GLYPH_DOT, ui_col.warn);
		x = gfx_text(f, x, y, ui_power_name(v->power, v->ch.tx_hz), pw_temp ? ui_col.warn : ui_col.fg) + f->w;
#if CONFIG_NXDN
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "RAN ");
		sb_u32(&b, v->nxdn.tx_ran, 1);
		sb_str(&b, (v->nxdn.flags & RADIO_NXDN_9600) != 0 ? " 9600" : " 4800");
		if (x + gfx_text_width(f, buf) <= w)
			gfx_text(f, x, y, buf, ui_col.fg);
#endif
		return;
	}
	sb_init(&b, buf, sizeof buf);
	bool tone_temp = (v->temp & (RADIO_TEMP_TX_TONE | RADIO_TEMP_RX_TONE)) != 0;
	if (tone_temp)
		x = gfx_text(f, x, y, GLYPH_DOT, ui_col.warn);
	if (v->ch.tx_tone.type == TONE_NONE && v->ch.rx_tone.type == TONE_NONE) {
		sb_str(&b, TR(CHIP_NO_TONE, "No tone"));
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
	if (pw_temp)
		x = gfx_text(f, x, y, GLYPH_DOT, ui_col.warn);
	x = gfx_text(f, x, y, ui_power_name(v->power, v->ch.tx_hz), pw_temp ? ui_col.warn : ui_col.fg) + f->w;
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

// A hardware option the radio still needs from a person (board_hwopt_pending, hal_hwopt.h): what to do, on the home
// screen and when PTT is refused
static void hwopt_needed(sbuf_t *b)
{
	const hal_hwopt_t *o = board_hwopt_pending();
	if (o == NULL) {
		return;
	}
	if (hwopt_state(o) == HWOPT_ST_RESTART) {
		sb_str(b, TR(HW_BODY_RESTART_TO_APPLY, "Restart to apply"));
		return;
	}
	sb_str(b, TR(HW_BODY_SET, "Set: "));
	sb_str(b, o->name);
}

static const char *hwopt_action(void)
{
	const hal_hwopt_t *o = board_hwopt_pending();
	if (o != NULL && hwopt_state(o) == HWOPT_ST_RESTART) {
		return TR(HW_FIX_POWER_CYCLE, "Turn the radio off and on");
	}
	return TR(HW_FIX_SETTINGS_PATH, "Settings>Advanced>Hardware");
}

#if CONFIG_P25_TRUNK || CONFIG_DMR_TRUNK
// RADIO_TRUNK_WHY_* of a refused trunked PTT: the P25 follower's or the Tier III system's
static uint8_t trunk_why(const radio_view_t *v)
{
#if CONFIG_DMR_TRUNK
	if (v->t3.on)
		return v->t3.why;
#endif
#if CONFIG_P25_TRUNK
	return v->p25.twhy;
#else
	return RADIO_TRUNK_WHY_NONE;
#endif
}
#endif

static const char *deny_reason(const radio_view_t *v, char *buf, size_t len)
{
	sbuf_t b;
	sb_init(&b, buf, len);
	switch (v->tx_deny) {
	case TXDENY_RX_ONLY: sb_str(&b, TR(DENY_WHY_RX_ONLY, "This channel is RX only")); break;
	case TXDENY_BAND: {
		char bands[32];
		ui_bands_text(bands, sizeof bands, true);
		sb_str(&b, TR(DENY_WHY_BANDS, "Bands "));
		sb_str(&b, bands);
		break;
	}
	case TXDENY_TIMEOUT: sb_str(&b, TR(DENY_WHY_TIMEOUT, "TX time-out reached")); break;
	case TXDENY_CEASED: sb_str(&b, TR(DENY_WHY_SYSTEM_STOPPED, "System stopped TX")); break;
	case TXDENY_NO_FREQ: sb_str(&b, TR(DENY_WHY_NO_TX_FREQ, "No TX frequency set")); break;
	case TXDENY_MODE: sb_str(&b, TR(WHY_MODE_NOT_IN_BUILD, "Mode: not in this build")); break;
	case TXDENY_NO_CODEC: sb_str(&b, TR(WHY_NO_CODEC, "No codec: voice off")); break;
	case TXDENY_NO_DMR: sb_str(&b, TR(DENY_WHY_MODEM_NOT_RUNNING, "DMR modem not running")); break;
	case TXDENY_BUSY: sb_str(&b, TR(DENY_WHY_CHANNEL_BUSY, "Channel busy")); break;
	case TXDENY_NO_REPEATER: sb_str(&b, TR(DENY_WHY_NO_REPEATER, "No repeater answered")); break;
	case TXDENY_REJECTED: sb_str(&b, TR(DENY_WHY_MODEM_REFUSED, "Modem refused to send")); break;
	case TXDENY_HOTSPOT: sb_str(&b, TR(DENY_WHY_HOTSPOT_MODE, "Hotspot mode: PTT off")); break;
	case TXDENY_NO_ID: sb_str(&b, TR(DENY_WHY_NO_DMR_ID, "No DMR ID in codeplug")); break;
	case TXDENY_AM: sb_str(&b, TR(WHY_AIR_BAND_RX_ONLY, "Air band: receive only")); break;
	case TXDENY_BCAST: sb_str(&b, TR(WHY_BROADCAST_RX_ONLY, "Broadcast: receive only")); break;
	case TXDENY_SFR_RPT: sb_str(&b, TR(DENY_WHY_REPEATER_CHANNEL, "Repeater channel: PTT off")); break;
	case TXDENY_HWOPT: hwopt_needed(&b); break;
	case TXDENY_REFUSED: sb_str(&b, TR(DENY_WHY_RADIO_REFUSED, "Radio refused to send")); break;
	case TXDENY_BENCH: sb_str(&b, TR(DENY_WHY_AIR_BENCH, "Air bench: PTT off")); break;
	case TXDENY_AIR_NO_TX: sb_str(&b, TR(DENY_WHY_AIR_NO_TX, "This mode: receive only")); break;
#if CONFIG_P25_TRUNK || CONFIG_DMR_TRUNK
	case TXDENY_TRUNK:
		switch (trunk_why(v)) {
		case RADIO_TRUNK_WHY_NO_CC: sb_str(&b, TR(DENY_WHY_P25T_NO_CC, "No control channel")); break;
		case RADIO_TRUNK_WHY_NOT_REG: sb_str(&b, TR(DENY_WHY_P25T_NOT_REG, "Not registered yet")); break;
		case RADIO_TRUNK_WHY_DENIED: sb_str(&b, TR(DENY_WHY_P25T_DENIED, "System denied the call")); break;
		case RADIO_TRUNK_WHY_NO_ANSWER: sb_str(&b, TR(DENY_WHY_P25T_NO_ANSWER, "No answer from system")); break;
		case RADIO_TRUNK_WHY_NO_CHAN: sb_str(&b, TR(DENY_WHY_P25T_NO_CHAN, "Voice channel unusable")); break;
		case RADIO_TRUNK_WHY_OTHER_SYS: sb_str(&b, TR(DENY_WHY_TRUNK_OTHER_SYS, "Another system heard")); break;
		case RADIO_TRUNK_WHY_OTHER_KIND: sb_str(&b, TR(DENY_WHY_TRUNK_OTHER_KIND, "Incompatible system")); break;
		case RADIO_TRUNK_WHY_REG_REFUSED: sb_str(&b, TR(DENY_WHY_TRUNK_REG_REFUSED, "Registration refused")); break;
#if CONFIG_DMR_TRUNK
		case RADIO_TRUNK_WHY_UNMAPPED:
			// The channel number the grant named, as the codeplug's System channels table needs it
			sb_str(&b, TR(DENY_WHY_TRUNK_UNMAPPED, "Unknown channel "));
			sb_u32(&b, v->t3.unmapped, 1);
			break;
#endif
		default: sb_str(&b, TR(DENY_WHY_RX_ONLY, "This channel is RX only")); break;
		}
		break;
#endif
	default: sb_str(&b, TR(DENY_WHY_NOT_ALLOWED, "Not allowed here")); break;
	}
	return buf;
}

static const char *deny_action(const radio_view_t *v)
{
	switch (v->tx_deny) {
	case TXDENY_BAND: return v->vfo ? TR(DENY_FIX_TUNE_INTO_BAND, "Tune into a band, then PTT") :
		TR(DENY_FIX_PICK_ANOTHER_CHANNEL, "Pick another channel");
	case TXDENY_TIMEOUT:
	case TXDENY_CEASED: return TR(DENY_FIX_RELEASE_PTT, "Release PTT to talk again");
	case TXDENY_NO_CODEC: return TR(DENY_FIX_ADD_CODEC, "Add codec with rfoundry");
	case TXDENY_BUSY: return TR(DENY_FIX_WAIT_FOR_CALL_END, "Wait for the call to end");
	case TXDENY_NO_REPEATER: return TR(DENY_FIX_CHECK_CC_AND_RANGE, "Check CC and range");
	case TXDENY_REJECTED:
	case TXDENY_NO_DMR: return TR(DENY_FIX_RELEASE_AND_RETRY, "Release PTT, try again");
	case TXDENY_HOTSPOT: return TR(HOTSPOT_HINT_OK_STOPS, "OK stop hotspot");
	case TXDENY_NO_ID: return TR(DENY_FIX_SET_DMR_ID, "Set your DMR ID in qdmr");
	case TXDENY_AM:
	case TXDENY_BCAST: return v->vfo ? TR(DENY_FIX_TUNE_OUT_OF_BAND, "Tune out of the band") :
		TR(DENY_FIX_PICK_ANOTHER_CHANNEL, "Pick another channel");
	case TXDENY_SFR_RPT: return TR(DENY_FIX_PICK_USER_CHANNEL, "Pick a user channel to talk");
	case TXDENY_HWOPT: return hwopt_action();
	case TXDENY_REFUSED: return TR(DENY_FIX_RELEASE_AND_RETRY, "Release PTT, try again");
	case TXDENY_BENCH: return TR(DENY_FIX_END_BENCH_ON_PC, "End the bench on the PC");
#if CONFIG_P25_TRUNK || CONFIG_DMR_TRUNK
	case TXDENY_TRUNK:
		if (trunk_why(v) == RADIO_TRUNK_WHY_OTHER_SYS || trunk_why(v) == RADIO_TRUNK_WHY_UNMAPPED)
			return TR(DENY_FIX_CHECK_SYSTEM, "Check system settings");
		if (trunk_why(v) == RADIO_TRUNK_WHY_REG_REFUSED)
			return TR(DENY_FIX_ASK_TO_ADD_ID, "Ask to add your ID");	// the system doesn't know it
		if (trunk_why(v) == RADIO_TRUNK_WHY_OTHER_KIND)
			return TR(DENY_FIX_PICK_ANOTHER_CHANNEL, "Pick another channel");	// no setting helps
		return TR(DENY_FIX_RELEASE_AND_RETRY, "Release PTT, try again");
#endif
	default: return TR(DENY_FIX_PICK_ANOTHER_CHANNEL, "Pick another channel");
	}
}

// On the air without PTT: the DMR stack sends a data burst list (ARS, SMS, a position, the IP gateway, a roaming probe)
static bool sending_data(const radio_view_t *v)
{
#if CONFIG_DMR
	return v->state == RS_TX && v->ch.mode == RADIO_MODE_DMR && v->dmr.state == RDMR_TX_DATA;
#else
	(void)v;
	return false;
#endif
}

// Outside the DMR block: the event banners call it in every build
bool ui_sb_gw_caller(sbuf_t *b, uint32_t src)
{
#if CONFIG_DMR_TRUNK_GW
	char num[24];
	if (ui_svc_t3_cli(src, num, sizeof num)) {
		sb_str(b, num[0] != '\0' ? num : TR(TRUNK_WORD_PHONE_LINE, "Phone line"));
		return true;
	}
#else
	(void)b;
	(void)src;
#endif
	return false;
}

// --- DMR (docs/ux/wireframes: Home DMR, Receiving, Transmitting) ---
#if CONFIG_DMR
// "TG 123 Main Ops", "Joe 9991234", "All call"; a private call without a name shows the ID only
// A contact name that only repeats the ID ("TG 123", "TG123", "123" for talkgroup 123; "9996543" for that radio):
// codeplugs often name talkgroups that way, and Home would read "TG 123 TG 123"
bool ui_name_is_id(const char *name, uint32_t id, bool private_call)
{
	const char *p = name;
	while (*p == ' ')
		p++;
	if (!private_call && (p[0] == 'T' || p[0] == 't') && (p[1] == 'G' || p[1] == 'g'))
		p += 2;
	while (*p == ' ')
		p++;
	if (*p < '0' || *p > '9')
		return false;
	uint32_t v = 0;
	for (; *p >= '0' && *p <= '9'; p++) {
		if (v > 0xFFFFFFu)
			return false;
		v = v * 10u + (uint32_t)(*p - '0');
	}
	while (*p == ' ')
		p++;
	return *p == '\0' && v == id;
}

static void sb_target(sbuf_t *b, uint32_t id, bool private_call, const char *name)
{
	if (ui_name_is_id(name, id, private_call))
		name = "";
	if (id == 0xFFFFFFu) {
		sb_str(b, TR(VALUE_ALL_CALL, "All call"));
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

// The target's short form, for when the whole doesn't fit: a talkgroup keeps its number ("TG 123"), a private contact
// its name ("Joe"), since the name is what the user picked and the ID alone says nothing at a glance
static void sb_target_short(sbuf_t *b, uint32_t id, bool private_call, const char *name)
{
	if (private_call && id != 0xFFFFFFu && name[0] != '\0' && !ui_name_is_id(name, id, private_call))
		sb_str(b, name);
	else
		sb_target(b, id, private_call, "");
}

// The target and its short form as text alternates (font.h), each followed by tail ("TG 91 World (9s)",
// "TG 91 (9s)"), and when tail isn't empty last the short form without it. Without a name, the one form
static void sb_target_alts(sbuf_t *b, uint32_t id, bool private_call, const char *name, const char *tail)
{
	bool named = name[0] != '\0' && id != 0xFFFFFFu && !ui_name_is_id(name, id, private_call);
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

// For the talk line: a named talkgroup as text alternates, longest first ("TG 201 Main Ops (8s)", "TG 201 Main Ops",
// "TG 201 Main O…" cut to room cells, "TG 201"), so the reply seconds go before the label and the label is cut before
// it goes. The cut form only when two letters of the label fit. label_first (Settings > Display, a side key) puts the
// name first and drops the number before the name. Anything else: sb_target_alts
static void sb_talk_alts(sbuf_t *b, uint32_t id, bool private_call, const char *name, const char *tail, size_t room,
			 bool label_first)
{
	bool named = name[0] != '\0' && id != 0xFFFFFFu && !ui_name_is_id(name, id, private_call);
	if (private_call || !named) {
		sb_target_alts(b, id, private_call, name, tail);
		return;
	}
	char num[16];
	sbuf_t nb;
	sb_init(&nb, num, sizeof num);
	sb_target(&nb, id, false, "");
	size_t len = strlen(name);
	sb_str(b, TEXT_ALT_OPEN);
	if (label_first) {
		// "Main Ops TG 201 (9s)", "Main Ops TG 201", "Main Ops", "Main O…": the name is what the user reads
		if (tail[0]) {
			sb_str(b, name);
			sb_chr(b, ' ');
			sb_str(b, num);
			sb_str(b, tail);
			sb_str(b, TEXT_ALT_NEXT);
		}
		sb_str(b, name);
		sb_chr(b, ' ');
		sb_str(b, num);
		sb_str(b, TEXT_ALT_NEXT);
		sb_str(b, name);
		if (room >= 2 && room < len) {
			sb_str(b, TEXT_ALT_NEXT);
			for (size_t i = 0; i < room - 1; i++)
				sb_chr(b, name[i]);
			sb_str(b, GLYPH_ELLIPSIS);
		}
		sb_str(b, TEXT_ALT_CLOSE);
		return;
	}
	size_t lead = strlen(num) + 1;	// "TG 201 "
	if (tail[0]) {
		sb_target(b, id, false, name);
		sb_str(b, tail);
		sb_str(b, TEXT_ALT_NEXT);
	}
	sb_target(b, id, false, name);
	if (room >= lead + 3 && room < lead + len) {
		sb_str(b, TEXT_ALT_NEXT);
		sb_str(b, num);
		sb_chr(b, ' ');
		for (size_t i = 0; i < room - lead - 1; i++)
			sb_chr(b, name[i]);
		sb_str(b, GLYPH_ELLIPSIS);
	}
	sb_str(b, TEXT_ALT_NEXT);
	sb_str(b, num);
	sb_str(b, TEXT_ALT_CLOSE);
}

// The caller as the big line: the contact name, else the ID
static void sb_caller(sbuf_t *b, const radio_dmr_call_t *c)
{
	if (c->src_name[0] == '\0' && ui_sb_gw_caller(b, c->src))
		return;
	if (c->src_name[0] != '\0')
		sb_str(b, c->src_name);
	else
		sb_u32(b, c->src, 1);
}

// Where the call went: "TG 123 Main Ops", "Private to you", "Private to 9991234"
static void sb_call_dst(sbuf_t *b, const radio_dmr_call_t *c)
{
	if (c->group) {
		sb_target(b, c->dst, false, c->dst_name);
	} else if (c->to_us) {
		sb_str(b, TR(CALL_VALUE_PRIVATE_TO_YOU, "Private to you"));
	} else {
		sb_str(b, TR(CALL_VALUE_PRIVATE_TO, "Private to "));
		if (c->dst_name[0] != '\0')
			sb_str(b, c->dst_name);
		else
			sb_u32(b, c->dst, 1);
	}
}

static bool dmr_call_screen(const radio_view_t *v)
{
	if (v->aprs_tx || v->ch.mode != RADIO_MODE_DMR || !v->dmr.active || v->hotspot)
		return false;
	if (v->state == RS_TX)
		return !sending_data(v);	// data has no talk target to show: Home stays, its activity line says so
	return v->state == RS_RX && (v->dmr.state == RDMR_RX || v->dmr.state == RDMR_RX_OTHER);
}

// Talk target line on Home: an arrow and "TG 201 Main Ops", or an inverted return arrow and "TG 9 (8s)" in warn color
// during the reply window. Icons, not words, so a small display keeps room for the label (#336)
static bool ui_st_dmr_off;		// ui_state_t.dmr_off of the frame being drawn

static void draw_talk_dmr(const radio_view_t *v, bool label_first)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->medium;
	const radio_dmr_view_t *d = &v->dmr;
	char buf[88], fit[48];
	sbuf_t b;
	color_t c = ui_col.fg;

	int w = gfx_width();
	// the reply window's icon inverted, so it stands out on a monochrome display as well. No space after either icon,
	// on purpose: the glyph's own blank column (or the inverted cell) sets it apart, and the space is one more
	// character of the label on a small display
	int x = d->reply_active ? gfx_text_bg(f, 0, L->talk_y, GLYPH_REPLY, ui_col.bg, ui_col.warn)
		: gfx_text(f, 0, L->talk_y, GLYPH_ARROW, ui_col.dim);
	size_t room = x < w ? (size_t)((w - x) / f->w) : 0;
	sb_init(&b, buf, sizeof buf);
	if (v->ch.kind != CODEPLUG_KIND_NONE && !codeplug_kind_built(v->ch.kind)) {
		// a mode this build doesn't run: its record's DMR fields are only a safe stand-in
		sb_str(&b, TR(WHY_MODE_NOT_IN_BUILD, "Mode: not in this build"));
		c = ui_col.warn;
	} else if (v->ch.rx_only) {
		sb_str(&b, TR(WORD_RX_ONLY, "RX only"));
		c = ui_col.warn;
	} else if (d->sfr_rpt) {
		// the channel's repeater role has the modem
		sb_str(&b, d->sfr_cw ? TR(HOME_STATE_SFR_SENDING_ID, "Sending ID") : d->sfr_busy ?
			TR(HOME_STATE_SFR_REPEATING, "Repeating") : TR(HOME_STATE_SFR_REPEATER, "SFR repeater"));
	} else if (!d->active && ui_st_dmr_off) {
		sb_str(&b, TR(HOME_STATE_DMR_MODEM_OFF, "DMR modem off"));
		c = ui_col.warn;
	} else {
		// The target and its shorter forms: "TG 91 World (9s)", "TG 91 World", "TG 91 Wo…", "TG 91"; "Joe 3101234", "Joe"
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
		sb_talk_alts(&b, d->target_id, d->target_private, d->target_name, secs, room, label_first);
	}
	// Too wide: the shorter alternate (a talkgroup's label cut, then dropped; a contact its ID), never a smaller font
	gfx_text_fit(f, buf, w - x, fit, sizeof fit);
	gfx_text(f, x, L->talk_y, fit, c);
}

#if CONFIG_DMR_TRUNK
void ui_draw_incoming_call(const radio_view_t *v, const ui_state_t *s, const char *name)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	char id[12], prompt[2][64];
	static char fit[CODEPLUG_NAME_LEN + 1];
	bool named = name[0] != 0;
	sbuf_t b;
	sb_init(&b, id, sizeof id);
	sb_u32(&b, v->t3.ring_src, 1);
	const char *title = TR(TRUNK_TITLE_INCOMING_CALL, "Incoming private call");
	const font_t *title_font = gfx_text_width(L->medium, title) <= w - 4 ? L->medium : L->small;
	int lines = wrap_lines(L->small, TR(TRUNK_HINT_PTT_ANSWER, "Press PTT to answer"),
			       w - 4, 2, prompt, false);

	gfx_clear(ui_col.bg);
	int top = ui_draw_top(v, s, NULL, NULL);
	int labels = (lines + named) * L->small->h;
	int height = title_font->h + L->large->h + labels;
	const font_t *id_font = height + 4 <= gfx_height() - top ? L->large : L->medium;
	height = title_font->h + id_font->h + labels;
	int gap = (gfx_height() - top - height) / 4;
	int y = top + gap;
	ui_draw_title(title_font, 2, y, w - 4, title, NULL, ui_col.fg);
	y += title_font->h + gap;
	if (named) {
		gfx_text_fit(L->small, name, w - 4, fit, sizeof fit);
		gfx_text(L->small, (w - gfx_text_width(L->small, fit)) / 2, y, fit, ui_col.fg);
		y += L->small->h;
	}
	gfx_text(id_font, (w - gfx_text_width(id_font, id)) / 2, y, id, ui_col.fg);
	y += id_font->h + gap;
	for (int i = 0; i < lines; i++, y += L->small->h)
		gfx_text(L->small, (w - gfx_text_width(L->small, prompt[i])) / 2, y, prompt[i], ui_col.fg);
}

// What the radio is doing on the trunked channel's system (dmr-trunking.md 12.4); *warn: something the user should see
static const char *t3_state_word(const radio_view_t *v, bool *warn)
{
	*warn = true;
	if (v->t3.unavail)
		return TR(TRUNK_WHY_SYSTEM_NOT_SET_UP, "System not set up");
	if (v->t3.fallback != 0)	// on the system's fallback channel, or looking for the system from it
		return TR(TRUNK_STATE_FALLBACK, "Fallback");
	if (v->t3.on && v->t3.state == T3_HUNT && (v->t3.other != 0 || v->t3.other_fid != 0))
		return TR(TRUNK_STATE_OTHER_SYSTEM, "Other system");	// Home's activity says which (draw_t3_other)
	if (v->t3.on && v->t3.refused)	// hunting for another TSCC, or registering again where it was refused
		return TR(TRUNK_STATE_REG_REFUSED, "Reg. refused");
	if (!v->t3.on || v->t3.state == T3_HUNT)
		return TR(TRUNK_STATE_SEARCHING, "Searching");
	if (v->t3.ringing)
		return TR(ACT_ROW_PRIVATE_CALL, "Private call");
	if (v->t3.state == T3_FOLLOW) {
		*warn = false;
		return TR(TRUNK_STATE_IN_CALL, "In call");
	}
	if (v->t3.alerting) {
		*warn = false;
		return TR(TRUNK_STATE_CALLING, "Calling");
	}
	if (v->t3.reg == T3_REG_DENIED)
		return TR(TRUNK_STATE_REFUSED, "Refused");
	if (v->t3.reg == T3_REG_NEEDED || v->t3.reg == T3_REG_UNKNOWN)
		return TR(TRUNK_STATE_REGISTERING, "Registering");
	*warn = false;
	return TR(TRUNK_STATE_READY, "Ready");
}

// Chips on a trunked channel: the system's name, what the radio does on it, power. Slot and color code are the
// site's, not the channel's. On the fallback channel the name and frequency rows are that channel's
static void draw_chips_t3(const radio_view_t *v)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	static char name[CODEPLUG_NAME_LEN + 1];
	static uint8_t named;		// the system name holds (n + 1; 0 none): read once, not every frame
	if (named != v->t3.sys) {
		named = v->t3.sys;
		if (!ui_svc_t3_name((uint8_t)(v->t3.sys - 1u), name) || name[0] == '\0')
			str_copy(name, sizeof name, TR(TRUNK_WORD_SYSTEM, "System"));
	}
	bool warn;
	const char *st = t3_state_word(v, &warn);
	const char *pw = ui_power_name(v->power, v->ch.tx_hz);
	int room = gfx_width() - gfx_text_width(f, st) - gfx_text_width(f, pw) - 2 * f->w;
	char fit[CODEPLUG_NAME_LEN + 4];
	int x = 0;
	if (room > 0) {
		gfx_text_fit(f, name, room, fit, sizeof fit);
		x = gfx_text(f, x, L->chips_y, fit, ui_col.fg) + f->w;
	}
	x = gfx_text(f, x, L->chips_y, st, warn ? ui_col.warn : ui_col.fg) + f->w;
	if (x + gfx_text_width(f, pw) <= gfx_width())
		gfx_text(f, x, L->chips_y, pw, ui_col.fg);
}
#endif

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
		x = gfx_text(f, x, L->chips_y, "Rpt", ui_col.fg) + f->w;	// a repeater channel; the signal bars show it heard
#if CONFIG_DMR_ROAMING
	// Roaming here: Roam, Lock when locked to the site, amber Roam? when the site is weak (dmr-roaming.md 6.2)
	if (v->roam.state != RROAM_STATE_OFF) {
		bool lock = v->roam.state == RROAM_STATE_LOCKED;
		x = gfx_text(f, x, L->chips_y, lock ? TR(HOME_CHIP_ROAM_LOCK, "Lock") : v->roam.weak ?
			TR(HOME_CHIP_ROAM_WEAK, "Roam?") : TR(HOME_CHIP_ROAM, "Roam"),
			     v->roam.weak ? ui_col.warn : ui_col.fg) + f->w;
	}
#endif
	bool pw_temp = v->power != channel_power(v);
	if (pw_temp)
		x = gfx_text(f, x, L->chips_y, GLYPH_DOT, ui_col.warn);
	x = gfx_text(f, x, L->chips_y, ui_power_name(v->power, v->ch.tx_hz), pw_temp ? ui_col.warn : ui_col.fg) + f->w;
	if (L->zone_y < 0 && v->vfo && !v->ch.rx_only && v->ch.tx_hz != v->ch.rx_hz) {
		// A split VFO with no top row for its TX frequency: the offset here ("+600k")
		sb_init(&b, buf, sizeof buf);
		sb_split(&b, v->ch.rx_hz, v->ch.tx_hz);
		if (x + gfx_text_width(f, buf) <= gfx_width())
			x = gfx_text(f, x, L->chips_y, buf, ui_col.warn) + f->w;
	}
	// Measured, not counted: a translation may be longer
	const char *dmon = TR(HOME_CHIP_DMON, "DMon"), *priv = TR(HOME_CHIP_PRIVACY, "Priv");
	if ((v->dmr.hear_all || s->hear_all) && x + gfx_text_width(f, dmon) <= gfx_width())
		x = gfx_text(f, x, L->chips_y, dmon, ui_col.warn) + f->w;	// digital monitor (FM's is "Mon")
	if ((s->privacy || v->dmr.privacy) && x + gfx_text_width(f, priv) <= gfx_width())
		gfx_text(f, x, L->chips_y, priv, ui_col.priv);
}

#if CONFIG_DMR_TRUNK
// Activity on Home while the radio searches a trunked system and hears only another one: what it heard, to compare
// with the system's settings (dmr-trunking.md 12.4): "Heard: Huge, net 0" / "site 83" (and the category when the
// site takes one only), or the first line alone where there is room for one. A system of another kind (no C_SYScode):
// "Incompatible system" / "Cannot join"
static void draw_t3_other(const radio_view_t *v, int y)
{
	const font_t *sm = ui_layout.small;
	int w = gfx_width();
	char a[48], c[40], fit[48];
	sbuf_t b;
	t3_syscode_t sc;
	if (v->t3.other == 0) {
		// Only a trunking system of another kind heard (its own FID, no ETSI Aloha): no setting helps
		gfx_text_fit(sm, TR(TRUNK_OTHER_KIND, "Incompatible system"), w, fit, sizeof fit);
		if (ui_layout.activity_h >= 2 * sm->h) {
			gfx_text(sm, 0, y, fit, ui_col.warn);
			gfx_text_fit(sm, TR(TRUNK_OTHER_KIND_NOTE, "Cannot join"), w, fit, sizeof fit);
			gfx_text(sm, 0, y + sm->h, fit, ui_col.fg);
		} else {
			gfx_text(sm, 0, y + (ui_layout.activity_h - sm->h) / 2, fit, ui_col.warn);
		}
		return;
	}
	t3_syscode_split(v->t3.other, &sc);
	static const tr_id_t model[4] = {	// the editor's network model names
		TRN(TRUNK_MODEL_TINY, "Tiny"), TRN(TRUNK_MODEL_SMALL, "Small"), TRN(TRUNK_MODEL_LARGE, "Large"),
		TRN(TRUNK_MODEL_HUGE, "Huge"),
	};
	sb_init(&b, a, sizeof a);
	sb_str(&b, TR(TRUNK_OTHER_HEARD, "Heard: "));
	sb_str(&b, tr(model[sc.model & 3u]));
	sb_str(&b, TR(TRUNK_OTHER_NET, ", net "));
	sb_u32(&b, sc.net, 1);
	sb_init(&b, c, sizeof c);
	sb_str(&b, TR(TRUNK_OTHER_SITE, "site "));
	sb_u32(&b, sc.site, 1);
	if (sc.par == T3_PAR_A)
		sb_str(&b, TR(TRUNK_OTHER_CAT_A, ", category A only"));
	else if (sc.par == T3_PAR_B)
		sb_str(&b, TR(TRUNK_OTHER_CAT_B, ", category B only"));
	if (ui_layout.activity_h >= 2 * sm->h) {
		gfx_text_fit(sm, a, w, fit, sizeof fit);
		gfx_text(sm, 0, y, fit, ui_col.warn);
		gfx_text_fit(sm, c, w, fit, sizeof fit);
		gfx_text(sm, 0, y + sm->h, fit, ui_col.fg);
		return;
	}
	// One line: the model and network, what a wrong setting usually is
	gfx_text_fit(sm, a, w, fit, sizeof fit);
	gfx_text(sm, 0, y + (ui_layout.activity_h - sm->h) / 2, fit, ui_col.warn);
}
#endif

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
		gfx_text(sm, 0, y + (ui_layout.activity_h - sm->h) / 2, TR(EMPTY_NO_CALLS_HEARD, "No calls heard yet"),
			ui_col.dim);	// centered
		return;
	}
	sb_init(&b, who, sizeof who);
	sb_caller(&b, &d->last);
	sb_init(&b, where, sizeof where);
	if (d->last.group) {
		sb_str(&b, "TG ");
		sb_u32(&b, d->last.dst, 1);
	} else {
		sb_str(&b, d->last.to_us ? TR(CALL_VALUE_TO_YOU, "To you") : TR(VALUE_PRIVATE, "Private"));
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
		// A private call says so where a group call has its "TG": "Private 9991234", else "Private"
		sb_str(&b, TEXT_ALT_OPEN);
		sb_str(&b, TR(CALL_BODY_PRIVATE_ID, "Private "));
		sb_u32(&b, d->target_id, 1);
		sb_str(&b, TEXT_ALT_NEXT);
		sb_str(&b, TR(VALUE_PRIVATE, "Private"));
		sb_str(&b, TEXT_ALT_CLOSE);
	} else if (v->state == RS_TX) {
		sb_target(&b, d->target_id, d->target_private, "");
	} else {
		sb_call_dst(&b, &d->call);
	}
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(v, s, NULL, title) + 1;

	if (v->state == RS_TX) {
		const char *word = TR(STATE_TALKING, "TALKING");
		switch (d->state) {
		case RDMR_TX_WAIT: word = d->rmo && !d->repeater_up ? TR(CALL_STATE_WAKING_RPT, "WAKING RPT") :
			TR(CALL_STATE_WAITING, "WAITING"); break;
		// the channel is ours: the talk permit sounds now
		case RDMR_TX_PERMIT: word = TR(CALL_STATE_ADMITTED, "ADMITTED"); break;
		case RDMR_TX_ENDING: word = TR(CALL_STATE_ENDING, "ENDING"); break;
		default: break;
		}
		const font_t *big = gfx_text_width(L->large, word) <= w ? L->large : L->medium;
		gfx_text_fit(big, word, w, buf, sizeof buf);	// a translation too wide even for the medium font
		gfx_text(big, 0, y, buf, ui_col.tx);
		y += big->h + 2;
		// The target with its name, else its short form: a talkgroup's number, a private contact's name
		sb_init(&b, buf, sizeof buf);
		sb_target_alts(&b, d->target_id, d->target_private, d->target_name, "");
		y = call_line(L->medium, y, buf, ui_col.fg);
		if (v->vfo) {	// a channel has its name in the codeplug; the VFO has only its TX frequency
			sb_init(&b, buf, sizeof buf);
			sb_str(&b, "TX ");
			sb_freq(&b, v->ch.tx_hz);
			sb_str(&b, " MHz");
			y = call_line(L->small, y, buf, ui_col.tx);
		}
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "TS");
		sb_u32(&b, d->ts, 1);
		sb_str(&b, " CC");
		sb_u32(&b, d->cc, 1);
		sb_str(&b, d->target_private ? TR(CALL_BODY_TO_PRIVATE, " Private") : TR(CALL_BODY_TO_GROUP, " Group"));
		if (d->rmo)
			sb_str(&b, " RPT");
		if (d->privacy)
			sb_str(&b, " ENC");
		y = call_line(L->small, y, buf, ui_col.dim);
		sb_init(&b, buf, sizeof buf);
		sb_mmss(&b, v->tx_elapsed_ms / 1000u);
		if (v->tot_s) {
			sb_str(&b, TR(PART_OF, " of "));	// the time-out
			sb_mmss(&b, v->tot_s);
		}
		call_line(L->small, y, buf, ui_col.fg);
		ui_draw_hint(d->state == RDMR_TX_ENDING ? TR(CALL_HINT_FINISHING, "Finishing the call") :
			TR(CALL_HINT_RELEASE_PTT, "Release PTT to stop"), false);
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
	sb_u32(&b, d->rx_ts ? d->rx_ts : d->ts, 1);
	if (d->hear_all && d->rx_cc != d->cc) {	// found by digital monitor: the code to set to talk to them
		sb_str(&b, " CC");
		sb_u32(&b, d->rx_cc, 1);
	}
	if (d->late_entry)
		sb_str(&b, TR(CALL_CHIP_LATE, " late"));
	if (d->call.encrypted)
		sb_str(&b, " ENC");	// privacy marker (played only with the key, never after a PI header)
	if (d->call.emergency)
		sb_str(&b, TR(CALL_CHIP_EMERGENCY, " EMERGENCY"));
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
	const char *why = d->state == RDMR_RX_OTHER ? (d->call.encrypted ?
		TR(CALL_WHY_ENCRYPTED, "Encrypted: not played") :
		TR(CALL_WHY_OTHER_TG, "Other TG: not played")) : !d->codec ? TR(WHY_NO_CODEC, "No codec: voice off") :
			NULL;
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
	ui_draw_hint(d->state == RDMR_RX_OTHER ? TR(CALL_HINT_MON_DMON, "MON: digital monitor") :
		TR(HINT_PTT_REPLIES, "PTT replies"), false);
}
#endif

#if CONFIG_AIR
// A P25 or NXDN call coming in (air_call): whether one is heard now, and its fields
typedef struct {
	bool played, enc, unit;
	uint32_t src, dst;
} air_call_t;

static bool air_call(const radio_view_t *v, air_call_t *c)
{
#if CONFIG_P25
	if (v->ch.mode == RADIO_MODE_P25 && (v->p25.flags & RADIO_P25_RX) != 0) {
		const radio_p25_view_t *p = &v->p25;
		c->played = (p->flags & RADIO_P25_PLAYED) != 0;
		c->enc = (p->flags & RADIO_P25_ENC) != 0;
		c->unit = (p->flags & RADIO_P25_UNIT) != 0;
		c->src = p->src;
		c->dst = p->dst;
		return true;
	}
#endif
#if CONFIG_NXDN
	if (v->ch.mode == RADIO_MODE_NXDN && (v->nxdn.flags & RADIO_NXDN_RX) != 0) {
		const radio_nxdn_view_t *n = &v->nxdn;
		c->played = (n->flags & RADIO_NXDN_PLAYED) != 0;
		c->enc = (n->flags & RADIO_NXDN_ENC) != 0;
		c->unit = (n->flags & RADIO_NXDN_UNIT) != 0;
		c->src = n->src;
		c->dst = n->dst;
		return true;
	}
#endif
	(void)v;
	(void)c;
	return false;
}

// Why the call isn't played, else who calls and where ("ID 1234 > TG 1", "ID 1234 > Private to 5678"; the ID is
// missing until the call's link control is read)
static void air_call_line(const air_call_t *c, const font_t *f, int y)
{
	char buf[64], fit[48];
	sbuf_t b;

	if (!c->played) {
		gfx_text_fit(f, c->enc ? TR(CALL_WHY_ENCRYPTED, "Encrypted: not played") :
			TR(CALL_WHY_OTHER_TG, "Other TG: not played"), gfx_width(), fit, sizeof fit);
		gfx_text(f, 0, y, fit, ui_col.warn);
		return;
	}
	// Too wide: keep the marker and destination, then cut the destination if needed
	for (int with_id = c->src != 0; with_id >= 0; with_id--) {
		sb_init(&b, buf, sizeof buf);
		if (c->enc)
			sb_str(&b, "ENC ");
		if (with_id) {
			sb_str(&b, "ID ");
			sb_u32(&b, c->src, 1);
			sb_str(&b, " " GLYPH_ARROW " ");
		}
		if (c->unit) {
			sb_str(&b, TR(CALL_VALUE_PRIVATE_TO, "Private to "));
		} else {
			sb_str(&b, "TG ");
		}
		sb_u32(&b, c->dst, 1);
		if (gfx_text_width(f, buf) <= gfx_width())
			break;
	}
	gfx_text_fit(f, buf, gfx_width(), fit, sizeof fit);
	gfx_text(f, 0, y, fit, ui_col.rx);
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
#if CONFIG_AIR
		air_call_t ac;
		bool air_rx = air_call(v, &ac);
		if (!two && air_rx) {
			air_call_line(&ac, sm, y);	// one line: who calls says more than the meter
			break;
		}
#endif
		sb_i32(&b, v->rssi_dbm);
		sb_str(&b, " dBm");
		int tw = gfx_text_width(big, buf);
		gfx_text(big, w - tw, y, buf, ui_col.fg);
		draw_meter(0, y + big->h / 4, w - tw - big->w, big->h / 2, v->rssi_dbm);
		if (!two)
			break;
#if CONFIG_AIR
		if (air_rx) {
			air_call_line(&ac, sm, y2);
			break;
		}
#endif
		gfx_text(sm, 0, y2, v->monitor ? TR(ACTIVITY_STATE_SQUELCH_OPEN, "Squelch open") :
			TR(STATE_RECEIVING, "Receiving"), ui_col.rx);
		break;
	}
	case RS_TX: {
		if (v->aprs_tx) {
			gfx_text(big, 0, y, TR(STATE_SENDING, "SENDING"), ui_col.tx);
			break;
		}
		sb_mmss(&b, v->tx_elapsed_ms / 1000u);
		int tw = gfx_text_width(big, buf);
		gfx_text(big, w - tw, y, buf, ui_col.fg);
		gfx_text(big, 0, y, sending_data(v) ? TR(STATE_SENDING, "SENDING") : TR(STATE_TALKING, "TALKING"),
			ui_col.tx);
		if (two) {
			sb_init(&b, buf, sizeof buf);
			if (v->dtmf_digits[0] != 0) {
				// DTMF sent in this transmission (radio-features.md 3.7): what the other end got, newest last
				sb_str(&b, "DTMF ");
				sb_str(&b, v->dtmf_digits);
				gfx_text(sm, 0, y2, buf, ui_col.fg);
				break;
			}
			if (v->tot_s) {
				uint32_t el = v->tx_elapsed_ms / 1000u;
				sb_mmss(&b, el < v->tot_s ? v->tot_s - el : 0);
				sb_str(&b, TR(ACTIVITY_BODY_LEFT_OF, " left of "));	// the time-out
				sb_mmss(&b, v->tot_s);
			} else {
				sb_str(&b, TR(ACTIVITY_BODY_NO_TIME_LIMIT, "No time limit"));
			}
			gfx_text(sm, 0, y2, buf, ui_col.dim);
		}
		break;
	}
	case RS_TX_DENIED:
		// One line only: the pill already says NO TX, so the line gives the reason
		gfx_text_fit(sm, deny_reason(v, buf, sizeof buf), w, fit, sizeof fit);
		if (two) {
			gfx_text(big, 0, y, TR(ACTIVITY_STATE_CANT_TRANSMIT, "Can't transmit"), ui_col.warn);
			gfx_text(sm, 0, y2, fit, ui_col.fg);
		} else {
			gfx_text(sm, 0, y, fit, ui_col.warn);
		}
		break;
	default:
		if (board_hwopt_pending() != NULL) {
			// Something only a person can tell the radio, and transmit waits for it: said where the eye
			// goes, before the last call heard
			hwopt_needed(&b);
			gfx_text_fit(sm, buf, w, fit, sizeof fit);
			bool two_sm = L->activity_h >= 2 * sm->h;
			y += (L->activity_h - (two_sm ? 2 : 1) * sm->h) / 2;
			gfx_text(sm, 0, y, fit, ui_col.warn);
			if (two_sm) {
				gfx_text_fit(sm, TR(ACTIVITY_BODY_TX_WAITS, "Transmit waits for it"), w, fit,
					sizeof fit);
				gfx_text(sm, 0, y + sm->h, fit, ui_col.fg);
			}
			break;
		}
#if CONFIG_DMR_TRUNK
		if (v->t3.on && v->t3.state == T3_HUNT && (v->t3.other != 0 || v->t3.other_fid != 0)) {
			draw_t3_other(v, y);
			break;
		}
#endif
#if CONFIG_DMR
		if (v->ch.mode == RADIO_MODE_DMR && v->dmr.active) {
			draw_last_heard(v, s, y);	// hear all shows as "All" in the chips and in the hint
			break;
		}
#endif
		if (v->last_rx_ms) {
			sb_str(&b, TR(ACTIVITY_BODY_LAST, "Last activity "));
			sb_age(&b, s->now_ms - v->last_rx_ms);
		} else {
			sb_str(&b, TR(ACTIVITY_EMPTY, "No activity yet"));
		}
		bool mon = v->monitor && L->activity_h >= 2 * sm->h;
		y += (L->activity_h - (mon ? 2 : 1) * sm->h) / 2;	// centered in the activity area
		gfx_text_fit(sm, buf, gfx_width(), fit, sizeof fit);
		gfx_text(sm, 0, y, fit, ui_col.dim);
		if (mon)
			gfx_text(sm, 0, y + sm->h, TR(ACTIVITY_STATE_SQUELCH_OPEN, "Squelch open"), ui_col.warn);
		break;
	}
}

static const char *home_hint(const radio_view_t *v, const ui_state_t *s)
{
	if (v->state == RS_TX_DENIED)
		return deny_action(v);
	if (s->entry)
		return TR(FREQ_HINT_ENTRY, "OK tune  Back delete");
	if (s->locked)
		return TR(LOCK_HINT_HOLD_STAR, TEXT_ALT_OPEN "Keys locked: hold * to unlock"
			TEXT_ALT_NEXT "Locked: hold * to unlock" TEXT_ALT_CLOSE);
	if (s->scanning)
		return TR(HOME_HINT_SCANNING, "# skip  Back stop scan");
	if (board_hwopt_pending() != NULL)
		return hwopt_action();
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR) {
		if (v->dmr.reply_active)
			return TR(HOME_HINT_REPLY, "# caller  Back end reply");
		if (!v->dmr.codec)
			return TR(WHY_NO_CODEC, "No codec: voice off");
		if (v->dmr.hear_all)
			return TR(HOME_HINT_DMON, "Digital monitor  Back off");
	}
#endif
	if (v->temporary)
		return TR(HOME_HINT_UNSAVED, "Back undo  Hold OK save");
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR && v->dmr.target_manual)
		return v->vfo ?
			TR(HOME_HINT_TALK_TARGET_VFO, TEXT_ALT_OPEN "Back: VFO's talk target"
				TEXT_ALT_NEXT "Back default target" TEXT_ALT_CLOSE) :
			TR(HOME_HINT_TALK_TARGET_CHANNEL, TEXT_ALT_OPEN "Back: channel's talkgroup"
				TEXT_ALT_NEXT "Back default target" TEXT_ALT_CLOSE);
#endif
	if (v->vfo)
		return TR(HOME_HINT_VFO, "0-9 freq  * Quick  Up/Dn");
	return s->home_hint != NULL ? s->home_hint : TR(HOME_HINT_MENU_QUICK, "OK Menu  * Quick");
}

// The largest font no taller than room in which text fits the width (at least the medium font), trying each of the
// text's alternates (font.h) from the longest; out gets the form chosen
static const font_t *large_font(const char *text, int room, char *out, size_t cap)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *fonts[] = { font_largest, L->large, L->medium };
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
	bool tx = v->state == RS_TX && v->vfo;
	if (v->vfo)
		sb_freq(&b, tx ? v->ch.tx_hz : v->ch.rx_hz);
	else
		sb_str(&b, v->ch.name);
	const font_t *f = large_font(buf, room, form, sizeof form);
	gfx_text_fit(f, form, w, fit, sizeof fit);
	gfx_text(f, 0, y, fit, tx ? ui_col.tx : ui_col.fg);
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
		sb_str(&b, mode_word(v));
		sb_chr(&b, ' ');
		sb_str(&b, ui_power_name(v->power, v->ch.tx_hz));
	}
	f = large_font(buf, room, form, sizeof form);
	gfx_text_fit(f, form, w, fit, sizeof fit);
	gfx_text(f, 0, y, fit, v->state == RS_TX ? ui_col.tx : v->state == RS_RX ? ui_col.rx : ui_col.fg);
}

void ui_draw_home(const radio_view_t *v, const ui_state_t *s)
{
#if CONFIG_DUAL_WATCH
	if (s->dual.on) {
		ui_draw_home_dual(v, s);
		return;
	}
#endif
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
		draw_talk(v, s->label_first);
		draw_chips(v, s);
		if (ui_layout.sep_y >= 0)
			gfx_hline(0, ui_layout.sep_y, gfx_width(), ui_col.line);
		draw_activity(v, s);
	}
	ui_draw_banner(s);
	ui_draw_bottom(s, home_hint(v, s));
}

#if CONFIG_DUAL_WATCH
// --- Dual watch Home (docs/ux/dual-watch.md 3) ---
// Two panes, A over B, the same size: neither channel is second class. Each pane has a letter tag, the channel's
// number and name (the VFO: its frequency), then what PTT talks to there (DMR) or its frequency (FM), and its
// frequency and chips where there is room. The selected pane's tag is filled and says PTT; the pane a call is coming
// in on is green and says RX, with the caller; the pane the last call came in on says how long ago. Every color has
// its word (P5)

typedef struct {
	const font_t *name, *target, *detail;	// detail NULL: no third row
	bool zone;				// a fourth row for the zone (else it shares the third when it fits)
	int act;				// activity lines under the panes
} dual_fit_t;

static const font_t *dual_nf;	// the channel names' font: one for both panes, the largest both fit

static int dual_pane_h(const dual_fit_t *ft)
{
	int h = ft->name->h + ft->target->h;
	if (ft->detail != NULL)
		h += ft->detail->h * (ft->zone ? 2 : 1);
	return h;
}

// Text right-aligned at x_end on a row of font rf that starts at y with height row_h, whole (or one of its
// alternates) or not at all: returns its left edge, or x_end when it doesn't fit right of min_x
static int dual_right(const font_t *rf, int x_end, int y, int row_h, int min_x, const char *text, color_t c)
{
	char fit[48];
	int room = x_end - min_x;
	bool more = true;
	int tw = 0;
	for (int level = 0; more; level++) {	// the longest alternate that fits
		gfx_text_alt(text, level, fit, sizeof fit, &more);
		tw = gfx_text_width(rf, fit);	// pixels: a CJK character is not one cell wide
		if (tw <= room)
			break;
	}
	if (tw > room || fit[0] == 0)
		return x_end;
	gfx_text(rf, x_end - tw, y + (row_h - rf->h + 1) / 2, fit, c);
	return x_end - tw;
}

// Where a pane's text starts: right of its tag, which is sized from the layout's name font
static int dual_tag_w(const font_t *nf) { return nf->w + (nf->w >= 12 ? 10 : nf->w >= 8 ? 6 : 4); }
static int dual_x0(const font_t *nf) { return dual_tag_w(nf) + (ui_layout.small->w + 1) / 2; }
static int dual_pill_w(const char *word)
{
	const font_t *sm = ui_layout.small;
	return gfx_text_width(sm, word) + 2 * (sm->w >= 8 ? 4 : 3);
}
// The channel number's font next to a name in f: medium beside the large font, else the name's
static const font_t *dual_num_font(const font_t *f) { return f == ui_layout.large ? ui_layout.medium : f; }

// The pane's first-row text: the channel name, the VFO's frequency, or the selected VFO's typed digits
static void dual_name(const ui_state_t *s, int i, char *buf, size_t len)
{
	const ui_dual_pane_t *p = &s->dual.pane[i];
	if (p->vfo && s->entry && s->dual.sel == i) {
		home_entry_text(s, buf, len);
		return;
	}
	sbuf_t b;
	sb_init(&b, buf, len);
	if (p->vfo)
		sb_freq(&b, p->rx_hz);
	else
		sb_str(&b, p->name);
}

// The largest font up to the layout's name font in which pane i's name fits whole next to its number and the
// widest pill (PTT)
static const font_t *dual_name_font(const ui_state_t *s, int i, const font_t *nf)
{
	const ui_layout_t *L = &ui_layout;
	const ui_dual_pane_t *p = &s->dual.pane[i];
	const font_t *order[] = { L->large, L->medium, L->small };
	char buf[40], num[8];
	sbuf_t nb;
	sb_init(&nb, num, sizeof num);
	sb_u32(&nb, p->number, 1);
	dual_name(s, i, buf, sizeof buf);
	int room = gfx_width() - dual_x0(nf) - dual_pill_w("PTT") - L->small->w / 2 - 1;
	for (unsigned k = 0; k < 3; k++) {
		const font_t *f = order[k];
		if (f->h > nf->h)
			continue;
		const font_t *mf = dual_num_font(f);
		int need = gfx_text_width(f, buf) + (p->vfo ? 0 : gfx_text_width(mf, num) + mf->w / 2);
		if (need <= room || f == L->small)
			return f;
	}
	return L->small;
}

// A right-pointing triangle ah high (odd) and aw wide at x, y: the selected channel's mark
static void dual_arrow(int x, int y, int ah, int aw, color_t c)
{
	int mid = ah / 2;
	for (int r = 0; r < ah; r++) {
		int d = r < mid ? mid - r : r - mid;
		int len = mid ? (aw * (mid - d) + mid - 1) / mid : aw;
		gfx_hline(x, y + r, len > 0 ? len : 1, c);
	}
}

static void dual_pane(const radio_view_t *v, const ui_state_t *s, int i, int y, int h, int pad, const dual_fit_t *ft)
{
	const ui_layout_t *L = &ui_layout;
	const ui_dual_t *d = &s->dual;
	const ui_dual_pane_t *p = &d->pane[i];
	const font_t *nf = ft->name, *sm = L->small;
	int w = gfx_width();
	bool tx = v->state == RS_TX && d->sel == i, rx = !tx && p->rx, sel = d->sel == i;
	bool entry = s->entry && sel && p->vfo;
	bool quiet = !rx && d->pane[!i].rx;	// the other pane has a call: this one steps back
	color_t fg = quiet ? ui_col.dim : ui_col.fg;
	char buf[48], fit[48];
	sbuf_t b;

	// The tag: the pane's letter in a box as tall as the pane's text. Filled for the selected pane (red while it
	// transmits), green while a call comes in on it, an outline otherwise (green while it is busy: p->busy)
	int ty = y + pad, th = h - 2 * pad;
	int tw = dual_tag_w(nf);
	const char *letter = i ? "B" : "A";
	color_t tbg = ui_col.bg, tfg = ui_col.dim;
	if (tx) {
		tbg = ui_col.tx;
		tfg = ui_col.pill_fg;
	} else if (rx) {
		tbg = ui_col.rx;
		tfg = ui_col.pill_dark_fg;
	} else if (sel) {
		tbg = ui_col.fg;
		tfg = ui_col.bg;
	}
	bool busy = p->busy && !rx && !tx;
	if (tbg != ui_col.bg)
		gfx_round_fill(0, ty, tw, th, tbg);
	else
		gfx_rect(0, ty, tw, th, busy ? ui_col.rx : ui_col.line);
	// The letter, and under it on the selected pane an arrow pointing at its channel (none on the other). Both
	// letters sit at the same height; a font smaller where the tag is too short for the letter and the arrow
	int ah = nf->h / 3 < 5 ? 5 : nf->h / 3 | 1;		// the arrow's height, odd
	const font_t *lf = nf;
	if (lf->h + 2 + ah > th)
		lf = nf == L->large ? L->medium : L->small;
	if (lf->h + 2 + ah > th)
		lf = L->small;
	if (lf->h + 1 + ah > th)
		ah = 0;					// no room even so: the filled tag says it alone
	else if (ah < lf->h * 2 / 3 && lf->h + 2 + lf->h * 2 / 3 <= th)
		ah = lf->h * 2 / 3 | 1;			// tall tags: an arrow in proportion to the letter
	int ly = ty + (th - lf->h - (ah ? ah + 2 : 0) + 1) / 2;
	gfx_text(lf, (tw - gfx_text_width(lf, letter) + 1) / 2, ly, letter, tfg);
	if (sel && ah) {
		dual_arrow((tw - (ah + 1) / 2) / 2, ly + lf->h + 2, ah, (ah + 1) / 2, tfg);
	}

	int x0 = dual_x0(nf);
	int rx_end = w;

	// Row 1, right: PTT on the selected pane, always (TX while transmitting), so where PTT goes stays plain with
	// calls on both; RX is on row 2. A name that doesn't fit whole beside it keeps the row: the pill goes to the
	// right of row 3, where there is one
	int y1 = ty, pill_h = sm->h;
	const font_t *f = dual_nf != NULL ? dual_nf : nf;
	const char *word = tx ? "TX" : "PTT";
	bool pill3 = false;
	if ((tx || sel) && ft->detail != NULL) {
		dual_name(s, i, buf, sizeof buf);
		int need = gfx_text_width(f, buf);
		if (!p->vfo) {
			sb_init(&b, fit, sizeof fit);
			sb_u32(&b, p->number, 1);
			need += gfx_text_width(dual_num_font(f), fit) + dual_num_font(f)->w / 2;
		}
		// Row 3 has room for it after the frequency and chips
		sb_init(&b, fit, sizeof fit);
		if (p->mode == RADIO_MODE_DMR && !p->vfo) {
			sb_freq(&b, p->rx_hz);
			sb_str(&b, " MHz  ");
		}
		sb_str(&b, p->detail);
		int room3 = w - x0 - gfx_text_width(ft->detail, fit) - 2 * ft->detail->w;
		pill3 = x0 + need > w - dual_pill_w(word) - sm->w / 2 - 1 && room3 >= dual_pill_w(word);
	}
	if ((tx || sel) && !pill3) {
		rx_end -= dual_pill_w(word);
		gfx_pill(sm, rx_end, y1 + (nf->h - pill_h + 1) / 2, pill_h, word, tx ? ui_col.pill_fg : ui_col.bg,
			 tx ? ui_col.tx : ui_col.fg);
		rx_end -= sm->w / 2 + 1;
	}

	// Row 1, left: the channel number (medium, the channel number's color) and name in the names' font; the VFO: its
	// frequency
	int base = y1 + nf->ascent;
	int x = x0;
	if (!p->vfo) {
		char num[8];
		sbuf_t nb;
		sb_init(&nb, num, sizeof num);
		sb_u32(&nb, p->number, 1);
		const font_t *mf = dual_num_font(f);
		x = gfx_text(mf, x, base - mf->ascent, num, quiet || gfx_bpp() == 1 ? ui_col.dim : ui_col.chnum) +
			mf->w / 2;
	}
	dual_name(s, i, buf, sizeof buf);
	if (tx && p->vfo && !entry) {	// the VFO transmitting: its TX frequency (the view's channel is the selected pane's)
		sb_init(&b, buf, sizeof buf);
		sb_freq(&b, v->ch.tx_hz);
	}
	gfx_text_fit(f, buf, rx_end - x, fit, sizeof fit);
	x = gfx_text(f, x, base - f->ascent, fit, entry ? ui_col.warn : tx && p->vfo ? ui_col.tx : rx ? ui_col.rx : fg);
	if (p->vfo && x + sm->w / 2 + gfx_text_width(sm, "MHz") <= rx_end) {
		gfx_text(sm, x + sm->w / 2, base - sm->ascent, "MHz", ui_col.dim);
		x += sm->w / 2 + gfx_text_width(sm, "MHz");
	}
	// Then how long ago a call last came in on this pane, where it fits whole: both panes, the newest call's in amber
	bool aged = false;
	uint32_t ago = s->now_ms - d->ended_ms[i];
	color_t agec = d->last == i ? ui_col.warn : ui_col.dim;
	if (d->ended[i] && !rx && !tx && !busy) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TEXT_ALT_OPEN);
		sb_str(&b, TR(DUAL_BODY_HEARD, "Heard "));
		sb_age_long(&b, ago);
		sb_str(&b, TEXT_ALT_NEXT);
		sb_age_long(&b, ago);
		sb_str(&b, TEXT_ALT_CLOSE);
		aged = dual_right(sm, rx_end, y1, nf->h, x + sm->w, buf, agec) < rx_end;
	}

	// Row 2: the caller while a call comes in here; else what PTT talks to (DMR) or the frequency (FM). Right: the
	// frequency (DMR) or the chips (FM) when there is no third row, the signal while receiving, Busy while the watch
	// waits for a reply on the other pane and a look found a signal here
	const font_t *tf = ft->target;
	int y2 = y1 + nf->h;
	char right[32];
	sbuf_t rb;
	sb_init(&rb, right, sizeof right);
	sb_init(&b, buf, sizeof buf);
	color_t c2 = fg;
	if (rx) {
		sb_str(&b, p->caller[0] ? p->caller : TR(STATE_RECEIVING, "Receiving"));
		c2 = ui_col.rx;
	} else if (p->mode == RADIO_MODE_DMR) {
		sb_str(&b, p->target[0] ? p->target : "DMR");
		if (ft->detail == NULL && !p->vfo)
			sb_freq(&rb, p->rx_hz);
		else if (ft->detail == NULL)
			sb_str(&rb, p->detail);
	} else if (p->vfo) {
		sb_str(&b, "VFO ");
		sb_str(&b, dual_mode_word(p));
		if (ft->detail == NULL)
			sb_str(&rb, p->detail);
	} else {
		sb_freq(&b, p->rx_hz);
		sb_str(&b, " MHz");
		if (ft->detail == NULL)
			sb_str(&rb, p->detail);
	}
	// No list of calls under the panes (the smallest displays): the last call's pane says how long ago instead
	color_t rc = rx ? ui_col.fg : ui_col.dim;
	if (ft->act == 0 && d->last == i && !rx && !tx && !aged) {
		sb_init(&rb, right, sizeof right);
		sb_str(&rb, TEXT_ALT_OPEN);
		sb_str(&rb, TR(DUAL_BODY_HEARD, "Heard "));
		sb_age_long(&rb, ago);
		sb_str(&rb, TEXT_ALT_NEXT);
		sb_age_long(&rb, ago);
		sb_str(&rb, TEXT_ALT_CLOSE);
		rc = ui_col.warn;
	}
	if (busy) {
		sb_init(&rb, right, sizeof right);
		sb_str(&rb, TR(DUAL_STATE_BUSY, "Busy"));
		rc = ui_col.rx;
	}
	int r2 = w;
	if (rx) {	// the RX pill at the right of the caller
		r2 -= dual_pill_w("RX");
		gfx_pill(sm, r2, y2 + (tf->h - pill_h + 1) / 2, pill_h, "RX", ui_col.pill_dark_fg, ui_col.rx);
		r2 -= sm->w / 2 + 1;
	} else if (right[0]) {
		int need = gfx_text_width(tf, buf) + sm->w;
		int nx = dual_right(sm, w, y2, tf->h, x0 + need, right, rc);
		if (nx < w)
			r2 = nx - sm->w;
	}
	gfx_text_fit(tf, buf, r2 - x0, fit, sizeof fit);
	gfx_text(tf, x0, y2, fit, c2);

	// Row 3: the frequency and the chips, the zone at the right (B may be in another zone than A)
	if (ft->detail != NULL) {
		const font_t *df = ft->detail;
		int y3 = y2 + tf->h;
		sb_init(&b, buf, sizeof buf);
		if (p->mode == RADIO_MODE_DMR && !p->vfo) {
			sb_freq(&b, p->rx_hz);
			sb_str(&b, " MHz  ");
		}
		sb_str(&b, p->detail);
		int lx = gfx_text(df, x0, y3, buf, ui_col.dim);
		int r3 = w;
		if (pill3) {
			r3 -= dual_pill_w(word);
			gfx_pill(sm, r3, y3 + (df->h - pill_h + 1) / 2, pill_h, word, tx ? ui_col.pill_fg : ui_col.bg,
				 tx ? ui_col.tx : ui_col.fg);
			r3 -= sm->w / 2 + 1;
		}
		if (ft->zone)
			gfx_text(df, x0, y3 + df->h, p->zone, ui_col.dim);
		else
			dual_right(df, r3, y3, df->h, lx + 2 * df->w, p->zone, ui_col.dim);
	}
}

// Large text (Settings > Display): the two channels only, each as large as fits. The selected one has the arrow;
// the one a call comes in on is green and says RX (a word, for mono displays)
static void dual_large(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	const ui_dual_t *d = &s->dual;
	int w = gfx_width();
	int y = L->strip_h + 2;
	int room = (L->hint_y - y) / 2;
	for (int i = 0; i < 2; i++) {
		const ui_dual_pane_t *p = &d->pane[i];
		char buf[48], form[48], fit[48];
		sbuf_t b;
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, i ? "B " : "A ");
		if (p->vfo) {
			char name[24];
			dual_name(s, i, name, sizeof name);
			sb_str(&b, name);
		} else
			sb_str(&b, p->name);
		const font_t *f = large_font(buf, room, form, sizeof form);
		if (gfx_text_width(f, form) + (d->sel == i ? f->w / 3 + 2 : 0) > w) {
			const font_t *smaller[] = { L->large, L->medium, L->small };
			for (unsigned k = 0; k < sizeof smaller / sizeof smaller[0]; k++) {
				f = smaller[k];
				if (f->h <= room && gfx_text_width(f, form) + (d->sel == i ? f->w / 3 + 2 : 0) <= w)
					break;
			}
		}
		bool tx = v->state == RS_TX && d->sel == i, rx = !tx && p->rx;
		color_t c = s->entry && d->sel == i && p->vfo ? ui_col.warn :
			    tx ? ui_col.tx : rx ? ui_col.rx : d->sel == i ? ui_col.fg : ui_col.dim;
		// The arrow in the room a selection bar would take: f->w / 3 wide, half the letter high, a 2 px gap; the
		// other line starts at 0
		int aw = d->sel == i ? f->w / 3 + 2 : 0, ah = (f->h / 2) | 1;
		if (aw)
			dual_arrow(0, y + (f->h - ah) / 2, ah, aw - 2, c);
		gfx_text_fit(f, form, w - aw, fit, sizeof fit);
		if (rx || tx) {	// receiving or transmitting: the letter in a filled box, as the tag on Home (the strip has the word)
			char letter[2] = { fit[0], 0 };
			gfx_fill(aw, y, f->w, f->h, c);
			gfx_text(f, aw, y, letter, gfx_bpp() == 1 ? ui_col.bg : ui_col.pill_dark_fg);
			gfx_text(f, aw + f->w, y, fit + 1, c);
		} else {
			gfx_text(f, aw, y, fit, c);
		}
		y += room;
	}
}

// One call of the list: its pane's letter in an outline tag, the caller, how long ago at the right
static void dual_call(int y, uint8_t pane, const char *caller, uint32_t age_ms, color_t c)
{
	const font_t *sm = ui_layout.small;
	int w = gfx_width();
	char age[16], fit[48];
	sbuf_t ab;
	int tw = sm->w + 4;
	gfx_rect(0, y, tw, sm->h, ui_col.line);
	gfx_text(sm, 2, y, pane ? "B" : "A", c);
	sb_init(&ab, age, sizeof age);
	sb_age(&ab, age_ms);
	int ax = dual_right(sm, w, y, sm->h, tw + 8 * sm->w, age, ui_col.dim);
	gfx_text_fit(sm, caller, ax - sm->w - tw - sm->w / 2, fit, sizeof fit);
	gfx_text(sm, tw + sm->w / 2, y, fit, c);
}

// The lines under the panes: what is happening, then the last calls, each with its pane
static void dual_activity(const radio_view_t *v, const ui_state_t *s, int y, int lines)
{
	const ui_layout_t *L = &ui_layout;
	const ui_dual_t *d = &s->dual;
	const font_t *sm = L->small;
	int w = gfx_width();
	char buf[64];
	sbuf_t b;
	if (lines <= 0)
		return;
	// A transmission or a call coming in takes the first line
	sb_init(&b, buf, sizeof buf);
	if (v->state == RS_TX) {
		if (v->aprs_tx)
			sb_str(&b, TR(STATE_SENDING, "SENDING"));
		else {
			sb_str(&b, sending_data(v) ? TR(DUAL_STATE_SENDING_ON, "SENDING on ") :
				TR(DUAL_STATE_TALKING_ON, "TALKING on "));
			sb_str(&b, d->sel ? "B" : "A");
		}
		gfx_text(sm, 0, y, buf, ui_col.tx);
		if (!v->aprs_tx) {
			sb_init(&b, buf, sizeof buf);
			sb_mmss(&b, v->tx_elapsed_ms / 1000u);
			gfx_text(sm, w - gfx_text_width(sm, buf), y, buf, ui_col.fg);
		}
		y += sm->h;
		lines--;
	} else if (d->pane[0].rx && d->pane[1].rx && lines == 1) {
		// two receivers, a call on each
		gfx_text(sm, 0, y, TR(DUAL_STATE_RECEIVING_ON_BOTH, "Receiving on A and B"), ui_col.rx);
		y += sm->h;
		lines--;
	} else if (d->rx != UI_DUAL_NONE) {
		// A line per pane receiving, the one heard first; the other (two receivers) says it is muted
		for (int k = 0; k < 2 && lines > 0; k++) {
			uint8_t i = (uint8_t)(k ? !d->rx : d->rx);
			if (!d->pane[i].rx)
				continue;
			sb_init(&b, buf, sizeof buf);
			sb_str(&b, TR(DUAL_STATE_RECEIVING_ON, "Receiving on "));
			sb_str(&b, i ? "B" : "A");
			if (k)
				sb_str(&b, TR(DUAL_STATE_MUTED, ", muted"));
			int x = gfx_text(sm, 0, y, buf, ui_col.rx) + sm->w;
			if (w - x >= 4 * sm->w)
				draw_meter(x, y + sm->h / 4, w - x, sm->h / 2, d->pane[i].rssi_dbm);
			y += sm->h;
			lines--;
		}
	}
	// Then the calls heard, newest first, as many as fit
	if (lines > 0 && d->calls == 0)
		gfx_text(sm, 0, y, TR(EMPTY_NO_CALLS_HEARD, "No calls heard yet"), ui_col.dim);
	for (int k = 0; k < d->calls && k < lines; k++, y += sm->h)
		dual_call(y, d->hist[k].pane, d->hist[k].caller, s->now_ms - d->hist[k].ms, k ? ui_col.dim : ui_col.fg);
}

static const char *dual_hint(const radio_view_t *v, const ui_state_t *s)
{
	const ui_dual_t *d = &s->dual;
	if (v->state == RS_TX_DENIED)
		return deny_action(v);
	if (s->entry)
		return TR(FREQ_HINT_ENTRY, "OK tune  Back delete");
	if (s->locked)
		return TR(LOCK_HINT_HOLD_STAR, TEXT_ALT_OPEN "Keys locked: hold * to unlock"
			TEXT_ALT_NEXT "Locked: hold * to unlock" TEXT_ALT_CLOSE);
	// Talk back on the channel a call came in on: the way back now
	if (d->back_to != UI_DUAL_NONE)
		return d->back_to ?
			TR(DUAL_HINT_BACK_TO_B, TEXT_ALT_OPEN "# 1 back to B  Back stop"
				TEXT_ALT_NEXT "# 1 back to B" TEXT_ALT_CLOSE) :
			TR(DUAL_HINT_BACK_TO_A, TEXT_ALT_OPEN "# 1 back to A  Back stop"
				TEXT_ALT_NEXT "# 1 back to A" TEXT_ALT_CLOSE);
	// A call just came in on the other channel: the way to answer it there
	if (d->last != UI_DUAL_NONE && d->last != d->sel && s->now_ms - d->last_ms < 10000u)
		return d->last ?
			TR(DUAL_HINT_TALK_ON_B, TEXT_ALT_OPEN "# 1 talk on B  Back stop"
				TEXT_ALT_NEXT "# 1 talk on B" TEXT_ALT_CLOSE) :
			TR(DUAL_HINT_TALK_ON_A, TEXT_ALT_OPEN "# 1 talk on A  Back stop"
				TEXT_ALT_NEXT "# 1 talk on A" TEXT_ALT_CLOSE);
	return TR(DUAL_HINT_OPTIONS, "# options  Back stop");
}

// The richest layout that fits avail rows: names in the large font first, then fewer rows, never a smaller font for a
// row than the single-channel Home uses for it (P5). noinline: inlined, the table sits in the frame every pane is
// drawn from, on the UI task's deepest path
static void __attribute__((noinline)) dual_choose(int avail, int gap, dual_fit_t *out, int *pad)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *lg = L->large, *md = L->medium, *sm = L->small;
	const dual_fit_t fits[] = {
		{ lg, md, sm, true, 2 }, { lg, md, sm, false, 2 }, { lg, md, sm, false, 1 },
		{ lg, md, NULL, false, 1 }, { lg, sm, NULL, false, 1 }, { md, sm, NULL, false, 1 },
		{ md, sm, NULL, false, 0 }, { sm, sm, NULL, false, 0 },
	};
	bool tall = gfx_height() >= UI_TALL_PX;
	const dual_fit_t *ft = &fits[sizeof fits / sizeof fits[0] - 1];
	int used = 0;
	for (unsigned k = 0; k < sizeof fits / sizeof fits[0]; k++) {
		used = 2 * dual_pane_h(&fits[k]) + gap + fits[k].act * sm->h + (fits[k].act ? 2 : 0);
		if (used <= avail) {
			ft = &fits[k];
			break;
		}
	}
	int extra = avail - used;
	*pad = extra / 4 < (tall ? 5 : 1) ? extra / 4 : (tall ? 5 : 1);
	if (*pad < 0)
		*pad = 0;
	*out = *ft;
}

void ui_draw_home_dual(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	gfx_clear(ui_col.bg);
	ui_draw_top(v, s, NULL, NULL);
	if (s->large_text) {
		dual_large(v, s);
	} else {
		int top = L->strip_h, gap = gfx_height() >= UI_TALL_PX ? 6 : 2, pad;
		dual_fit_t fit;
		dual_choose(L->hint_y - top, gap, &fit, &pad);
		const dual_fit_t *ft = &fit;
		const font_t *sm = L->small;
		int ph = dual_pane_h(ft) + 2 * pad;
		const font_t *f0 = dual_name_font(s, 0, ft->name), *f1 = dual_name_font(s, 1, ft->name);
		dual_nf = f0->h < f1->h ? f0 : f1;
		if (dual_nf->h < ft->target->h)
			dual_nf = ft->target;	// never smaller than the row under it: a long name is shortened instead
		dual_pane(v, s, 0, top, ph, pad, ft);
		gfx_hline(0, top + ph + gap / 2, w, ui_col.line);
		dual_pane(v, s, 1, top + ph + gap, ph, pad, ft);
		int ay = top + 2 * ph + gap + 2;
		dual_activity(v, s, ay, ft->act ? (L->hint_y - ay) / sm->h : 0);
	}
	ui_draw_banner(s);
	ui_draw_bottom(s, dual_hint(v, s));
}
#endif

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
	if (gfx_text_width(f, bn->title) > w - 4 && bn->name_at > 0 && bn->name_at < sizeof title[0]) {
		// "Call alert from" / "Jodi Ruiz": the station's name together on the second line
		size_t n = bn->name_at;
		while (n > 0 && bn->title[n - 1] == ' ')
			n--;
		memcpy(title[0], bn->title, n);
		title[0][n] = 0;
		nt = 1 + wrap_lines(f, bn->title + bn->name_at, w - 4, 1, &title[1], false);
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
	const char *act = (bn->kind == UI_BANNER_MESSAGE || bn->kind == UI_BANNER_ZONE_OFFER) ?
		TR(BANNER_HINT_OPEN, "# Open") :
		(bn->kind == UI_BANNER_CALL_ALERT || bn->kind == UI_BANNER_PRIVATE) ?
			TR(BANNER_HINT_ACTIONS, "# Actions") : TR(BANNER_HINT_HIDE, "Back hide");
	gfx_text(f, 2, ty, act, gfx_bpp() == 1 ? fg : ui_col.dim);
	if (bn->hold)
		return;	// no countdown: it waits for the user
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
#if CONFIG_LANG_ZH
	// A character at a time: a CJK character may end a line as well as a space, and the width is measured on whole
	// characters, so a line never holds half of one
	int n = 0, cut = 0;
	while (t[n] && t[n] != '\n') {
		int seq;
		uint32_t cp = gfx_utf8_next(t + n, &seq);
		if (n + seq > (int)cap - 1)
			break;
		memcpy(line + n, t + n, (size_t)seq);
		n += seq;
		line[n] = 0;
		if (gfx_text_width(f, line) > max_w) {
			n -= seq;
			line[n] = 0;
			break;
		}
		if (t[n - seq] == ' ' || cp > 0xFF)
			cut = n;
	}
	bool fits_all = t[n] == 0 || t[n] == '\n';
	if (!fits_all && cut > 0)
		n = cut;
	if (n == 0 && t[0] && t[0] != '\n') {
		int seq;
		gfx_utf8_next(t, &seq);		// narrower than one character: still move on
		n = seq < (int)cap ? seq : (int)cap - 1;
	}
	line[n] = 0;
	t += n;
	if (*t == '\n')
		t++;		// an empty line ("\n\n") stays one line
	else
		while (*t == ' ')
			t++;
	return t;
#else
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
#endif
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

// The length of a text line's "Label:" (colon included) when the line starts with one: a colon within its first 20
// characters followed by a space or the line's end. 0 when it has none
static size_t label_len(const char *t)
{
	for (size_t n = 0; n < 20 && t[n] != 0 && t[n] != '\n'; n++) {
		if (t[n] == ':')
			return t[n + 1] == ' ' || t[n + 1] == '\n' || t[n + 1] == 0 ? n + 1 : 0;
	}
	return 0;
}

int ui_draw_text_page(const radio_view_t *v, const ui_state_t *s, const char *title, const char *text,
	uint16_t *first, bool labels)
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
		// a label only where a line of the text starts, never on a wrapped value's next line
		bool starts = t == text || t[-1] == '\n';
		size_t lab = starts && labels ? label_len(t) : 0;
		t = wrap_line(f, t, w, line, sizeof line);
		int r = i - *first;
		if (r < 0)
			continue;
		if (r >= rows)
			break;
		// "Label: value" shows the label dim, and so does a line that is only "Label:" (a list follows). The label is
		// found in the text, not the wrapped line: a wrap right after the colon drops the space
		size_t len = strlen(line);
		color_t dim = gfx_bpp() == 1 ? ui_col.fg : ui_col.dim;
		if (lab > 0 && lab < len) {
			char label[24];
			memcpy(label, line, lab);
			label[lab] = 0;
			int x = gfx_text(f, 0, y + r * f->h, label, dim);
			gfx_text(f, x, y + r * f->h, line + lab, ui_col.fg);
		} else if (lab > 0 && lab == len) {
			gfx_text(f, 0, y + r * f->h, line, dim);
		} else {
			gfx_text(f, 0, y + r * f->h, line, ui_col.fg);
		}
	}
	if (*first > 0)
		gfx_fill(w - 3, y, 3, 2, ui_col.dim);
	if (*first < max_first)
		gfx_fill(w - 3, y + rows * f->h - 2, 3, 2, ui_col.dim);
	ui_draw_banner(s);
	ui_draw_bottom(s, max_first > 0 ? TR(PAGE_HINT_MORE, "Up/Dn more  Back") : TR(HINT_BACK, "Back"));
	return max_first;
}

// s shortened at the end, at a word if one ends late enough, with an ellipsis: for a preview of longer text (a
// message), where the start is what matters
static void fit_end(const font_t *f, const char *s, int max_w, char *out, size_t len)
{
#if CONFIG_LANG_ZH
	// Chinese has no spaces to break at, and a character is not one byte: a line with a wide character is cut by whole
	// characters at the end; Latin text keeps the word cut below
	gfx_text_fit_end(f, s, max_w, out, len);
#else
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
#endif	// CONFIG_LANG_ZH
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
		gfx_text_alt(value, level, val, sizeof val, &more);
		vw = gfx_text_width(f, val);
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

// Where a list's body ends: half the room below y (a message keeps its replies in view), all of it without rows
static int list_body_end(const ui_list_t *l, int y, int y_end)
{
	return l->count ? y + (y_end - y) / 2 + ui_layout.small->h : y_end;
}

bool ui_list_body_fits(const ui_state_t *s, const ui_list_t *l)
{
	const font_t *f = ui_layout.small;
	int y = list_top(NULL, NULL, l, false);
	int y_end = list_body_end(l, y, list_bottom(s));
	char line[64];
	// as wrap_text: a line that doesn't fit is where ui_draw_list shortens the text
	for (const char *t = l->body; t != NULL && *t; y += f->h) {
		if (y + f->h > y_end)
			return false;
		t = wrap_line(f, t, gfx_width(), line, sizeof line);
	}
	return true;
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
		y = wrap_text(f, l->body, y, list_body_end(l, y, y_end), true, ui_col.fg);
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
			fg = gfx_bpp() == 1 ? ui_col.bg : (it.flags & UI_ITEM_EDIT) ? ui_col.pill_dark_fg : ui_col.focus_fg;
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
			str_copy(val, sizeof val, it.value);
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
	color_t fg = gfx_bpp() == 1 ? ui_col.fg : ui_col.pill_fg;	// the words on the TX color, as on the TX pill
	int y = ui_draw_top(v, s, TR(TITLE_EMERGENCY, "Emergency"), NULL) + 2;
	const char *word = TR(EMERG_STATE_EMERGENCY, "EMERGENCY");
	const font_t *big = gfx_text_width(L->large, word) <= w ? L->large : L->medium;
	if (gfx_bpp() == 1) {
		gfx_fill(0, y, w, big->h, ui_col.fg);
		gfx_text(big, (w - gfx_text_width(big, word)) / 2, y, word, ui_col.bg);
	} else {
		gfx_text(big, (w - gfx_text_width(big, word)) / 2, y, word, fg);
	}
	y += big->h + 2;
	sb_init(&b, buf, sizeof buf);
	if (name != NULL && name[0])
		sb_str(&b, name);
	else
		sb_u32(&b, id, 1);
	char fit[40];
	gfx_text_fit(L->medium, buf, w, fit, sizeof fit);
	gfx_text(L->medium, (w - gfx_text_width(L->medium, fit)) / 2, y, fit, fg);
	y += L->medium->h;
	if (name != NULL && name[0] && y + L->small->h <= L->hint_y) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "ID ");
		sb_u32(&b, id, 1);
		gfx_text(L->small, (w - gfx_text_width(L->small, buf)) / 2, y, buf, fg);
	}
	ui_draw_hint(s->toast != NULL ? s->toast : TR(EMERG_HINT, "OK acknowledge  Back hide"), s->toast != NULL);
}

// The volume step as RADIO_VOLUME_MAX segments in w px, filled up to step: the volume screen and the knob's bar
static void volume_segments(int x, int y, int w, int h, int step)
{
	int pitch = w / RADIO_VOLUME_MAX, gap = pitch >= 6 ? 2 : 1;
	x += (w - pitch * RADIO_VOLUME_MAX) / 2;
	for (int i = 0; i < RADIO_VOLUME_MAX; i++) {
		if (i < step)
			gfx_fill(x + i * pitch, y, pitch - gap, h, ui_col.fg);
		else
			gfx_rect(x + i * pitch, y, pitch - gap, h, ui_col.line);
	}
}

#if CONFIG_VOLUME_BAR
// noinline: inlined into the frame code, its locals land in ui_task's deepest path, which has 8 bytes to spare on
// the boards with the tightest UI stack (make stack-check)
__attribute__((noinline)) void ui_draw_volume_bar(const ui_state_t *s, bool top)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	// Rows no screen lays its content in, so nothing is cut on any display class: the hint row (its keys and toasts
	// come back with it), or on a screen that fills the bottom (Quick's tiles, a list with hints off) the status strip
	int y = top ? 0 : L->hint_y, y0 = top ? 0 : y - 1, bh = top ? L->strip_h : gfx_height() - y0;
	char num[4];
	sbuf_t b;
	sb_init(&b, num, sizeof num);
	sb_u32(&b, (uint32_t)s->volbar, 1);
	const char *label = TR(VOL_LABEL_SHORT, "Vol");
	int lw = gfx_text_width(f, label), nw = gfx_text_width(f, "16");
	gfx_fill(0, y0, w, bh, ui_col.bg);
	gfx_hline(0, top ? y0 + bh - 1 : y0, w, ui_col.line);
	gfx_text(f, 2, y, label, ui_col.fg);
	gfx_text(f, w - 2 - gfx_text_width(f, num), y, num, ui_col.fg);
	volume_segments(lw + 6, y + 1, w - lw - nw - 12, f->h - 2, s->volbar);
}
#endif

// The volume screen: the step as a number (a word at 0) over a bar of RADIO_VOLUME_MAX segments.
// Large type, no scale to read
void ui_draw_volume(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	char buf[16];
	sbuf_t b;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(v, s, TR(TITLE_VOLUME, "Volume"), NULL) + 4;
	sb_init(&b, buf, sizeof buf);
	if (s->volume == 0)
		sb_str(&b, TR(VOL_VALUE_MUTED, "Muted"));
	else
		sb_u32(&b, s->volume, 1);
	const font_t *big = L->large;
	if (gfx_text_width(big, buf) > w)
		big = L->medium;
	gfx_text(big, (w - gfx_text_width(big, buf)) / 2, y, buf, ui_col.fg);
	y += big->h + 6;
	volume_segments(4, y, w - 8, L->row_h > 10 ? L->row_h : 10, s->volume);
	ui_draw_bottom(s, TR(VOL_HINT, "Up Down change  OK done"));
}

// --- Hotspot ---
#if CONFIG_HOTSPOT
void ui_draw_hotspot(const radio_view_t *v, const hotspot_view_t *h, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	char buf[48];
	sbuf_t b;

	gfx_clear(ui_col.bg);
	// the strip's pill shows what the hotspot does
	int y = draw_top_as(v, h->transmitting ? RS_TX : h->receiving ? RS_RX : RS_IDLE, s,
		TR(TITLE_HOTSPOT, "Hotspot"), NULL) + 1;
	const char *word = !h->running ? TR(HOTSPOT_STATE_STARTING, "STARTING") : h->transmitting ?
		TR(HOTSPOT_STATE_TX_NET, "TX NET") : h->receiving ? TR(HOTSPOT_STATE_RX_RF, "RX RF") :
		TR(HOTSPOT_STATE_READY, "READY");
	const font_t *big = gfx_text_width(L->large, word) <= w ? L->large : L->medium;
	gfx_text_fit(big, word, w, buf, sizeof buf);	// a translation too wide even for the medium font
	gfx_text(big, 0, y, buf, h->transmitting ? ui_col.tx : h->receiving ? ui_col.rx : ui_col.fg);
	y += big->h + 1;

	sb_init(&b, buf, sizeof buf);
	if (h->rx_hz == 0) {
		sb_str(&b, TR(HOTSPOT_BODY_NO_FREQUENCY, "No frequency yet"));
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
	sb_str(&b, ui_power_name(v->power, v->ch.tx_hz));
	y = call_line(L->small, y, buf, ui_col.dim);
	sb_init(&b, buf, sizeof buf);
	if (h->src != 0) {
		sb_str(&b, h->from_net ? TR(HOTSPOT_BODY_FROM_NET, "Net ") : "RF ");
		sb_u32(&b, h->src, 1);
		sb_str(&b, " " GLYPH_ARROW " ");
		if (h->group)
			sb_str(&b, "TG ");
		sb_u32(&b, h->dst, 1);
	} else {
		sb_str(&b, TR(HOTSPOT_EMPTY_NO_CALLS, "No calls yet"));
	}
	call_line(L->small, y, buf, ui_col.fg);
	ui_draw_hint(s->toast != NULL ? s->toast : TR(HOTSPOT_HINT_OK_STOPS, "OK stop hotspot"), s->toast != NULL);
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
	if (!v->vfo && pos == 3)
		return QT_TO_VFO;	// a channel's tile 4 copies it to the VFO; the VFO's stays Mode
	uint8_t kind = v->ch.mode == RADIO_MODE_DMR ? dmr[pos] : fm[pos];
	if (v->vfo && (kind == QT_MONITOR || kind == QT_PRIVACY))
		return QT_SPLIT;	// the VFO's TX frequency; Monitor stays on its key, privacy on channels
	return kind;
}

static bool feat(uint32_t f)
{
	return (ui_svc_features() & f) != 0;
}

bool ui_quick_tile(int tile, const radio_view_t *v, const ui_state_t *s, char *label, size_t label_len,
	char *value, size_t value_len)
{
	// The whole word, and an abbreviation of 6 characters at most for a tile (font.h alternates), so a tile's name
	// never runs into the next tile's digit in the label font. Written out (no macro): tools/i18n.py reads the marks
	static const tr_id_t names[] = {
		[QT_POWER] = TRN(QUICK_LABEL_POWER, TEXT_ALT_OPEN "Power" TEXT_ALT_NEXT "Pwr" TEXT_ALT_CLOSE),
		[QT_SQUELCH] = TRN(QUICK_LABEL_SQUELCH, TEXT_ALT_OPEN "Squelch" TEXT_ALT_NEXT "SQL" TEXT_ALT_CLOSE),
		[QT_TONE] = TRN(QUICK_LABEL_TONE, "Tone"), [QT_MODE] = TRN(LABEL_MODE, "Mode"),
		[QT_MONITOR] = TRN(QUICK_LABEL_MONITOR, TEXT_ALT_OPEN "Monitor" TEXT_ALT_NEXT "MON" TEXT_ALT_CLOSE),
		[QT_VOX] = TRX(WORD_VOX, "VOX"),
#if CONFIG_DUAL_WATCH
		// tile 7 is Dual watch; a zone scan is in Tools > Scan
		[QT_SCAN] = TRN(QUICK_LABEL_DUAL_WATCH, TEXT_ALT_OPEN "Dual watch" TEXT_ALT_NEXT "Dual" TEXT_ALT_CLOSE),
#else
		[QT_SCAN] = TRN(WORD_SCAN, "Scan"),
#endif
		[QT_BANDWIDTH] = TRN(QUICK_LABEL_WIDTH, "Width"), [QT_BRIGHTNESS] = TRN(QUICK_LABEL_BRIGHT, "Bright"),
		[QT_TIMESLOT] = TRN(QUICK_LABEL_TIMESLOT, TEXT_ALT_OPEN "Timeslot" TEXT_ALT_NEXT "Slot" TEXT_ALT_CLOSE),
		[QT_CC] = TRN(QUICK_LABEL_COLOR, "Color"),
		[QT_HEAR_ALL] = TRN(QUICK_LABEL_DMON, TEXT_ALT_OPEN "Digital monitor"
			TEXT_ALT_NEXT "DMon" TEXT_ALT_CLOSE),
		[QT_PRIVACY] = TRN(QUICK_LABEL_PRIVACY, TEXT_ALT_OPEN "Privacy" TEXT_ALT_NEXT "Priv" TEXT_ALT_CLOSE),
		[QT_TALK_TO] = TRN(QUICK_LABEL_TALK_TO, TEXT_ALT_OPEN "Talk to" TEXT_ALT_NEXT "Talk" TEXT_ALT_CLOSE),
		[QT_TO_VFO] = TRN(QUICK_LABEL_COPY_TO_VFO, TEXT_ALT_OPEN "Copy to VFO"
			TEXT_ALT_NEXT "To VFO" TEXT_ALT_CLOSE),
		[QT_SPLIT] = TRN(QUICK_LABEL_TX_FREQ, TEXT_ALT_OPEN "TX freq" TEXT_ALT_NEXT "TXFreq" TEXT_ALT_CLOSE),
	};
	uint8_t kind = ui_quick_kind(tile, v);
	bool editing = s->quick_edit && s->quick_focus == tile;
	sbuf_t b;
	sb_init(&b, label, label_len);
	sb_chr(&b, (char)('1' + tile));
	sb_str(&b, tr(names[kind]));
	sb_init(&b, value, value_len);
	switch (kind) {
	case QT_POWER:
		if (v->power != channel_power(v))
			sb_str(&b, GLYPH_DOT);
		sb_str(&b, ui_power_name(v->power, v->ch.tx_hz));
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
			sb_str(&lb, s->tone_target == RADIO_TONE_TX ? TR(QUICK_LABEL_TONE_TX, "Tone TX") :
				TR(QUICK_LABEL_TONE_RX, "Tone RX"));
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
			sb_str(&b, TR(VALUE_NONE, "None"));
		else
			sb_tone(&b, t);
		return v->ch.mode == RADIO_MODE_FM;
	}
	case QT_MODE:
		sb_str(&b, mode_word(v));	// what it is, as every other tile
		// Only in the VFO (a channel's tile 4 is QT_TO_VFO), in a build with both VFO kinds; it keeps the mode like
		// its other settings
		return feat(UI_FEAT_MODE) && v->vfo && CONFIG_VFO_FM && CONFIG_VFO_DMR;
	case QT_TO_VFO:
		sb_str(&b, TR(QUICK_VALUE_COPY, "Copy"));
		return CONFIG_VFO_FM || CONFIG_VFO_DMR;	// a channel the VFO can't hold is refused on the press, with the reason
	case QT_SPLIT:
		if (editing && s->entry_len > 0) {
			sb_str(&b, "TX");	// the typed frequency is too wide for a tile: the hint row has it whole
		} else {
			sb_split(&b, v->ch.rx_hz, v->ch.tx_hz);
		}
		return v->vfo && (CONFIG_VFO_FM || CONFIG_VFO_DMR);
	case QT_MONITOR:
		sb_str(&b, v->monitor ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
		return feat(UI_FEAT_MONITOR_LATCH);
	case QT_VOX:
		sb_str(&b, v->vox_on ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
		return feat(UI_FEAT_VOX) && v->ch.mode == RADIO_MODE_FM;
	case QT_SCAN:
#if CONFIG_DUAL_WATCH
		sb_str(&b, s->dual.on ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
#else
		sb_str(&b, s->scanning ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
#endif
		return feat(UI_FEAT_SCAN);
	case QT_BANDWIDTH:
		if (v->temp & RADIO_TEMP_BW)
			sb_str(&b, GLYPH_DOT);
		sb_str(&b, v->ch.bw == RADIO_BW_25K ? "25k" : "12.5k");
		return v->ch.mode == RADIO_MODE_FM;
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
		sb_str(&b, s->hear_all ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
		return feat(UI_FEAT_HEAR_ALL);
	case QT_PRIVACY:
		sb_str(&b, s->privacy ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
		return feat(UI_FEAT_PRIVACY) && CONFIG_DMR_PRIVACY;
	case QT_TALK_TO:
		if (v->dmr.target_private)
			sb_u32(&b, v->dmr.target_id, 1);
		else if (v->dmr.target_id == 0xFFFFFFu)
			sb_str(&b, TR(QUICK_VALUE_ALL, "All"));
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
	str_copy(it->label, sizeof it->label, label + 1);	// the whole word: a list row has room for it
	if (!ok)
		it->flags |= UI_ITEM_DIM;
	if (quick_s->quick_edit && quick_s->quick_focus == i)
		it->flags |= UI_ITEM_EDIT;
}

// The hint while the TX freq tile edits: what Up / Down and the digits do, or the TX frequency typed so far, whole
// (blanks for the digits to come); NULL when that tile isn't editing
static const char *quick_split_hint(const radio_view_t *v, const ui_state_t *s, char *buf, size_t len)
{
	if (!s->quick_edit || ui_quick_kind(s->quick_focus, v) != QT_SPLIT)
		return NULL;
	if (s->entry_len == 0)
		return TR(QUICK_HINT_SPLIT, TEXT_ALT_OPEN "Up/Dn: +/- offset  Type: TX freq"
			TEXT_ALT_NEXT "Up/Dn +/- offset  0-9 TX freq"
			TEXT_ALT_NEXT "Up/Dn offset  0-9 TX freq" TEXT_ALT_NEXT "Up/Dn +/-  0-9 TX freq" TEXT_ALT_CLOSE);
	sbuf_t b;
	sb_init(&b, buf, len);
	sb_str(&b, "TX ");
	for (uint8_t i = 0; i < UI_ENTRY_MAX; i++) {
		if (i == UI_ENTRY_MHZ)
			sb_chr(&b, '.');
		sb_chr(&b, i < s->entry_len ? s->entry_digits[i] : '_');
	}
	sb_str(&b, TR(QUICK_HINT_OK_SETS, "  OK sets"));
	return buf;
}

// 128x64: a numbered list with the same numbers (wireframe 128x64). The list and the tiles each have their own
// frame, so the list's deep draw doesn't sit on the tiles' (the ui task's stack is tight)
static void __attribute__((noinline)) draw_quick_list(const radio_view_t *v, const ui_state_t *s)
{
	static uint16_t top;
	char hint[24];
	const char *split_hint = quick_split_hint(v, s, hint, sizeof hint);
	quick_v = v;
	quick_s = s;
	quick_list_focus = s->quick_focus;
	ui_list_t l = { TR(QUICK_TITLE, "Quick"), v->vfo ? "VFO" : v->ch.name, NULL, NULL, UI_QUICK_TILES,
		quick_list_focus, &top,
			quick_list_item, NULL, split_hint != NULL ? split_hint :
				TR(QUICK_HINT, "1-9 change  Hold OK save") };
	ui_draw_list(v, s, &l);
}

static void __attribute__((noinline)) draw_quick_tiles(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width(), h = gfx_height();
	const char *name = v->vfo ? "VFO" : v->ch.name;
	char label[24];	// a tile's label, and the hint's text after the tiles (one buffer: the ui task's stack is tight)
	const char *split_hint = quick_split_hint(v, s, label, sizeof label);

	gfx_clear(ui_col.bg);
	// Title row keeps the state pill (P2)
	int top = ui_draw_top(v, s, TR(QUICK_TITLE, "Quick"), name);
	int bottom = h;
	if (s->toast != NULL || split_hint != NULL || h >= UI_TALL_PX)
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
		char value[12], fit[12];	// label: "5" and the name's alternates
		bool ok = ui_quick_tile(i, v, s, label, sizeof label, value, sizeof value);
		bool focus = i == s->quick_focus;
		color_t fg = ok ? ui_col.fg : ui_col.line, bg = ui_col.bg;
		if (gfx_bpp() == 1 && !ok)
			fg = ui_col.fg;
		if (focus) {
			bg = s->quick_edit ? ui_col.warn : ui_col.focus;
			fg = gfx_bpp() == 1 ? ui_col.bg : s->quick_edit ? ui_col.pill_dark_fg : ui_col.focus_fg;
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
	else if (split_hint != NULL)
		ui_draw_hint(quick_split_hint(v, s, label, sizeof label), false);	// the tiles used label
	else if (h >= UI_TALL_PX)
		ui_draw_hint(TR(QUICK_HINT_ALL_SETTINGS, "0 All settings  Hold OK save"), false);
}

void ui_draw_quick(const radio_view_t *v, const ui_state_t *s)
{
	if (gfx_width() < 150)
		draw_quick_list(v, s);
	else
		draw_quick_tiles(v, s);
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
	gfx_text(f, 0, y, TR(TITLE_DIAGNOSTICS, "Diagnostics"), ui_col.warn);
	y += f->h;
	// On every page, at most two lines: a translation may be longer than one
	if (board_variant_mismatch())
		y = wrap_text(f, TR(DIAG_VARIANT_MISMATCH, "This firmware may be for another version of this radio"), y,
			y + 2 * f->h, true, ui_col.warn);

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
	char named[32];
	gfx_text(f, 0, gfx_height() - f->h, key_names(TR(DIAG_HINT, "* more  Back"), named, sizeof named, true),
		ui_col.idle);
}
#endif
