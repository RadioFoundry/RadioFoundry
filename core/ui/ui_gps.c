// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tools > Position with CONFIG_GPS_SKY (docs/spec/ui.md 2.6): the board GPS on four pages, Up / Down (the knob, the
// digits 1-4) to turn them. Status: the fix, satellites, position, altitude, speed, dilution, time. Sky: where each
// satellite is (a polar plot, north up), filled when the fix uses it. Signal: each satellite's signal as a bar. Grid:
// the Maidenhead locator large, the position in degrees, minutes and seconds. Nothing is kept: every draw reads the
// service again, the satellites one at a time.
#include "config.h"
#if CONFIG_GPS_SKY
#include <string.h>
#include "ui_nav.h"
#include "fmt.h"

enum { PG_STATUS, PG_SKY, PG_SIGNAL, PG_GRID, PG_COUNT };
static const char *const page_names[PG_COUNT] = { "Status", "Sky", "Signal", "Grid" };
static const char sys_letter[UI_GNSS_COUNT] = { 'G', 'R', 'E', 'C', 'J' };
static const char *const sys_names[UI_GNSS_COUNT] = { "GPS", "GLONASS", "Galileo", "BeiDou", "QZSS" };
static const char *const sys_short[UI_GNSS_COUNT] = { "GPS", "GLO", "GAL", "BDS", "QZS" };
#define SNR_FULL	50	// dB-Hz for a full bar: a strong satellite under open sky

static uint8_t page;

// A system's color; every one is also named in the legend (P5). Ink on a monochrome panel
static color_t sys_color(uint8_t sys)
{
	static const color_t dark[UI_GNSS_COUNT] = {
		RGB565(0x40, 0xA0, 0xFF), RGB565(0xFF, 0x60, 0x60), RGB565(0xF0, 0xC8, 0x30), RGB565(0x40, 0xD8, 0x80),
		RGB565(0xC8, 0x88, 0xFF),
	};
	static const color_t light[UI_GNSS_COUNT] = {
		RGB565(0x00, 0x58, 0xC8), RGB565(0xC0, 0x18, 0x18), RGB565(0x98, 0x70, 0x00), RGB565(0x00, 0x80, 0x38),
		RGB565(0x78, 0x30, 0xC0),
	};
	if (!CONFIG_UI_COLOR || gfx_bpp() == 1 || sys >= UI_GNSS_COUNT)
		return ui_col.fg;
	bool light_bg = ((ui_col.bg >> 5) & 0x3Fu) > 0x20u;
	return light_bg ? light[sys] : dark[sys];
}

// sin x 256 of a whole degree, from a 6 degree table
static int isin(int deg)
{
	static const uint16_t q[16] = { 0, 27, 53, 79, 104, 128, 150, 171, 190, 207, 222, 234, 243, 250, 255, 256 };
	deg %= 360;
	if (deg < 0)
		deg += 360;
	int sign = deg >= 180 ? -1 : 1;
	deg %= 180;
	if (deg > 90)
		deg = 180 - deg;
	int i = deg / 6, r = deg % 6;
	int v = q[i] + (i < 15 ? (q[i + 1] - q[i]) * r / 6 : 0);
	return sign * v;
}

static void circle(int cx, int cy, int r, color_t c)
{
	int x = r, y = 0, e = 1 - r;
	while (x >= y) {
		gfx_pixel(cx + x, cy + y, c); gfx_pixel(cx - x, cy + y, c);
		gfx_pixel(cx + x, cy - y, c); gfx_pixel(cx - x, cy - y, c);
		gfx_pixel(cx + y, cy + x, c); gfx_pixel(cx - y, cy + x, c);
		gfx_pixel(cx + y, cy - x, c); gfx_pixel(cx - y, cy - x, c);
		y++;
		if (e < 0) {
			e += 2 * y + 1;
		} else {
			x--;
			e += 2 * (y - x) + 1;
		}
	}
}

static void disc(int cx, int cy, int r, color_t c)
{
	int dx = r;
	for (int dy = 0; dy <= r; dy++) {
		while (dx * dx + dy * dy > r * r + r)
			dx--;
		gfx_hline(cx - dx, cy + dy, 2 * dx + 1, c);
		gfx_hline(cx - dx, cy - dy, 2 * dx + 1, c);
	}
}

static const char *compass(uint16_t deg)
{
	static const char *const pts[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	return pts[((deg + 22u) / 45u) % 8u];
}

// The Maidenhead locator to 6 characters ("FN42li")
static void locator(char out[7], int32_t lat_e7, int32_t lon_e7)
{
	uint32_t lo = (uint32_t)(lon_e7 + 1800000000), la = (uint32_t)(lat_e7 + 900000000);
	if (lo >= 3600000000u)
		lo = 3599999999u;
	if (la >= 1800000000u)
		la = 1799999999u;
	out[0] = (char)('A' + lo / 200000000u);
	out[1] = (char)('A' + la / 100000000u);
	lo %= 200000000u;
	la %= 100000000u;
	out[2] = (char)('0' + lo / 20000000u);
	out[3] = (char)('0' + la / 10000000u);
	out[4] = (char)('a' + (lo % 20000000u) * 24u / 20000000u);
	out[5] = (char)('a' + (la % 10000000u) * 24u / 10000000u);
	out[6] = 0;
}

// 42 21'36.4"N
static void sb_dms(sbuf_t *b, int32_t e7, char pos, char neg)
{
	uint32_t a = (uint32_t)(e7 < 0 ? -e7 : e7);
	uint64_t m = (uint64_t)(a % 10000000u) * 60u;	// minutes x 1e7
	uint32_t s10 = (uint32_t)((m % 10000000u) * 600u / 10000000u);
	sb_u32(b, a / 10000000u, 1);
	sb_chr(b, ' ');
	sb_u32(b, (uint32_t)(m / 10000000u), 2);
	sb_chr(b, '\'');
	sb_u32(b, s10 / 10u, 2);
	sb_chr(b, '.');
	sb_u32(b, s10 % 10u, 1);
	sb_str(b, "\"");
	sb_chr(b, e7 < 0 ? neg : pos);
}

static void sb_deg(sbuf_t *b, int32_t e7, char pos, char neg)
{
	sb_degrees(b, e7 < 0 ? -e7 : e7);
	sb_chr(b, ' ');
	sb_chr(b, e7 < 0 ? neg : pos);
}

static void sb_dop(sbuf_t *b, uint16_t d10)
{
	sb_u32(b, d10 / 10u, 1);
	sb_chr(b, '.');
	sb_u32(b, d10 % 10u, 1);
	sb_str(b, d10 <= 20u ? " good" : d10 <= 50u ? " fair" : " poor");
}

static const char *fix_word(const ui_position_t *p)
{
	if (p->fix)
		return p->mode == 2 ? "2D fix" : "3D fix";
	return p->in_view > 0 ? "Searching" : "No signal";
}

// Satellites the fix uses and those tracked (a signal), per system: the legend's numbers
static void count_sats(uint8_t used[UI_GNSS_COUNT], uint8_t seen[UI_GNSS_COUNT])
{
	ui_gps_sat_t s;
	memset(used, 0, UI_GNSS_COUNT);
	memset(seen, 0, UI_GNSS_COUNT);
	for (uint8_t i = 0; ui_svc_gps_sat(i, &s); i++) {
		if (s.sys >= UI_GNSS_COUNT)
			continue;
		seen[s.sys]++;
		used[s.sys] += s.used;
	}
}

// "label  value" rows from y; false once the next would run into the hint row
static bool row(int *y, const char *label, const char *value, color_t vc)
{
	const font_t *f = ui_layout.small;
	if (*y + f->h > ui_layout.hint_y)
		return false;
	int x = gfx_text(f, 0, *y, label, ui_col.dim);
	gfx_text(f, x + f->w, *y, value, vc);
	*y += f->h + (ui_layout.row_h > f->h ? 1 : 0);
	return true;
}

static void draw_status(const ui_position_t *p, int y)
{
	char buf[32];
	sbuf_t b;
	const font_t *big = gfx_width() >= 200 ? ui_layout.large : ui_layout.medium;
	gfx_text(big, 0, y, fix_word(p), p->fix ? ui_col.rx : ui_col.warn);
	y += big->h + 1;
	sb_init(&b, buf, sizeof buf);
	sb_u32(&b, p->sats, 1);
	sb_str(&b, " used, ");
	sb_u32(&b, p->in_view, 1);
	sb_str(&b, " seen");
	if (!row(&y, "Sats", buf, ui_col.fg))
		return;
	if (p->fix) {
		sb_init(&b, buf, sizeof buf);
		sb_deg(&b, p->lat_e7, 'N', 'S');
		if (!row(&y, "Lat", buf, ui_col.fg))
			return;
		sb_init(&b, buf, sizeof buf);
		sb_deg(&b, p->lon_e7, 'E', 'W');
		if (!row(&y, "Lon", buf, ui_col.fg))
			return;
		if (p->alt_ok) {
			sb_init(&b, buf, sizeof buf);
			sb_i32(&b, p->alt_m);
			sb_str(&b, " m");
			if (!row(&y, "Alt", buf, ui_col.fg))
				return;
		}
		if (p->speed_kmh != 0xFFFFu) {
			sb_init(&b, buf, sizeof buf);
			sb_u32(&b, p->speed_kmh, 1);
			sb_str(&b, " km/h");
			if (p->course != 0xFFFFu) {
				sb_chr(&b, ' ');
				sb_str(&b, compass(p->course));
			}
			if (!row(&y, "Speed", buf, ui_col.fg))
				return;
		}
	}
	if (p->hdop10 != 0xFFFFu && p->fix) {
		sb_init(&b, buf, sizeof buf);
		sb_dop(&b, p->hdop10);
		if (!row(&y, "HDOP", buf, ui_col.fg))
			return;
	}
	if (p->utc_hms != 0xFFFFFFFFu) {
		sb_init(&b, buf, sizeof buf);
		sb_u32(&b, p->utc_hms / 10000u, 2);
		sb_chr(&b, ':');
		sb_u32(&b, p->utc_hms / 100u % 100u, 2);
		sb_chr(&b, ':');
		sb_u32(&b, p->utc_hms % 100u, 2);
		row(&y, "UTC", buf, ui_col.fg);
	}
}

// The systems heard: "GPS 6/9" (used / seen) in their colors, one a line in cols columns of col_w from x, y
static void legend(int x, int y, int cols, int col_w, const uint8_t used[], const uint8_t seen[])
{
	const font_t *f = ui_layout.small;
	bool full = gfx_text_width(f, "GLONASS 10/10") <= col_w;
	char buf[16];
	sbuf_t b;
	int k = 0;
	for (uint8_t s = 0; s < UI_GNSS_COUNT; s++) {
		if (seen[s] == 0)
			continue;
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, full ? sys_names[s] : sys_short[s]);
		sb_chr(&b, ' ');
		sb_u32(&b, used[s], 1);
		sb_chr(&b, '/');
		sb_u32(&b, seen[s], 1);
		int ly = y + (k / cols) * (f->h + 1);
		if (ly + f->h > ui_layout.hint_y)
			return;
		gfx_text(f, x + (k % cols) * col_w, ly, buf, sys_color(s));
		k++;
	}
}

static void draw_sky(const ui_position_t *p, int y)
{
	int w = gfx_width(), h = ui_layout.hint_y - y - 1;
	uint8_t used[UI_GNSS_COUNT], seen[UI_GNSS_COUNT];
	count_sats(used, seen);
	int systems = 0;
	for (uint8_t i = 0; i < UI_GNSS_COUNT; i++)
		systems += seen[i] > 0;
	bool below = gfx_height() > w;		// a tall panel: the legend under the plot, two columns
	int side = below ? w : (h < w ? h : w);
	int legend_h = (systems + 1) / 2 * (ui_layout.small->h + 1) + 3;
	if (below && side > h - legend_h)
		side = h - legend_h;
	int r = side / 2 - 1;
	int cx = below ? w / 2 : r + 1, cy = y + r + 1;
	const font_t *t = ui_layout.tiny;
	circle(cx, cy, r, ui_col.line);
	circle(cx, cy, r / 2, ui_col.line);	// 45 degrees
	gfx_hline(cx - r, cy, 2 * r + 1, ui_col.line);
	gfx_vline(cx, cy - r, 2 * r + 1, ui_col.line);
	gfx_text(t, cx + 2, cy - r, "N", ui_col.dim);
	int dot = r >= 40 ? 3 : 2;
	bool labels = r >= 40;
	ui_gps_sat_t s;
	uint8_t drawn = 0;
	for (uint8_t i = 0; ui_svc_gps_sat(i, &s); i++) {
		if (s.elev == 0xFFu || s.az == 0xFFFFu)
			continue;
		int d = r * (90 - s.elev) / 90;
		int sx = cx + d * isin(s.az) / 256, sy = cy - d * isin(s.az + 90) / 256;
		color_t c = s.snr == 0 ? ui_col.dim : sys_color(s.sys);
		if (s.used)
			disc(sx, sy, dot, c);
		else
			circle(sx, sy, dot, c);
		if (labels) {
			char lb[4] = { sys_letter[s.sys < UI_GNSS_COUNT ? s.sys : 0], 0, 0, 0 };
			sbuf_t b;
			sb_init(&b, lb + 1, sizeof lb - 1);
			sb_u32(&b, s.num, 1);
			gfx_text(t, sx + dot + 1, sy - t->h / 2, lb, c);
		}
		drawn++;
	}
	if (drawn == 0)
		gfx_text(ui_layout.small, 0, y, p->in_view ? "No positions yet" : "No satellites yet", ui_col.dim);
	if (below)
		legend(0, cy + r + 3, 2, w / 2, used, seen);
	else
		legend(cx + r + 4, y, 1, w - (cx + r + 4), used, seen);
}

static void draw_signal(const ui_position_t *p, int y)
{
	int w = gfx_width();
	const font_t *t = ui_layout.tiny;
	char buf[24];
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);
	sb_u32(&b, p->sats, 1);
	sb_str(&b, " of ");
	sb_u32(&b, p->in_view, 1);
	sb_str(&b, " in the fix");
	gfx_text(ui_layout.small, 0, y, buf, ui_col.fg);
	y += ui_layout.small->h + 2;
	uint8_t n = p->in_view;
	if (n == 0)
		return;
	int pitch = w / n;
	if (pitch > 16)
		pitch = 16;
	if (pitch < 3)
		pitch = 3;
	bool labels = pitch >= t->w * 2 + 3;	// two digits and a gap
	int base = ui_layout.hint_y - 2 - (labels ? t->h : 0);
	int hmax = base - y;
	if (hmax < 4)
		return;
	ui_gps_sat_t s;
	for (uint8_t i = 0; i < n && (i + 1) * pitch <= w && ui_svc_gps_sat(i, &s); i++) {
		int x = i * pitch, bw = pitch - 1;
		int bh = s.snr * hmax / SNR_FULL;
		bh = bh > hmax ? hmax : bh < 1 ? 1 : bh;
		color_t c = sys_color(s.sys);
		if (s.used)
			gfx_fill(x, base - bh, bw, bh, c);
		else
			gfx_rect(x, base - bh, bw, bh, s.snr ? c : ui_col.dim);
		if (labels) {
			char lb[4];
			sbuf_t lbb;
			sb_init(&lbb, lb, sizeof lb);
			sb_u32(&lbb, s.num, 1);
			gfx_text(t, x + (bw - gfx_text_width(t, lb)) / 2, base + 1, lb, c);
		}
	}
	gfx_hline(0, base, w, ui_col.line);
}

static void draw_grid(const ui_position_t *p, int y)
{
	int w = gfx_width();
	char buf[24];
	sbuf_t b;
	if (!p->fix) {
		gfx_text(ui_layout.small, 0, y, "No fix yet", ui_col.warn);
		return;
	}
	char loc[7];
	locator(loc, p->lat_e7, p->lon_e7);
	const font_t *big = gfx_text_width(ui_layout.large, loc) <= w ? ui_layout.large : ui_layout.medium;
	gfx_text(big, (w - gfx_text_width(big, loc)) / 2, y, loc, ui_col.fg);
	y += big->h + 2;
	sb_init(&b, buf, sizeof buf);
	sb_dms(&b, p->lat_e7, 'N', 'S');
	if (!row(&y, "", buf, ui_col.fg))
		return;
	sb_init(&b, buf, sizeof buf);
	sb_dms(&b, p->lon_e7, 'E', 'W');
	if (!row(&y, "", buf, ui_col.fg))
		return;
	sb_init(&b, buf, sizeof buf);
	sb_deg(&b, p->lat_e7, 'N', 'S');
	if (!row(&y, "", buf, ui_col.dim))
		return;
	sb_init(&b, buf, sizeof buf);
	sb_deg(&b, p->lon_e7, 'E', 'W');
	row(&y, "", buf, ui_col.dim);
}

static void gps_draw(void)
{
	ui_position_t p;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, "GPS", page_names[page]) + 2;
	if (!ui_svc_position(&p)) {
		gfx_text(ui_layout.small, 0, y, "No GPS, or it is off", ui_col.warn);
		gfx_text(ui_layout.small, 0, y + ui_layout.small->h + 2, "Settings > Location", ui_col.dim);
	} else {
		switch (page) {
		case PG_SKY: draw_sky(&p, y); break;
		case PG_SIGNAL: draw_signal(&p, y); break;
		case PG_GRID: draw_grid(&p, y); break;
		default: draw_status(&p, y); break;
		}
	}
	ui_draw_bottom(&ui_st, "Up Down page  Back");
	ui_draw_banner(&ui_st);
}

static bool gps_key(const input_event_t *ev)
{
	int dir = 0;
	if (ev->action == ACT_ENC_STEP) {
		dir = ev->value > 0 ? 1 : ev->value < 0 ? -1 : 0;
	} else if (ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT) {
		if (ev->action == ACT_NAV_DOWN || ev->action == ACT_NAV_RIGHT)
			dir = 1;
		else if (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_LEFT)
			dir = -1;
		else if (ev->gesture == GESTURE_PRESS && ev->action >= ACT_DIGIT_1 && ev->action < ACT_DIGIT_1 + PG_COUNT) {
			page = (uint8_t)(ev->action - ACT_DIGIT_1);
			ui_dirty();
			return true;
		}
	}
	if (dir == 0)
		return false;
	page = (uint8_t)((page + PG_COUNT + dir) % PG_COUNT);
	ui_dirty();
	return true;
}

static void gps_enter(void)
{
	page = PG_STATUS;
}

const ui_scr_def_t ui_gps_screen = {
	.name = "gps", .flags = SF_FIELD, .key = gps_key, .draw = gps_draw, .enter = gps_enter,
};
#endif
