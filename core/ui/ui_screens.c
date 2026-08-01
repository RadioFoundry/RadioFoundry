// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Home, quick settings and the boot splash, laid out from the display size (docs/ux/information-architecture.md 3, 5;
// docs/ux/wireframes). Regions top to bottom: status strip, place, talk target, activity, hint row.
#include <string.h>
#include "board.h"
#include "ui_screens.h"
#include "fmt.h"

ui_layout_t ui_layout;

// Colors carry meaning only (P5); every color cue also has a word. On 1 bpp panels everything is ink on paper
// and pills are inverse video
static struct {
	color_t bg, fg, dim, line, tx, rx, warn, idle, pill_fg, pill_dark_fg, focus, hint_bg;
} col;

void ui_layout_init(void)
{
	int w = gfx_width(), h = gfx_height();
	ui_layout_t *L = &ui_layout;

	if (gfx_bpp() == 1) {
		col.bg = 0; col.fg = col.dim = col.line = col.tx = col.rx = col.warn = col.idle = col.focus = 1;
		col.pill_fg = col.pill_dark_fg = 0;
		col.hint_bg = 0;
	} else {
		col.bg = RGB565(0, 0, 0);
		col.fg = RGB565(255, 255, 255);
		col.dim = RGB565(168, 168, 168);
		col.line = RGB565(72, 72, 72);
		col.tx = RGB565(224, 32, 32);
		col.rx = RGB565(40, 184, 72);
		col.warn = RGB565(240, 168, 0);
		col.idle = RGB565(96, 96, 96);
		col.pill_fg = RGB565(255, 255, 255);
		col.pill_dark_fg = RGB565(0, 0, 0);
		col.focus = RGB565(32, 96, 200);
		col.hint_bg = RGB565(32, 32, 32);
	}

	// Display classes (P10): about 128 px wide, about 160 px wide, 240 px and wider
	if (w >= 220) {
		L->tiny = &font_6x12; L->small = &font_8x16; L->medium = &font_12x24; L->large = &font_16x32;
	} else if (w >= 150) {
		L->tiny = &font_5x8; L->small = &font_6x12; L->medium = &font_8x16; L->large = &font_12x24;
	} else {
		L->tiny = &font_5x8; L->small = &font_5x8; L->medium = &font_6x12; L->large = &font_8x16;
	}

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
}

// --- shared pieces ---

static const char *power_name(uint8_t p)
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

// Status strip (P2): state pill, mode, monitor, signal, battery. Returns the x after the left part
static int draw_strip(const radio_view_t *v, const ui_state_t *s, const char *title)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	int ph = L->strip_h - 1;
	const char *word = "IDLE";
	color_t pbg = col.idle, pfg = col.pill_fg;
	switch (v->state) {
	case RS_RX: word = "RX"; pbg = col.rx; pfg = col.pill_dark_fg; break;
	case RS_TX: word = "TX"; pbg = col.tx; break;
	case RS_TX_DENIED: word = "NO TX"; pbg = col.warn; pfg = col.pill_dark_fg; break;
	default: break;
	}
	if (gfx_bpp() == 1)
		pfg = col.pill_fg;
	gfx_fill(0, 0, w, L->strip_h, col.bg);
	int x = gfx_pill(f, 0, 0, ph, word, pfg, pbg) + f->w / 2;
	int ty = (ph - f->h + 1) / 2;

	// Right side, right to left: battery %, battery icon, signal bars
	char pct[6];
	sbuf_t b;
	sb_init(&b, pct, sizeof pct);
	sb_u32(&b, s->battery_pct, 1);
	sb_chr(&b, '%');
	int rx = w - gfx_text_width(f, pct);
	gfx_text(f, rx, ty, pct, s->battery_pct <= 15 ? col.warn : col.fg);
	int bh = f->ascent - 2 < 5 ? 5 : f->ascent - 2;
	int bw = bh * 2;
	rx -= bw + 2;
	gfx_icon_battery(rx, ty + f->ascent - bh, bw, bh, s->battery_pct, col.fg, s->battery_pct <= 15 ? col.warn : col.fg);
	int sw = bh + 4;
	rx -= sw + f->w / 2;
	gfx_icon_bars(rx, ty + f->ascent - bh - 1, sw, bh + 1, 4, rssi_bars(v->rssi_dbm), col.fg, col.line);

	if (title != NULL) {
		char fit[32];
		gfx_text_fit(f, title, rx - f->w / 2 - x, fit, sizeof fit);
		x = gfx_text(f, x, ty, fit, col.fg);
	} else {
		x = gfx_text(f, x, ty, v->ch.mode == RADIO_MODE_DMR ? "DMR" : "FM", col.fg);
		if (v->monitor && x + 4 * f->w <= rx)
			x = gfx_text(f, x + f->w, ty, "MON", col.warn);
	}
	return x;
}

static void draw_hint(const char *text, bool toast)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	char fit[48];
	color_t bg = toast ? col.fg : col.hint_bg;
	color_t fg = toast ? col.bg : col.dim;
	if (gfx_bpp() == 1 && !toast)
		fg = col.fg;
	gfx_fill(0, L->hint_y, w, gfx_height() - L->hint_y, bg);
	if (gfx_bpp() == 1 && !toast)
		gfx_hline(0, L->hint_y, w, col.line);
	gfx_text_fit(L->small, text, w - 2, fit, sizeof fit);
	gfx_text(L->small, toast ? (w - gfx_text_width(L->small, fit)) / 2 : 1, L->hint_y, fit, fg);
}

// --- boot splash ---

void ui_draw_splash(const char *product, const char *board_name, const char *git)
{
	int w = gfx_width(), h = gfx_height();
	const ui_layout_t *L = &ui_layout;
	gfx_clear(col.bg);
	const font_t *big = gfx_text_width(L->large, product) <= w ? L->large :
		gfx_text_width(L->medium, product) <= w ? L->medium : L->small;
	int total = big->h + 4 + L->small->h * 2;
	int y = (h - total) / 2;
	gfx_text(big, (w - gfx_text_width(big, product)) / 2, y, product, col.fg);
	y += big->h + 4;
	gfx_hline(w / 4, y - 3, w / 2, col.focus);
	gfx_text(L->small, (w - gfx_text_width(L->small, board_name)) / 2, y, board_name, col.dim);
	y += L->small->h;
	char line[24];
	sbuf_t b;
	sb_init(&b, line, sizeof line);
	sb_str(&b, "git ");
	sb_str(&b, git);
	gfx_text(L->small, (w - gfx_text_width(L->small, line)) / 2, y, line, col.dim);
}

// --- Home ---

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
		gfx_text(L->small, w - rw, L->zone_y, right, col.dim);
		const char *left = v->vfo ? "VFO" : v->zone_name;
		gfx_text_fit(L->small, left, w - rw - L->small->w, fit, sizeof fit);
		gfx_text(L->small, 0, L->zone_y, fit, v->vfo ? col.warn : col.fg);
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
		int x = gfx_text(L->large, 0, y, buf, s->entry ? col.warn : col.fg);
		gfx_text(L->small, x + L->small->w / 2, base - L->small->ascent, "MHz", col.dim);
		if (L->zone_y < 0 && !s->entry)
			gfx_text(L->small, w - 3 * L->small->w, base - L->small->ascent, "VFO", col.dim);
		return;
	}

	// Channel number (medium, dim) and name, baselines aligned. The name uses the largest font it fits in; only a name
	// too long even for the small font is shortened from the middle
	sb_init(&b, buf, sizeof buf);
	sb_u32(&b, v->channel_number, 1);
	int x = gfx_text(L->medium, 0, base - L->medium->ascent, buf, col.dim) + L->medium->w / 2;
	int avail = w - x;
	if (v->temporary)
		avail -= L->medium->w;
	const font_t *nf = L->large;
	if (gfx_text_width(nf, v->ch.name) > avail)
		nf = L->medium;
	if (gfx_text_width(nf, v->ch.name) > avail)
		nf = L->small;
	gfx_text_fit(nf, v->ch.name, avail, fit, sizeof fit);
	x = gfx_text(nf, x, nf == L->large ? y : base - nf->ascent, fit, col.fg);
	if (v->temporary)
		gfx_text(L->medium, x, base - L->medium->ascent, GLYPH_DOT, col.warn);
}

static void draw_talk(const radio_view_t *v)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->medium;
	char buf[40];
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "PTT" GLYPH_ARROW " ");
	int x = gfx_text(f, 0, L->talk_y, buf, col.dim);
	sb_init(&b, buf, sizeof buf);
	color_t c = col.fg;
	if (v->ch.mode == RADIO_MODE_DMR) {
		sb_str(&b, "DMR comes later");
		c = col.warn;
	} else if (v->ch.rx_only) {
		sb_str(&b, "RX only");
		c = col.warn;
	} else if (v->ch.tx_hz == 0) {
		sb_str(&b, "no TX freq");
		c = col.warn;
	} else {
		sb_freq(&b, v->ch.tx_hz);
		sb_str(&b, " MHz");
	}
	char fit[40];
	gfx_text_fit(f, buf, gfx_width() - x, fit, sizeof fit);
	gfx_text(f, x, L->talk_y, fit, c);
}

// Chips: tone, bandwidth, power, squelch. A dot marks a temporary change
static void draw_chips(const radio_view_t *v)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	char buf[48];
	sbuf_t b;
	int x = 0, y = L->chips_y;

	sb_init(&b, buf, sizeof buf);
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
	x = gfx_text(f, x, y, buf, col.fg) + f->w;
	x = gfx_text(f, x, y, v->ch.bw == RADIO_BW_25K ? "25k" : "12.5k", col.fg) + f->w;
	bool pw_temp = v->power != channel_power(v);
	if (pw_temp)
		x = gfx_text(f, x, y, GLYPH_DOT, col.warn);
	x = gfx_text(f, x, y, power_name(v->power), pw_temp ? col.warn : col.fg) + f->w;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "SQ");
	sb_u32(&b, v->squelch, 1);
	if (x + gfx_text_width(f, buf) <= w)
		gfx_text(f, x, y, buf, col.fg);
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
			gfx_fill(x + i * pitch, y, pitch - 1, h, col.rx);
		else if (gfx_bpp() == 1)
			gfx_rect(x + i * pitch, y, pitch - 1, h, col.line);
		else
			gfx_fill(x + i * pitch, y, pitch - 1, h, col.line);
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
		sb_str(&b, "Outside ");
		sb_str(&b, bands);
		break;
	}
	case TXDENY_TIMEOUT: sb_str(&b, "TX time-out reached"); break;
	case TXDENY_NO_FREQ: sb_str(&b, "No TX frequency set"); break;
	default: sb_str(&b, "Not allowed here"); break;
	}
	return buf;
}

static const char *deny_action(const radio_view_t *v)
{
	switch (v->tx_deny) {
	case TXDENY_BAND: return v->vfo ? "Tune into a band, then PTT" : "Pick another channel";
	case TXDENY_TIMEOUT: return "Release PTT to talk again";
	default: return "Pick another channel";
	}
}

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
	char buf[48], fit[48];
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);

	switch (v->state) {
	case RS_RX: {
		sb_i32(&b, v->rssi_dbm);
		sb_str(&b, " dBm");
		int tw = gfx_text_width(big, buf);
		gfx_text(big, w - tw, y, buf, col.fg);
		draw_meter(0, y + big->h / 4, w - tw - big->w, big->h / 2, v->rssi_dbm);
		if (two)
			gfx_text(sm, 0, y2, v->monitor ? "Monitor (squelch open)" : "Receiving", col.rx);
		break;
	}
	case RS_TX: {
		sb_mmss(&b, v->tx_elapsed_ms / 1000u);
		int tw = gfx_text_width(big, buf);
		gfx_text(big, w - tw, y, buf, col.fg);
		gfx_text(big, 0, y, "TALKING", col.tx);
		if (two) {
			sb_init(&b, buf, sizeof buf);
			if (v->tot_s) {
				sb_str(&b, "TOT ");
				sb_mmss(&b, v->tot_s);
				uint32_t el = v->tx_elapsed_ms / 1000u;
				sb_str(&b, "  left ");
				sb_mmss(&b, el < v->tot_s ? v->tot_s - el : 0);
			} else {
				sb_str(&b, "No TX time-out");
			}
			gfx_text(sm, 0, y2, buf, col.dim);
		}
		break;
	}
	case RS_TX_DENIED:
		// One line only: the pill already says NO TX, so the line gives the reason
		gfx_text_fit(sm, deny_reason(v, buf, sizeof buf), w, fit, sizeof fit);
		if (two) {
			gfx_text(big, 0, y, "Can't transmit", col.warn);
			gfx_text(sm, 0, y2, fit, col.fg);
		} else {
			gfx_text(sm, 0, y, fit, col.warn);
		}
		break;
	default:
		if (v->last_rx_ms) {
			sb_str(&b, "Last activity ");
			sb_age(&b, s->now_ms - v->last_rx_ms);
		} else {
			sb_str(&b, "No activity yet");
		}
		gfx_text(sm, 0, y, buf, col.dim);
		if (v->monitor && L->activity_h >= 2 * sm->h)
			gfx_text(sm, 0, y + sm->h, "Monitor: squelch open", col.warn);
		break;
	}
}

static const char *home_hint(const radio_view_t *v, const ui_state_t *s)
{
	if (v->state == RS_TX_DENIED)
		return deny_action(v);
	if (s->entry)
		return "OK tune  Back delete";
	if (v->temporary)
		return "Back undo edits  * Quick";
	if (v->vfo)
		return "0-9 freq  * Quick  Up/Dn";
	return "Up/Dn channel  * Quick";
}

void ui_draw_home(const radio_view_t *v, const ui_state_t *s)
{
	gfx_clear(col.bg);
	draw_strip(v, s, NULL);
	draw_place(v, s);
	draw_talk(v);
	draw_chips(v);
	if (ui_layout.sep_y >= 0)
		gfx_hline(0, ui_layout.sep_y, gfx_width(), col.line);
	draw_activity(v, s);
	if (s->toast != NULL)
		draw_hint(s->toast, true);
	else
		draw_hint(home_hint(v, s), false);
}

// --- Quick settings (3x3 grid, positions match the keypad) ---

bool ui_quick_tile(int tile, const radio_view_t *v, const ui_state_t *s, char *label, size_t label_len,
	char *value, size_t value_len)
{
	static const char *const names[UI_QUICK_TILES] = {
		"Power", "Squelch", "Tone", "Mode", "Monitor", "VOX", "VFO", "Width", "Bright",
	};
	sbuf_t b;
	sb_init(&b, label, label_len);
	sb_chr(&b, (char)('1' + tile));
	sb_str(&b, names[tile]);
	sb_init(&b, value, value_len);
	switch (tile) {
	case QT_POWER:
		if (v->power != channel_power(v))
			sb_str(&b, GLYPH_DOT);
		sb_str(&b, power_name(v->power));
		return true;
	case QT_SQUELCH:
		sb_u32(&b, v->squelch, 1);
		return true;
	case QT_TONE:
		if (v->ch.tx_tone.type == TONE_NONE)
			sb_str(&b, "None");
		else
			sb_tone(&b, &v->ch.tx_tone);
		return false;
	case QT_MODE:
		sb_str(&b, v->ch.mode == RADIO_MODE_DMR ? "DMR" : "FM");
		return false;
	case QT_MONITOR:
		sb_str(&b, v->monitor ? "On" : "Off");
		return false;
	case QT_VOX:
		sb_str(&b, "Off");
		return false;
	case QT_VFO:
		sb_str(&b, v->vfo ? "On" : "Off");
		return true;
	case QT_BANDWIDTH:
		sb_str(&b, v->ch.bw == RADIO_BW_25K ? "25k" : "12.5k");
		return false;
	case QT_BRIGHTNESS:
		sb_u32(&b, s->brightness, 1);
		sb_chr(&b, '%');
		return true;
	default:
		return false;
	}
}

void ui_draw_quick(const radio_view_t *v, const ui_state_t *s)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width(), h = gfx_height();
	gfx_clear(col.bg);

	// Title row keeps the state pill (P2)
	char title[CODEPLUG_NAME_LEN + 8];
	sbuf_t b;
	sb_init(&b, title, sizeof title);
	sb_str(&b, "Quick ");
	sb_str(&b, v->vfo ? "VFO" : v->ch.name);
	draw_strip(v, s, title);

	int top = L->strip_h;
	int gh = h - top;
	if (s->toast != NULL)
		gh = L->hint_y - top;
	const font_t *lf = L->small;
	const font_t *vf = L->medium;
	if (gh / 3 < lf->h + vf->h + 2)
		vf = lf;
	if (gh / 3 < lf->h + vf->h)
		lf = L->tiny;
	for (int i = 0; i < UI_QUICK_TILES; i++) {
		int cx = i % 3, cy = i / 3;
		int x0 = w * cx / 3, x1 = w * (cx + 1) / 3;
		int y0 = top + gh * cy / 3, y1 = top + gh * (cy + 1) / 3;
		char label[12], value[12], fit[12];
		bool ok = ui_quick_tile(i, v, s, label, sizeof label, value, sizeof value);
		bool focus = i == s->quick_focus;
		color_t fg = ok ? col.fg : col.line, bg = col.bg;
		if (gfx_bpp() == 1 && !ok)
			fg = col.fg;
		if (focus) {
			bg = s->quick_edit ? col.warn : col.focus;
			fg = gfx_bpp() == 1 ? col.bg : s->quick_edit ? col.pill_dark_fg : col.fg;
			gfx_fill(x0 + 1, y0 + 1, x1 - x0 - 1, y1 - y0 - 1, bg);
		}
		gfx_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, col.line);
		gfx_clip(x0 + 1, y0 + 1, x1 - x0 - 1, y1 - y0 - 1);
		gfx_text(lf, x0 + 2, y0 + 1, label, focus ? fg : ok ? col.dim : fg);
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
		gfx_text_fit(vf, value, x1 - x0 - 3, fit, sizeof fit);
		int vx = x0 + (x1 - x0 + 1 - gfx_text_width(vf, fit)) / 2;
		int vy = y0 + 1 + lf->h + (y1 - y0 - 1 - lf->h - vf->h) / 2;
		gfx_text(vf, vx, vy, fit, fg);
		gfx_clip_off();
	}
	if (s->toast != NULL)
		draw_hint(s->toast, true);
}
