// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Home, quick settings and the boot splash, laid out from the display size (docs/ux/information-architecture.md 3, 5;
// docs/ux/wireframes). Regions top to bottom: status strip, place, talk target, activity, hint row.
#include <string.h>
#include "board.h"
#include "ui_screens.h"
#include "fmt.h"
#include "config.h"

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
		L->tiny = &font_6x12; L->small = &font_8x16; L->medium = &font_12x24; L->large = FONT_LARGEST;
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

#if CONFIG_DMR
static void draw_talk_dmr(const radio_view_t *v);
static void draw_chips_dmr(const radio_view_t *v);
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
	sb_str(&b, "PTT" GLYPH_ARROW " ");
	int x = gfx_text(f, 0, L->talk_y, buf, col.dim);
	sb_init(&b, buf, sizeof buf);
	color_t c = col.fg;
	if (v->ch.mode == RADIO_MODE_DMR) {
		sb_str(&b, "DMR not in this build");
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

#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR) {
		draw_chips_dmr(v);
		return;
	}
#endif
	sb_init(&b, buf, sizeof buf);
	bool tone_temp = (v->temp & (RADIO_TEMP_TX_TONE | RADIO_TEMP_RX_TONE)) != 0;
	if (tone_temp)
		x = gfx_text(f, x, y, GLYPH_DOT, col.warn);
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
	x = gfx_text(f, x, y, buf, tone_temp ? col.warn : col.fg) + f->w;
	bool bw_temp = (v->temp & RADIO_TEMP_BW) != 0;
	if (bw_temp)
		x = gfx_text(f, x, y, GLYPH_DOT, col.warn);
	x = gfx_text(f, x, y, v->ch.bw == RADIO_BW_25K ? "25k" : "12.5k", bw_temp ? col.warn : col.fg) + f->w;
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
	case TXDENY_MODE: sb_str(&b, "Mode not in this build"); break;
	case TXDENY_NO_CODEC: sb_str(&b, "No codec: voice off"); break;
	case TXDENY_NO_DMR: sb_str(&b, "DMR modem not running"); break;
	case TXDENY_BUSY: sb_str(&b, "Channel busy (polite)"); break;
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
	case TXDENY_NO_CODEC: return "Merge the codec (README)";
	case TXDENY_BUSY: return "Wait for the call to end";
	case TXDENY_NO_REPEATER: return "Check freq / CC / range";
	case TXDENY_REJECTED: return "Release PTT, try again";
	case TXDENY_HOTSPOT: return "OK leaves hotspot mode";
	case TXDENY_NO_ID: return "Set the radio ID in CPS";
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
static void draw_talk_dmr(const radio_view_t *v)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->medium;
	const radio_dmr_view_t *d = &v->dmr;
	char buf[48], fit[48];
	sbuf_t b;
	color_t c = col.fg;

	sb_init(&b, buf, sizeof buf);
	sb_str(&b, d->reply_active ? "Reply" GLYPH_ARROW " " : "PTT" GLYPH_ARROW " ");
	int x = gfx_text(f, 0, L->talk_y, buf, d->reply_active ? col.warn : col.dim);
	sb_init(&b, buf, sizeof buf);
	if (v->ch.rx_only) {
		sb_str(&b, "RX only");
		c = col.warn;
	} else if (!d->active) {
		sb_str(&b, "DMR modem off");
		c = col.warn;
	} else {
		sb_target(&b, d->target_id, d->target_private, d->target_name);
		if (d->reply_active) {
			sb_str(&b, " (");
			sb_u32(&b, d->reply_s, 1);
			sb_str(&b, "s)");
			c = col.warn;
		}
	}
	// Too wide for the medium font: the small one, baseline aligned, before shortening anything
	const font_t *tf = gfx_text_width(f, buf) <= gfx_width() - x ? f : L->small;
	gfx_text_fit(tf, buf, gfx_width() - x, fit, sizeof fit);
	gfx_text(tf, x, L->talk_y + f->ascent - tf->ascent, fit, c);
}

// Chips: timeslot, color code, repeater / direct, power, hear all
static void draw_chips_dmr(const radio_view_t *v)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	char buf[40];
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "TS");
	sb_u32(&b, v->dmr.ts ? v->dmr.ts : v->ch.timeslot, 1);
	sb_str(&b, " CC");
	sb_u32(&b, v->dmr.active ? v->dmr.cc : v->ch.color_code, 1);
	if (v->dmr.rmo)
		sb_str(&b, v->dmr.repeater_up ? " RPT" : " Rpt");
	int x = gfx_text(f, 0, L->chips_y, buf, col.fg) + f->w;
	bool pw_temp = v->power != channel_power(v);
	if (pw_temp)
		x = gfx_text(f, x, L->chips_y, GLYPH_DOT, col.warn);
	x = gfx_text(f, x, L->chips_y, power_name(v->power), pw_temp ? col.warn : col.fg) + f->w;
	if (v->dmr.hear_all && x + 3 * f->w <= gfx_width())
		gfx_text(f, x, L->chips_y, "All", col.warn);
}

// Activity on Home, DMR idle: the last call heard
static void draw_last_heard(const radio_view_t *v, const ui_state_t *s, int y)
{
	const font_t *sm = ui_layout.small;
	const radio_dmr_view_t *d = &v->dmr;
	char buf[48], fit[48];
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);
	if (d->last_ms != 0) {
		sb_caller(&b, &d->last);
		sb_str(&b, " " GLYPH_DOT " ");
		if (d->last.group) {
			sb_str(&b, "TG");
			sb_u32(&b, d->last.dst, 1);
		} else {
			sb_str(&b, d->last.to_us ? "to you" : "private");
		}
		sb_chr(&b, ' ');
		sb_age(&b, s->now_ms - d->last_ms);
	} else {
		sb_str(&b, "No calls heard yet");
	}
	gfx_text_fit(sm, buf, gfx_width(), fit, sizeof fit);
	gfx_text(sm, 0, y, fit, col.dim);
}

// One text line of the call screens, if it still fits above the hint row; returns the next y
static int call_line(const font_t *f, int y, const char *text, color_t c)
{
	char fit[48];
	if (gfx_text_width(f, text) > gfx_width())
		f = ui_layout.small;	// too wide: the small font before shortening
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
	if (v->state == RS_TX) {
		sb_target(&b, d->target_id, d->target_private, "");
	} else {
		sb_call_dst(&b, &d->call);
	}
	gfx_clear(col.bg);
	draw_strip(v, s, title);
	int y = L->strip_h + 1;

	if (v->state == RS_TX) {
		const char *word = "TALKING";
		switch (d->state) {
		case RDMR_TX_WAIT: word = d->rmo && !d->repeater_up ? "WAKING RPT" : "WAITING"; break;
		case RDMR_TX_ENDING: word = "ENDING"; break;
		default: break;
		}
		const font_t *big = gfx_text_width(L->large, word) <= w ? L->large : L->medium;
		gfx_text(big, 0, y, word, col.tx);
		y += big->h + 2;
		sb_init(&b, buf, sizeof buf);
		sb_target(&b, d->target_id, d->target_private, d->target_name);
		y = call_line(L->medium, y, buf, col.fg);
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
		y = call_line(L->small, y, buf, col.dim);
		sb_init(&b, buf, sizeof buf);
		sb_mmss(&b, v->tx_elapsed_ms / 1000u);
		if (v->tot_s) {
			sb_str(&b, "   TOT ");
			sb_mmss(&b, v->tot_s);
		}
		call_line(L->small, y, buf, col.fg);
		draw_hint(d->state == RDMR_TX_ENDING ? "Finishing the superframe" : "Release PTT to stop", false);
		return;
	}

	// Receiving: the caller large, then ID, alias, talkgroup, signal
	sb_init(&b, buf, sizeof buf);
	sb_caller(&b, &d->call);
	const font_t *big = gfx_text_width(L->large, buf) <= w ? L->large : L->medium;
	char fit[40];
	gfx_text_fit(big, buf, w, fit, sizeof fit);
	gfx_text(big, 0, y, fit, d->call.emergency ? col.warn : (d->state == RDMR_RX ? col.fg : col.dim));
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
	y = call_line(L->small, y, buf, d->call.emergency ? col.warn : col.dim);
	if (d->alias[0] != '\0') {
		sb_init(&b, buf, sizeof buf);
		sb_chr(&b, '"');
		sb_str(&b, d->alias);
		sb_chr(&b, '"');
		y = call_line(L->small, y, buf, col.fg);
	}
	sb_init(&b, buf, sizeof buf);
	if (d->state == RDMR_RX_OTHER)
		sb_str(&b, d->call.encrypted ? "Encrypted: not played" : "Other TG: not played");
	else if (!d->codec)
		sb_str(&b, "No codec: voice off");
	else
		sb_call_dst(&b, &d->call);
	y = call_line(L->small, y, buf, (d->state == RDMR_RX_OTHER || !d->codec) ? col.warn : col.fg);
	if (y + L->small->h <= L->hint_y) {
		int16_t dbm = d->rssi_dbm != 0 ? d->rssi_dbm : v->rssi_dbm;
		sb_init(&b, buf, sizeof buf);
		sb_i32(&b, dbm);
		sb_str(&b, "dBm");
		int tw = gfx_text_width(L->small, buf);
		gfx_text(L->small, w - tw, y, buf, col.fg);
		draw_meter(0, y + L->small->h / 4, w - tw - L->small->w, L->small->h / 2, dbm);
	}
	draw_hint(d->state == RDMR_RX_OTHER ? "Hold MON to hear all" : "PTT talks after the call", false);
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
#if CONFIG_DMR
		if (v->ch.mode == RADIO_MODE_DMR && v->dmr.active) {
			draw_last_heard(v, s, y);
			if (v->dmr.hear_all && L->activity_h >= 2 * sm->h)
				gfx_text(sm, 0, y + sm->h, "Hear all: every talkgroup", col.warn);
			break;
		}
#endif
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
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR) {
		if (v->dmr.reply_active)
			return "Back ends reply  * Quick";
		if (!v->dmr.codec)
			return "No codec: voice off";
		if (v->dmr.hear_all)
			return "Hear all while MON held";
	}
#endif
	if (v->temporary)
		return "Back undo edits  * Quick";
	if (v->vfo)
		return "0-9 freq  * Quick  Up/Dn";
	return "Up/Dn channel  * Quick";
}

void ui_draw_home(const radio_view_t *v, const ui_state_t *s)
{
#if CONFIG_DMR
	if (dmr_call_screen(v)) {
		draw_dmr_call(v, s);
		if (s->toast != NULL)
			draw_hint(s->toast, true);
		return;
	}
#endif
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

	gfx_clear(col.bg);
	draw_strip(&pill, s, "Hotspot");
	int y = L->strip_h + 1;
	const char *word = !h->running ? "STARTING" : h->transmitting ? "TX NET" : h->receiving ? "RX RF" : "HOTSPOT";
	const font_t *big = gfx_text_width(L->large, word) <= w ? L->large : L->medium;
	gfx_text(big, 0, y, word, h->transmitting ? col.tx : h->receiving ? col.rx : col.fg);
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
	y = call_line(L->medium, y, buf, col.fg);
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "CC");
	sb_u32(&b, h->cc, 1);
	sb_str(&b, " ");
	sb_str(&b, power_name(v->power));
	y = call_line(L->small, y, buf, col.dim);
	sb_init(&b, buf, sizeof buf);
	if (h->src != 0) {
		sb_str(&b, h->from_net ? "Net " : "RF ");
		sb_u32(&b, h->src, 1);
		sb_str(&b, GLYPH_ARROW);
		if (h->group)
			sb_str(&b, "TG");
		sb_u32(&b, h->dst, 1);
	} else {
		sb_str(&b, "No calls yet");
	}
	call_line(L->small, y, buf, col.fg);
	draw_hint(s->toast != NULL ? s->toast : "OK or hold Back: exit", s->toast != NULL);
}
#endif

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
	case QT_TONE: {
		if (!CONFIG_SUBTONE)
			return false;
		bool editing = s->quick_edit && s->quick_focus == QT_TONE;
		if (editing && s->tone_target != RADIO_TONE_BOTH) {
			sbuf_t lb;
			sb_init(&lb, label, label_len);
			sb_chr(&lb, '3');
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
		sb_str(&b, v->ch.mode == RADIO_MODE_DMR ? "DMR" : "FM");
		return false;
	case QT_MONITOR:
		sb_str(&b, v->monitor ? "On" : "Off");
		return false;
	case QT_VOX:
		sb_str(&b, "Off");
		return false;
	case QT_VFO:
		if (!CONFIG_VFO_FM)
			return false;	// built without a VFO
		sb_str(&b, v->vfo ? "On" : "Off");
		return true;
	case QT_BANDWIDTH:
		if (v->temp & RADIO_TEMP_BW)
			sb_str(&b, GLYPH_DOT);
		sb_str(&b, v->ch.bw == RADIO_BW_25K ? "25k" : "12.5k");
		return v->ch.mode == RADIO_MODE_FM;
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
		const font_t *tf = vf;	// a value too wide for the tile ("<D023N>") drops to the label font
		if (gfx_text_width(tf, value) > x1 - x0 - 3)
			tf = lf;
		gfx_text_fit(tf, value, x1 - x0 - 3, fit, sizeof fit);
		int vx = x0 + (x1 - x0 + 1 - gfx_text_width(tf, fit)) / 2;
		int vy = y0 + 1 + lf->h + (y1 - y0 - 1 - lf->h - tf->h) / 2;
		gfx_text(tf, vx, vy, fit, fg);
		gfx_clip_off();
	}
	if (s->toast != NULL)
		draw_hint(s->toast, true);
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

	gfx_clear(col.bg);
	gfx_text(f, 0, y, "Diagnostics", col.warn);
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
	gfx_text(f, 0, y, buf, col.fg);
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
	gfx_text(f, 0, y, buf, col.fg);
	y += f->h;

	diag_lines_per_page = 0;
	for (uint8_t i = first_line; board.diag_line != NULL && y + f->h <= gfx_height() - f->h; i++) {
		diag_lines_per_page++;
		if (!board.diag_line(i, buf, sizeof buf))
			break;
		gfx_text(f, 0, y, buf, col.dim);
		y += f->h;
	}
	gfx_text(f, 0, gfx_height() - f->h, "* more  BACK Home", col.idle);
}
#endif
