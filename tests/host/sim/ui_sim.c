// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI host simulator (ui_sim.h). Everything core/ui needs from the firmware is simulated here: the board
// descriptor, hal_millis, the radio core (radio_get_view / radio_req_*), the codeplug, the host link and the
// services other packages own (ui_svc.h; the settings store is the weak one from ui_svc_stub.c).
//
// Script tokens (separated by spaces):
//   up down left right ok back quick ctx 0..9 tt mon user1..user4 emerg enc+ enc-   a short press (counted);
//   powerkey zone+ zone- scankey lockkey cbkey homekey dualkey abkey: what a reassigned side key sends
//   long:<key>          a long press (counted)
//   ptt / ptt:<ms>      PTT pressed for 1 s / ms (counted); pttdown ... pttup holds it
//   mondown ... monup   the monitor key held (FM: the squelch held open)
//   wait:<ms>           time passes
//   reboot              power off and on: the screens start over, the settings stay
//   receivers:2         a receiver per dual watch channel; rxa:/rxb:<src>,<dst>,g|p a call on A's / B's own,
//                       rxaend / rxbend its end
//   tune:a|b            dual watch: the receiver moves to channel A or B (a call there holds it)
//   busy:a|b|none       dual watch: the radio's mark for a signal found on A / B in the wait after a call
//   dlpos:<n>           another task (the debug link) asks the radio for position n of the zone
//   dldual:<n>          the debug link's scan dual: the radio watches the channel it is on (A) and position n
//   dlstop              the debug link's scan stop (dual watch: ends on the selected channel, view.dual_stops)
//   dlsel:a|b           the debug link selects A or B of the dual watch (view.dual_sel); no UI tick after it
//   cpwrite, cpdel:<n>, zonesize:<z>,<n>   the codeplug is written: channel 40 first in zone 0, channel n deleted,
//                       zone z cut to n channels
//   fmrx / fmrxend      an FM signal opens / closes the squelch
//   rx:<src>,<dst>,g|p[,name]   a DMR call starts (g = group, p = private to us); rxend ends it
//   call:<src>,<dst>,g|p        a 2 s call, then 1 s of quiet
//   msg:<src>,<text>    a text message arrives (_ = space); alert:<src>; emergency:<src>; privcall:<src>
//   hotspot:on|off      the host link enters / leaves hotspot mode
//   usbnet:off|on|up    USB a serial port, a network adapter, or a network adapter the computer has brought up
//   battery:<pct>       the battery reads pct; offfailed: auto power off couldn't switch off
//   nobattery           the board has no battery (CAP_NO_BATTERY), until the next reset
//   supply:<mv>         no battery, and the board measures its DC input, which reads mv (hal_power_t.supply_mv)
//   color:<name>        the highlight color (sim_color_find); strip:on|off  the filled status bar (Settings > Display)
//   theme:<name>        the color theme (sim_theme_find: dark, light, high_contrast)
//   splash              draw the boot screen (put it last: nothing redraws over it)
//   shot:<name>         save the screen (sim_set_shot_dir)
//   expect:<key>=<value>   check (see check()); value "*" = anything; shows=<text> / hides=<text>: a drawn
//                          text contains it / none does; markafter=<text> / nomarkafter=<text>: a mark is drawn just right of it / none is
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_sim.h"
// The board is changed at run time here (display, key map): this file defines it writable, the UI sees it const
#define board board_as_declared
#include "board.h"
#undef board
#include "hal_time.h"
#include "ui/gfx.h"
#include "ui/ui_screens.h"
#include "ui/ui_app.h"
#include "ui/ui_svc.h"
#include "ui/ui_nav.h"
#include "input/input.h"
#include "hostlink/hostlink.h"
#include "hotspot/hotspot.h"
#include "ui/ui_svc_aprs.h"

#define TICK_MS		50
#define PRESS_MS	150
#define LONG_MS		700
#define REPLY_S		10

sim_record_t sim_rec;

static void (*fit_hook)(const char *src, const char *out);

void sim_set_fit_hook(void (*hook)(const char *src, const char *out))
{
	fit_hook = hook;
}

void gfx_fit_trace(const char *src, const char *out)
{
	sim_rec.shortened++;
	if (fit_hook != NULL)
		fit_hook(src, out);
}

// ---------------- the layout check (gfx_draw_trace) ----------------
// Each frame starts with a clear. Texts drawn since then are "live" until a box (a fill at least 2 px each way) is
// drawn over them: a box as wide as the screen is an overlay (banner, toast, hint row) and may hide whole texts, but
// not half a line; a narrower box over part of a text is a problem too. Two texts side by side with no space between
// them read as one word. Marks of 6 px or less (scroll marks, ellipsis dots) and lines (one pixel thick) are left out.

static bool worst;	// sim_worst: worst-case codeplug content

#define LIVE_MAX	160
static struct { int x0, y0, x1, y1; char s[40]; } live[LIVE_MAX];
static int live_n;
static char issues[SIM_LAYOUT_MAX][160];
static int issue_n;

static void layout_reset(void)
{
	live_n = 0;
	issue_n = 0;
}

static void issue(const char *what, const char *s, int x, int y, int w, int h, const char *other)
{
	if (issue_n >= SIM_LAYOUT_MAX)
		return;
	char a[40], b[40];
	snprintf(a, sizeof a, "%s", s);
	snprintf(b, sizeof b, "%s", other != NULL ? other : "");
	for (char *p = a; *p; p++)
		if ((unsigned char)*p >= 0x80 || (unsigned char)*p < 0x20)
			*p = '~';
	for (char *p = b; *p; p++)
		if ((unsigned char)*p >= 0x80 || (unsigned char)*p < 0x20)
			*p = '~';
	for (int i = 0; i < issue_n; i++)
		if (strstr(issues[i], a) != NULL && strncmp(issues[i], what, strlen(what)) == 0)
			return;		// the same text, the same problem: once
	if (other != NULL)
		snprintf(issues[issue_n++], sizeof issues[0], "%s: \"%s\" at %d,%d %dx%d and \"%s\"", what, a, x, y, w, h, b);
	else
		snprintf(issues[issue_n++], sizeof issues[0], "%s: \"%s\" at %d,%d %dx%d", what, a, x, y, w, h);
}

int sim_layout_issues(char out[][160], int max)
{
	int n = issue_n < max ? issue_n : max;
	for (int i = 0; i < n; i++)
		memcpy(out[i], issues[i], sizeof issues[0]);
	return n;
}

void gfx_draw_trace(gfx_trace_kind_t kind, int x, int y, int w, int h, const int clip[4], const char *s)
{
	int vw = gfx_width(), vh = gfx_height();
	if (kind == GFX_TRACE_CLEAR) {
		layout_reset();
		return;
	}
	// The part drawn: inside the clip rectangle
	int x0 = x > clip[0] ? x : clip[0], y0 = y > clip[1] ? y : clip[1];
	int x1 = x + w < clip[2] ? x + w : clip[2], y1 = y + h < clip[3] ? y + h : clip[3];
	if (kind == GFX_TRACE_FILL) {
		if (w < 2 || h < 2 || w * h <= 6 || x0 >= x1 || y0 >= y1)
			return;
		bool overlay = x0 <= 0 && x1 >= vw;
		for (int i = 0; i < live_n; i++) {
			if (live[i].x0 >= x1 || live[i].x1 <= x0 || live[i].y0 >= y1 || live[i].y1 <= y0)
				continue;
			// Covered: no ink of the text (a pixel unlike the cell's background, its top-left corner) is left
			// outside the box, or none is under it (only the cell's blank margin is)
			bool covers = true, touches = false;
			color_t paper = gfx_get_pixel(live[i].x0, live[i].y0);
			for (int py = live[i].y0; py < live[i].y1; py++) {
				for (int px = live[i].x0; px < live[i].x1; px++) {
					if (gfx_get_pixel(px, py) == paper)
						continue;
					bool in = px >= x0 && px < x1 && py >= y0 && py < y1;
					touches |= in;
					covers &= in;
				}
			}
			if (!touches) {
				// The box only meets the blank margin of the cell: the text stays whole, its cell ends at the box
				if (x0 <= live[i].x0 && x1 >= live[i].x1) {
					if (y0 > live[i].y0)
						live[i].y1 = y0;
					else
						live[i].y0 = y1;
				} else if (x0 > live[i].x0) {
					live[i].x1 = x0;
				} else {
					live[i].x0 = x1;
				}
				continue;
			}

			if (!covers)

				issue(overlay ? "partly under an overlay" : "box over text", live[i].s, live[i].x0, live[i].y0,
				      live[i].x1 - live[i].x0, live[i].y1 - live[i].y0, NULL);
			live[i--] = live[--live_n];	// hidden now
		}
		return;
	}
	if (w <= 0 || s == NULL || s[0] == 0)
		return;
	// Spaces at either end draw nothing: measure the ink's cells only
	size_t n = strlen(s), a = 0, e = n;
	int cw = w / (int)n;
	while (a < n && s[a] == ' ')
		a++;
	while (e > a && s[e - 1] == ' ')
		e--;
	if (a == e)
		return;
	x += (int)a * cw;
	w = (int)(e - a) * cw;
	x0 = x > clip[0] ? x : clip[0];
	x1 = x + w < clip[2] ? x + w : clip[2];
	if (x < 0 || y < 0 || x + w > vw || y + h > vh)
		issue("outside the screen", s, x, y, w, h, NULL);
	else if (x0 > x || y0 > y || x1 < x + w || y1 < y + h)
		issue("cut by its box", s, x, y, w, h, NULL);
	if (x0 >= x1 || y0 >= y1)
		return;
	// Invisible: a whole text (not one its box cuts) whose cells are one color after it is drawn, its ink the
	// color of what is under it (the high-contrast theme's white idle pill had white text)
	if (x0 == x && y0 == y && x1 == x + w && y1 == y + h) {
		color_t c0 = gfx_get_pixel(x0, y0);
		bool one = true;
		for (int py = y0; py < y1 && one; py++)
			for (int px = x0; px < x1 && one; px++)
				one = gfx_get_pixel(px, py) == c0;
		if (one)
			issue("invisible", s, x, y, w, h, NULL);
	}
	for (int i = 0; i < live_n; i++) {
		if (live[i].y0 >= y1 || live[i].y1 <= y0)
			continue;
		if (live[i].x1 == x0 || live[i].x0 == x1) {
			// Two words run together. A mark of one glyph (the temporary dot) sits next to its value on purpose,
			// and the wordmark is two colors of one word
			bool mark = e - a == 1 || live[i].s[1] == 0;
			bool wordmark = strcmp(live[i].s, "Open") == 0 && strcmp(s + a, "DigitalRadio") == 0;
			if (!mark && !wordmark)
				issue("no space between", s, x, y, w, h, live[i].s);
		} else if (live[i].x0 < x1 && live[i].x1 > x0) {
			issue("overlap", s, x, y, w, h, live[i].s);
		}
	}
	if (live_n < LIVE_MAX) {
		live[live_n].x0 = x0;
		live[live_n].y0 = y0;
		live[live_n].x1 = x1;
		live[live_n].y1 = y1;
		snprintf(live[live_n].s, sizeof live[0].s, "%.*s", (int)(e - a), s + a);
		live_n++;
	}
}

const sim_geom_t sim_geoms[] = {
	{ "160x120i5", 160, 120, 4, 5, 0, false, false },	// DM-1701: the lens hides 5 px left and right (boards/dm1701/config.mk)
	{ "160x128", 160, 128, 4, 0, 0, false, false },	// MD-380 / MD-UV380 / MD-9600 class
	{ "128x64", 128, 64, 1, 0, 0, false, false },	// GD-77 class, mono
	// DM-32UV: 16 colors, the bezel hides 10 px left and right, 5 top and bottom; FONT_BIG
	{ "240x320i10x5", 240, 320, 4, 10, 5, true, true },
	{ "240x320", 240, 320, 16, 0, 0, true, false },	// 240x320 class at full resolution, standard text size
};
const int sim_geom_count = sizeof sim_geoms / sizeof sim_geoms[0];

const sim_geom_t *sim_geom_find(const char *name)
{
	static sim_geom_t custom;
	for (int i = 0; i < sim_geom_count; i++)
		if (strcmp(name, sim_geoms[i].name) == 0)
			return &sim_geoms[i];
	unsigned w, h, inset = 0, inset_tb = 0;
	char tail = 0;
	int n = sscanf(name, "%ux%u%c%ux%u", &w, &h, &tail, &inset, &inset_tb);
	if (n < 2 || w < 64 || h < 48 || w > 320 || h > 320)
		return NULL;
	custom.name = name;
	custom.w = (uint16_t)w;
	custom.h = (uint16_t)h;
	custom.bpp = tail == 'm' ? 1 : 16;
	custom.inset = tail == 'i' ? (uint8_t)inset : 0;
	custom.inset_tb = tail == 'i' ? (uint8_t)(n == 5 ? inset_tb : inset) : 0;
	custom.aa = custom.bpp != 1 && w >= 220;	// as the gallery's classes
	return &custom;
}

// ---------------- the board ----------------

enum { K_GREEN, K_RED, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_STAR, K_HASH, K_0, K_9 = K_0 + 9, K_SK1, K_SK2, K_TOP,
       K_FN1, K_FN2, K_PTT, K_P1, K_P2 };

#if CONFIG_DUAL_WATCH
#define SIM_SK2_SHORT	ACT_AB		// the DM-1701's SK2: A/B on a press, Dual watch held (boards/dm1701/board.c)
#else
#define SIM_SK2_SHORT	ACT_TALK_TARGET
#endif
static const keymap_entry_t map_k[] = {	// DM-1701 (7.1)
	{ K_GREEN, ACT_SELECT, ACT_SELECT, false, false }, { K_RED, ACT_BACK, ACT_BACK, false, false },
	{ K_UP, ACT_NAV_UP, ACT_NAV_UP, false, true }, { K_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ K_STAR, ACT_QUICK, ACT_QUICK, false, false }, { K_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ K_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false }, { K_0 + 1, ACT_DIGIT_1, ACT_DIGIT_1, false, false },
	{ K_0 + 2, ACT_DIGIT_2, ACT_DIGIT_2, false, false }, { K_0 + 3, ACT_DIGIT_3, ACT_DIGIT_3, false, false },
	{ K_0 + 4, ACT_DIGIT_4, ACT_DIGIT_4, false, false }, { K_0 + 5, ACT_DIGIT_5, ACT_DIGIT_5, false, false },
	{ K_0 + 6, ACT_DIGIT_6, ACT_DIGIT_6, false, false }, { K_0 + 7, ACT_DIGIT_7, ACT_DIGIT_7, false, false },
	{ K_0 + 8, ACT_DIGIT_8, ACT_DIGIT_8, false, false }, { K_0 + 9, ACT_DIGIT_9, ACT_DIGIT_9, false, false },
	{ K_SK1, ACT_MONITOR, ACT_MONITOR, true, false }, { K_SK2, SIM_SK2_SHORT, ACT_USER_2, false, false },
	{ K_TOP, ACT_USER_3, ACT_EMERGENCY, false, false }, { K_FN1, ACT_USER_1, ACT_USER_1, false, false },
	{ K_FN2, ACT_USER_4, ACT_USER_4, false, false }, { K_PTT, ACT_PTT, ACT_PTT, true, false },
};
static const keymap_entry_t map_m[] = {	// GD-77 (7.3)
	{ K_GREEN, ACT_SELECT, ACT_SELECT, false, false }, { K_RED, ACT_BACK, ACT_BACK, false, false },
	{ K_UP, ACT_NAV_UP, ACT_NAV_UP, false, true }, { K_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ K_LEFT, ACT_NAV_LEFT, ACT_NAV_LEFT, false, true }, { K_RIGHT, ACT_NAV_RIGHT, ACT_NAV_RIGHT, false, true },
	{ K_STAR, ACT_QUICK, ACT_QUICK, false, false }, { K_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ K_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false }, { K_0 + 1, ACT_DIGIT_1, ACT_DIGIT_1, false, false },
	{ K_0 + 2, ACT_DIGIT_2, ACT_DIGIT_2, false, false }, { K_0 + 3, ACT_DIGIT_3, ACT_DIGIT_3, false, false },
	{ K_0 + 4, ACT_DIGIT_4, ACT_DIGIT_4, false, false }, { K_0 + 5, ACT_DIGIT_5, ACT_DIGIT_5, false, false },
	{ K_0 + 6, ACT_DIGIT_6, ACT_DIGIT_6, false, false }, { K_0 + 7, ACT_DIGIT_7, ACT_DIGIT_7, false, false },
	{ K_0 + 8, ACT_DIGIT_8, ACT_DIGIT_8, false, false }, { K_0 + 9, ACT_DIGIT_9, ACT_DIGIT_9, false, false },
	{ K_SK1, ACT_MONITOR, ACT_MONITOR, true, false }, { K_SK2, ACT_TALK_TARGET, ACT_USER_2, false, false },
	{ K_TOP, ACT_USER_3, ACT_USER_3, false, false }, { K_PTT, ACT_PTT, ACT_PTT, true, false },
};
static const keymap_entry_t map_v[] = {	// MD-9600 front panel + hand mic (7.4); the encoder sends ENC_STEP
	{ K_GREEN, ACT_SELECT, ACT_SELECT, false, false }, { K_RED, ACT_BACK, ACT_BACK, false, false },
	{ K_P1, ACT_QUICK, ACT_QUICK, false, false }, { K_P2, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ K_UP, ACT_NAV_UP, ACT_NAV_UP, false, true }, { K_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ K_STAR, ACT_QUICK, ACT_QUICK, false, false }, { K_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ K_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false }, { K_0 + 1, ACT_DIGIT_1, ACT_DIGIT_1, false, false },
	{ K_0 + 2, ACT_DIGIT_2, ACT_DIGIT_2, false, false }, { K_0 + 3, ACT_DIGIT_3, ACT_DIGIT_3, false, false },
	{ K_0 + 4, ACT_DIGIT_4, ACT_DIGIT_4, false, false }, { K_0 + 5, ACT_DIGIT_5, ACT_DIGIT_5, false, false },
	{ K_0 + 6, ACT_DIGIT_6, ACT_DIGIT_6, false, false }, { K_0 + 7, ACT_DIGIT_7, ACT_DIGIT_7, false, false },
	{ K_0 + 8, ACT_DIGIT_8, ACT_DIGIT_8, false, false }, { K_0 + 9, ACT_DIGIT_9, ACT_DIGIT_9, false, false },
	{ K_SK1, ACT_MONITOR, ACT_MONITOR, true, false }, { K_SK2, ACT_TALK_TARGET, ACT_USER_2, false, false },
	{ K_TOP, ACT_USER_3, ACT_USER_3, false, false }, { K_PTT, ACT_PTT, ACT_PTT, true, false },
};
static const keymap_entry_t map_f[] = {	// few keys (7.5): Up / Down / Select / Back, PTT
	{ K_GREEN, ACT_SELECT, ACT_SELECT, false, false }, { K_RED, ACT_BACK, ACT_BACK, false, false },
	{ K_UP, ACT_NAV_UP, ACT_NAV_UP, false, true }, { K_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ K_PTT, ACT_PTT, ACT_PTT, true, false },
};
static const uint8_t side_k[] = { K_SK1, K_SK2, K_TOP }, side_m[] = { K_SK1, K_SK2 };	// board.side_keys
static const struct {
	const keymap_entry_t *m;
	uint8_t n;
	uint32_t caps;
	const uint8_t *side;
	uint8_t side_n;
} maps[SIM_MAP_COUNT] = {
	{ map_k, sizeof map_k / sizeof map_k[0], CAP_KEYPAD | CAP_ENCODER | CAP_DMR, side_k, 3 },	// the DM-1701 has the channel knob
	{ map_m, sizeof map_m / sizeof map_m[0], CAP_KEYPAD | CAP_DMR, side_m, 2 },
	{ map_v, sizeof map_v / sizeof map_v[0], CAP_KEYPAD | CAP_ENCODER | CAP_DMR, side_m, 2 },
	{ map_f, sizeof map_f / sizeof map_f[0], CAP_ENCODER | CAP_DMR, NULL, 0 },
};

static void d_init(void) {}
static void d_flush(const void *fb, uint16_t y0, uint16_t y1) { (void)fb; (void)y0; (void)y1; }
static void d_flush_indexed(const void *fb, const uint16_t *pal, uint16_t y0, uint16_t y1)
{
	(void)fb; (void)pal; (void)y0; (void)y1;
}
static void d_backlight(uint8_t p) { (void)p; }
static hal_display_t disp = { .width = 160, .height = 120, .bpp = 16, .init = d_init, .flush = d_flush,
			      .flush_indexed = d_flush_indexed,
			      .set_backlight = d_backlight };
static uint8_t battery = 84;
static uint8_t p_battery(void) { return battery; }
static uint16_t p_supply(void) { return 13800; }	// the sim's view carries the reading (supply:<mv>)
static hal_power_t power = { .battery_pct = p_battery };

static const band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
static const char *const power_names[] = { "50mW", "250mW", "500mW", "750mW", "1W", "2W", "3W", "4W", "5W" };

static bool afsk_tx_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t mark, uint16_t space, uint8_t level)
{
	(void)t; (void)n; (void)baud; (void)mark; (void)space; (void)level;
	return false;
}
static bool afsk_tx_busy(void) { return false; }
static void afsk_tx_stop(void) {}
// A transmit path (the APRS RF rows are live); the UI doesn't key, so it never sends
static const hal_afsk_t afsk = { .tx_start = afsk_tx_start, .tx_busy = afsk_tx_busy, .tx_stop = afsk_tx_stop };
// A vocoder with encode options (Settings > Talk & listen > Mic AGC and Mic noise reduction are live); only options()
// is called
static uint32_t v_options(void) { return VOCODER_OPT_NOISE_REDUCTION | VOCODER_OPT_TONE_FRAMES | VOCODER_OPT_AGC; }
static const hal_vocoder_t vocoder = { .options = v_options };

board_t board = {
	.name = "Simulator", .bands = bands, .band_count = 2, .power_count = 9, .power_names = power_names,
	.power_default = 4, .display = &disp, .power = &power, .afsk = &afsk, .vocoder = &vocoder,
};

static uint32_t now_ms = 1000;
uint32_t hal_millis(void) { return now_ms; }
uint32_t sim_now(void) { return now_ms; }

// Stand-ins for firmware pieces core/ui calls
static bool hotspot_on;
hostlink_mode_t hostlink_mode(void) { return hotspot_on ? HOSTLINK_HOTSPOT : HOSTLINK_IDLE; }
void hostlink_hotspot_exit(void) { hotspot_on = false; }
static bool usb_net;	// the USB personality (Tools > USB network)
static bool usb_up;	// the computer has brought the network adapter up (hal_netif_t.up)

// APRS (Tools > APRS, core/ui/ui_aprs.c): a small stand-in for the engine's view and configuration
static struct {
	bool off;		// "aprs:off": not in this build
	bool no_call;		// "aprs:nocall": no callsign anywhere
	aprs_cfg_t cfg;
	uint32_t sent;
} aprs_sim;
static void aprs_sim_reset(void)
{
	memset(&aprs_sim, 0, sizeof aprs_sim);
	aprs_sim.cfg.region = APRS_REGION_NA;
	aprs_sim.cfg.ssid = 7;
	aprs_sim.cfg.path = APRS_PATH_WIDE1_WIDE2;
	aprs_sim.cfg.level = 50;
}
bool ui_svc_aprs_view(aprs_view_t *v)
{
	static const uint32_t hz[APRS_REGION_COUNT] = { 144390000, 144800000, 145175000, 144575000, 144640000, 144640000, 0 };
	memset(v, 0, sizeof *v);
	if (aprs_sim.off)
		return false;
	if (!aprs_sim.no_call)
		strcpy(v->call, "N0SIM-7");
	v->block = aprs_sim.no_call ? APRS_NO_CALL : APRS_OK;
	v->freq_hz = aprs_sim.cfg.region == APRS_REGION_CUSTOM ? aprs_sim.cfg.freq_hz : hz[aprs_sim.cfg.region % APRS_REGION_COUNT];
	v->pos_valid = !aprs_sim.no_call;
	v->pos_src = APRS_SRC_FIXED;
	v->lat = 337000000;
	v->lon = -841000000;
	v->next_s = aprs_sim.cfg.tx_on ? 245 : 0xFFFFu;
	v->rf_sent = aprs_sim.sent;
	v->stations = 2;
	return true;
}
void ui_svc_aprs_cfg(aprs_cfg_t *c) { *c = aprs_sim.cfg; }
void ui_svc_aprs_set_cfg(const aprs_cfg_t *c) { aprs_sim.cfg = *c; }
bool ui_svc_aprs_send_now(void)
{
	if (aprs_sim.off || aprs_sim.no_call)
		return false;
	aprs_sim.sent++;
	return true;
}
uint8_t ui_svc_aprs_station_count(void) { return aprs_sim.off ? 0 : 2; }
bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out)
{
	static const char *const calls[] = { "W4ABC-9", "KJ4XYZ" };
	if (aprs_sim.off || i >= 2)
		return false;
	memset(out, 0, sizeof *out);
	strcpy(out->call, calls[i]);
	out->heard_ms = now_ms > 60000u * (i + 1u) ? now_ms - 60000u * (i + 1u) : 0;
	out->direct = i == 0;
	return true;
}
bool hostlink_request_usb_network(bool network) { usb_net = network; return true; }
bool hostlink_network_mode(void) { return usb_net; }
static bool netif_up(void) { return usb_net && usb_up; }
static const hal_netif_t sim_netif = { .up = netif_up };
void hotspot_get_view(hotspot_view_t *v)
{
	memset(v, 0, sizeof *v);
	v->running = true;
	v->rx_hz = v->tx_hz = 438800000;
	v->cc = 1;
	v->src = 3101234;
	v->dst = 91;
	v->group = true;
	v->from_net = true;
}
uint32_t input_raw_keys(void) { return 0; }
int32_t input_encoder_total(void) { return 0; }
uint32_t input_ptt_downs(void) { return 0; }
void ui_crit_enter(void) {}
void ui_crit_exit(void) {}

// ---------------- the codeplug (tests/ux: 3 zones, 40 channels, 30 contacts, 20 talkgroups) ----------------

typedef struct { const char *name; uint32_t hz; uint8_t mode; } sim_ch_t;
static const sim_ch_t chans[40] = {
	{ "Base repeater", 146940000, RADIO_MODE_FM }, { "Valley rpt", 147060000, RADIO_MODE_FM },
	{ "COUNTY DMR", 467375000, RADIO_MODE_DMR }, { "County fire", 154250000, RADIO_MODE_FM },
	{ "Metro DMR", 444500000, RADIO_MODE_DMR }, { "Hill top", 442100000, RADIO_MODE_DMR },
	{ "North link", 443450000, RADIO_MODE_DMR }, { "Harbor", 145230000, RADIO_MODE_FM },
	{ "Airport DMR", 441000000, RADIO_MODE_DMR }, { "Lake rpt", 146820000, RADIO_MODE_FM },
	{ "Ridge DMR", 444925000, RADIO_MODE_DMR }, { "Downtown", 443000000, RADIO_MODE_DMR },
	{ "College rpt", 147300000, RADIO_MODE_FM }, { "Johnson Pk", 442500000, RADIO_MODE_DMR },
	{ "Tower DMR", 440700000, RADIO_MODE_DMR }, { "Mill rpt", 145490000, RADIO_MODE_FM },
	// Simplex (FM)
	{ "2M CALLING", 146520000, RADIO_MODE_FM }, { "70CM CALLING", 446000000, RADIO_MODE_FM },
	{ "2M SPX 1", 146550000, RADIO_MODE_FM }, { "2M SPX 2", 146580000, RADIO_MODE_FM },
	{ "70CM SPX 1", 446025000, RADIO_MODE_FM }, { "70CM SPX 2", 446050000, RADIO_MODE_FM },
	{ "MURS 1", 151820000, RADIO_MODE_FM }, { "MURS 2", 151880000, RADIO_MODE_FM },
	{ "NOAA 1", 162550000, RADIO_MODE_FM }, { "NOAA 2", 162400000, RADIO_MODE_FM },
	{ "Marine 16", 156800000, RADIO_MODE_FM }, { "SAR", 155160000, RADIO_MODE_FM },
	// Hotspots (DMR)
	{ "HS 438.8", 438800000, RADIO_MODE_DMR }, { "HS 433.5", 433500000, RADIO_MODE_DMR },
	{ "HS 430.1", 430100000, RADIO_MODE_DMR }, { "DMR SPX 1", 441000000, RADIO_MODE_DMR },
	{ "DMR SPX 2", 446500000, RADIO_MODE_DMR }, { "DMR SPX 3", 446075000, RADIO_MODE_DMR },
	{ "DMR SPX 4", 433450000, RADIO_MODE_DMR }, { "DMR SPX 5", 145510000, RADIO_MODE_DMR },
	{ "Parrot", 438800000, RADIO_MODE_DMR }, { "Brandmeister", 438800000, RADIO_MODE_DMR },
	{ "TGIF", 438800000, RADIO_MODE_DMR }, { "Local TS2", 438800000, RADIO_MODE_DMR },
};
static const struct { const char *name; uint16_t first, count; } zones[3] = {
	{ "Local repeaters", 1, 16 }, { "Simplex", 17, 12 }, { "Hotspots", 29, 12 },
};
typedef struct { const char *name; uint32_t id; uint8_t type; } sim_contact_t;
static const sim_contact_t contacts[] = {
	{ "USA", 3100, CONTACT_GROUP }, { "World", 91, CONTACT_GROUP }, { "N.Amer", 93, CONTACT_GROUP },
	{ "Mass", 3125, CONTACT_GROUP }, { "TAC 310", 310, CONTACT_GROUP }, { "Local", 9, CONTACT_GROUP },
	{ "Parrot", 9990, CONTACT_GROUP }, { "Calif", 3106, CONTACT_GROUP }, { "Texas", 3148, CONTACT_GROUP },
	{ "NY", 3136, CONTACT_GROUP }, { "TAC 311", 311, CONTACT_GROUP }, { "TAC 312", 312, CONTACT_GROUP },
	{ "SKYWARN", 3133, CONTACT_GROUP }, { "ARES", 31064, CONTACT_GROUP }, { "UK", 235, CONTACT_GROUP },
	{ "Germany", 262, CONTACT_GROUP }, { "Canada", 302, CONTACT_GROUP }, { "Mexico", 334, CONTACT_GROUP },
	{ "Main Ops", 201, CONTACT_GROUP }, { "Regional", 8, CONTACT_GROUP },
	{ "Joe Smith", 3101234, CONTACT_PRIVATE }, { "Jodi Ruiz", 3105678, CONTACT_PRIVATE },
	{ "Ann Lee", 3102222, CONTACT_PRIVATE }, { "Bob Kent", 3103333, CONTACT_PRIVATE },
	{ "Carl Diaz", 3104444, CONTACT_PRIVATE }, { "Dana Wu", 3106666, CONTACT_PRIVATE },
	{ "Eve Park", 3107777, CONTACT_PRIVATE }, { "Fred Ng", 3108888, CONTACT_PRIVATE },
	{ "Gail Fox", 3109090, CONTACT_PRIVATE }, { "All call", 0xFFFFFF, CONTACT_ALL_CALL },
};
#define N_CONTACTS	(sizeof contacts / sizeof contacts[0])

// Worst case (sim_worst): the same channels with 16-character names, numbered 985 to 1024; the same contacts with
// 16-character names, 8-digit talkgroups and 7-digit IDs; three zones of 80 channels with 16-character names
static const char *const worst_ch[16] = {
	"Johnson Peak Rpt", "COUNTY FIRE DISP", "Metro DMR Wide 2", "North Link TS2 B",
	"Harbor Pilots Ch", "Airport Ops DMR1", "Lakeshore Rpt 22", "Ridge DMR Statew",
	"Downtown Tac 310", "College Rpt 147.", "Tower DMR Local9", "Mill Valley Rpt.",
	"Base Repeater 01", "Valley Repeat 02", "Hill Top DMR 003", "County Fire 0004"
};
static const char *const worst_zone[3] = { "Northeast Region", "Simplex & Call 2", "Hotspots & Links" };
static const sim_contact_t worst_contacts[] = {
	{ "North America TG", 16776415, CONTACT_GROUP }, { "Worldwide Englsh", 9100091, CONTACT_GROUP },
	{ "Christopher Wood", 3109999, CONTACT_PRIVATE }, { "Alexandra Nguyen", 3108765, CONTACT_PRIVATE },
	{ "Massachusetts St", 3125000, CONTACT_GROUP }, { "TAC 310 Tactical", 31010, CONTACT_GROUP },
};
#define N_WORST	(sizeof worst_contacts / sizeof worst_contacts[0])
#define WORST_FIRST	985u

codeplug_source_t codeplug_source(void) { return CODEPLUG_OPENFW; }

// Channels the channel editor saved (ui_svc_write_channel): they shadow the built-in ones and fill numbers past 40
#define SIM_EDITS	8
static struct { uint16_t number; channel_t ch; } sim_edits[SIM_EDITS];
static uint8_t n_edits;

static const channel_t *sim_edit(uint16_t number)
{
	for (uint8_t i = 0; i < n_edits; i++)
		if (sim_edits[i].number == number)
			return &sim_edits[i].ch;
	return NULL;
}

bool ui_svc_write_channel(uint16_t number, const channel_t *ch)
{
	uint8_t i = 0;
	while (i < n_edits && sim_edits[i].number != number)
		i++;
	if (i == SIM_EDITS)
		return false;
	if (i == n_edits)
		n_edits++;
	sim_edits[i].number = number;
	sim_edits[i].ch = *ch;
	sim_rec.codeplug_writes++;
	return true;
}

uint16_t codeplug_free_channel(void)
{
	for (uint16_t n = worst ? 1 : 41; n <= CODEPLUG_CHANNELS_MAX; n++) {
		if (worst && n >= WORST_FIRST && n < WORST_FIRST + 40u)
			continue;
		if (sim_edit(n) == NULL)
			return n;
	}
	return 0;
}

uint16_t codeplug_channel_count(void)
{
	uint16_t top = worst ? CODEPLUG_CHANNELS_MAX : 40;
	for (uint8_t i = 0; i < n_edits; i++)
		if (sim_edits[i].number > top)
			top = sim_edits[i].number;
	return top;
}

bool codeplug_channel(uint16_t number, channel_t *out)
{
	const channel_t *e = sim_edit(number);
	if (e != NULL) {
		*out = *e;
		return true;
	}
	if (worst) {
		if (number < WORST_FIRST || number >= WORST_FIRST + 40u)
			return false;
		number = (uint16_t)(number - WORST_FIRST + 1u);
	}
	if (number < 1 || number > 40)
		return false;
	const sim_ch_t *c = &chans[number - 1];
	memset(out, 0, sizeof *out);
	strncpy(out->name, worst ? worst_ch[(number - 1) % 16] : c->name, CODEPLUG_NAME_LEN);
	out->rx_hz = out->tx_hz = c->hz;
	if (number <= 2 || number == 10 || number == 13 || number == 16)
		out->tx_hz = c->hz + (c->hz > 147000000 ? 600000 : -600000);	// repeaters
	out->mode = c->mode;
	out->bw = RADIO_BW_25K;
	out->power = CODEPLUG_POWER_DEFAULT;
	if (c->mode == RADIO_MODE_FM && number <= 16) {
		out->tx_tone.type = TONE_CTCSS;
		out->tx_tone.value = 1000;
	}
	out->color_code = 1;
	out->timeslot = 1;
	out->contact = 1;	// TG 3100 USA
	out->listen_list = 1;
	out->rx_only = number == 25 || number == 26;	// NOAA
	return true;
}

uint16_t codeplug_zone_count(void) { return 3; }

// A codeplug written while the radio runs (cpwrite, cpdel:N, zonesize:Z,N): codeplug_serial() moves on
static uint32_t sim_cp_serial;
static bool sim_cp_insert;		// cpwrite: zone 0 starts with channel 40, its other channels one position down
static uint16_t sim_cp_deleted;		// cpdel:N: channel N is in no zone
static uint8_t sim_zone_size[3];	// zonesize:Z,N: zone Z has its first N channels only (0: all)
uint32_t codeplug_serial(void) { return sim_cp_serial; }

bool codeplug_zone(uint16_t index, zone_t *out)
{
	if (index >= 3)
		return false;
	memset(out, 0, sizeof *out);
	if (worst) {
		strncpy(out->name, worst_zone[index], CODEPLUG_NAME_LEN);
		out->count = CODEPLUG_ZONE_MAX;
		for (uint16_t i = 0; i < out->count; i++)
			out->channels[i] = (uint16_t)(WORST_FIRST + (index * 13u + i) % 40u);
		return true;
	}
	strncpy(out->name, zones[index].name, CODEPLUG_NAME_LEN);
	uint16_t n = sim_zone_size[index] ? sim_zone_size[index] : zones[index].count;
	out->count = 0;
	if (index == 0 && sim_cp_insert)
		out->channels[out->count++] = 40;
	for (uint16_t i = 0; i < n; i++)
		if (zones[index].first + i != sim_cp_deleted)
			out->channels[out->count++] = (uint16_t)(zones[index].first + i);
	return true;
}

uint32_t codeplug_dmr_id(void) { return 3101276; }

bool codeplug_contact(uint16_t index, contact_t *out)
{
	if (worst) {
		if (index < 1 || index > N_WORST)
			return false;
		memset(out, 0, sizeof *out);
		strncpy(out->name, worst_contacts[index - 1].name, CODEPLUG_NAME_LEN);
		out->id = worst_contacts[index - 1].id;
		out->type = worst_contacts[index - 1].type;
		return true;
	}
	if (index < 1 || index > N_CONTACTS)
		return false;
	memset(out, 0, sizeof *out);
	strncpy(out->name, contacts[index - 1].name, CODEPLUG_NAME_LEN);
	out->id = contacts[index - 1].id;
	out->type = contacts[index - 1].type;
	return true;
}

bool codeplug_listen_list(uint16_t index, listen_list_t *out)
{
	static const uint16_t l1[] = { 1, 2, 3, 4, 5 };	// USA, World, N.Amer, Mass, TAC 310
	if (index != 1)
		return false;
	memset(out, 0, sizeof *out);
	strcpy(out->name, "County");
	out->count = sizeof l1 / sizeof l1[0];
	memcpy(out->contacts, l1, sizeof l1);
	return true;
}

static const char *contact_name(uint32_t id, bool group)
{
	if (worst) {
		for (size_t i = 0; i < N_WORST; i++)
			if (worst_contacts[i].id == id && (worst_contacts[i].type == CONTACT_GROUP) == group)
				return worst_contacts[i].name;
		return "";
	}
	for (size_t i = 0; i < N_CONTACTS; i++)
		if (contacts[i].id == id && (contacts[i].type == CONTACT_GROUP) == group)
			return contacts[i].name;
	return "";
}

// ---------------- the radio core ----------------

static radio_view_t view;
static uint32_t serial;
static channel_t ch_saved;	// the channel as stored (temporary changes are on view.ch)
static uint32_t temp_target;	// temporary talk target (0 = channel default)
static bool temp_private;
static uint32_t focus_id;
static bool focus_private;
// The VFO's own mode, color code, slot and talk target: kept while on a channel (core/radio/radio_sm.c vfo_ch)
static uint8_t vfo_mode = RADIO_MODE_FM, vfo_cc = 1, vfo_ts = 1, vfo_bw = RADIO_BW_25K;
static uint32_t vfo_rx_hz = 146520000, vfo_tx_hz = 146520000;
static subtone_t vfo_rx_tone, vfo_tx_tone;
static uint32_t vfo_talk;
static bool vfo_talk_private;
// The other VFO (dual watch gives each channel its own), swapped with the ones above by sim_vfo_use
typedef struct {
	uint8_t mode, cc, ts, bw;
	uint32_t rx_hz, tx_hz, talk;
	subtone_t rx_tone, tx_tone;
	bool talk_private;
} sim_vfo_t;
static sim_vfo_t vfo_other;
static void sim_vfo_get(sim_vfo_t *o)
{
	*o = (sim_vfo_t){ vfo_mode, vfo_cc, vfo_ts, vfo_bw, vfo_rx_hz, vfo_tx_hz, vfo_talk, vfo_rx_tone, vfo_tx_tone,
			  vfo_talk_private };
}
static void sim_vfo_use(uint8_t idx)
{
	if (idx == view.vfo_index)
		return;
	if (view.vfo) {		// the changes made in it (bandwidth, tones) are its own
		vfo_rx_hz = view.ch.rx_hz;
		vfo_tx_hz = view.ch.tx_hz;
		vfo_rx_tone = view.ch.rx_tone;
		vfo_tx_tone = view.ch.tx_tone;
		vfo_bw = view.ch.bw;
		vfo_mode = view.ch.mode;
		vfo_cc = view.ch.color_code;
		vfo_ts = view.ch.timeslot;
	}
	sim_vfo_t t = vfo_other;
	sim_vfo_get(&vfo_other);
	vfo_mode = t.mode;
	vfo_cc = t.cc;
	vfo_ts = t.ts;
	vfo_bw = t.bw;
	vfo_rx_hz = t.rx_hz;
	vfo_tx_hz = t.tx_hz;
	vfo_talk = t.talk;
	vfo_rx_tone = t.rx_tone;
	vfo_tx_tone = t.tx_tone;
	vfo_talk_private = t.talk_private;
	view.vfo_index = idx;
}
static ui_dual_ref_t sim_dual[2];
static uint8_t sim_sel;
static uint32_t reply_until;
static uint32_t ptt_up_at;
static bool rx_on;
static uint32_t check_due, check_id;

const radio_view_t *sim_view(void) { return &view; }
void radio_get_view(radio_view_t *v) { *v = view; }
uint32_t radio_view_serial(void) { return serial; }
static int32_t sim_step;	// UI_SET_VFO_STEP: 0 auto (the width's), else 10 Hz units
static void changed(void)
{
	view.vfo_step_hz = sim_step == 833 ? 8333u : sim_step > 0 ? (uint32_t)sim_step * 10u :
			   view.ch.bw == RADIO_BW_25K ? 25000u : 12500u;
	view.vfo_step_auto = sim_step == 0;
	serial++;
}

static void default_target(void)
{
	contact_t c;
	view.dmr.target_manual = temp_target != 0;
	if (view.vfo && !temp_target) {
		view.dmr.target_id = vfo_talk != 0 ? vfo_talk : 9;
		view.dmr.target_private = vfo_talk != 0 && vfo_talk_private;
		strcpy(view.dmr.target_name, contact_name(view.dmr.target_id, !view.dmr.target_private));
	} else if (temp_target) {
		view.dmr.target_id = temp_target;
		view.dmr.target_private = temp_private;
		strcpy(view.dmr.target_name, contact_name(temp_target, !temp_private));
	} else if (codeplug_contact(view.ch.contact, &c)) {
		view.dmr.target_id = c.id;
		view.dmr.target_private = c.type == CONTACT_PRIVATE;
		strcpy(view.dmr.target_name, c.name);
	}
}

static void load_channel(void)
{
	zone_t z;
	codeplug_zone(view.zone_index, &z);
	view.zone_size = z.count;
	if (view.zone_pos >= z.count)
		view.zone_pos = 0;
	strcpy(view.zone_name, z.name);
	view.channel_number = z.channels[view.zone_pos];
	codeplug_channel(view.channel_number, &ch_saved);
	view.ch = ch_saved;
	view.power = board.power_default;
	view.temp = 0;
	view.temporary = false;
	view.squelch = 5;
	temp_target = 0;
	view.dmr.hear_all_latch = false;	// the latch ends with the channel, as in the radio core
	view.dmr.hear_all = view.monitor;
	view.dmr.active = view.ch.mode == RADIO_MODE_DMR;
	view.dmr.codec = true;
	view.dmr.cc = view.ch.color_code;
	view.dmr.ts = view.ch.timeslot;
	view.dmr.reply_active = false;
	reply_until = 0;
	default_target();
	changed();
}

void sim_worst(bool on)
{
	worst = on;
	if (on) {
		view.zone_pos = 2;
		load_channel();
	}
}

static void set_temp(uint8_t bit)
{
	view.temp |= bit;
	view.temporary = true;
	changed();
}

static void retune(void);
static void to_selected(void);
void radio_req_channel_step(int8_t dir)
{
	retune();
	view.vfo = false;
	view.zone_pos = (uint16_t)((view.zone_pos + view.zone_size + dir) % view.zone_size);
	load_channel();
}
void radio_req_zone_pos(uint16_t pos) { retune(); view.vfo = false; view.zone_pos = pos; load_channel(); }
void radio_req_zone(uint16_t index) { retune(); view.zone_index = index; view.zone_pos = 0; load_channel(); }
static void sim_vfo(bool on);
void radio_req_vfo(bool on) { retune(); sim_vfo(on); }
static void sim_vfo(bool on)
{
	view.vfo = on;
	if (on) {
		memset(&view.ch, 0, sizeof view.ch);
		strcpy(view.ch.name, "VFO");
		view.ch.rx_hz = vfo_rx_hz;
		view.ch.tx_hz = vfo_tx_hz;
		view.ch.rx_tone = vfo_rx_tone;
		view.ch.tx_tone = vfo_tx_tone;
		view.ch.bw = vfo_bw;
		view.ch.power = CODEPLUG_POWER_DEFAULT;
		view.ch.mode = vfo_mode;
		view.ch.color_code = vfo_cc;
		view.ch.timeslot = vfo_ts;
		view.dmr.active = vfo_mode == RADIO_MODE_DMR;
		view.dmr.cc = vfo_cc;
		view.dmr.ts = vfo_ts;
		view.channel_number = 0;
		view.temp = 0;		// the VFO has no temporary changes
		view.temporary = false;
		temp_target = 0;
		default_target();
		changed();
	} else {
		load_channel();
	}
}
void radio_req_vfo_step(int8_t dir)
{
	retune();
	view.ch.rx_hz = view.ch.tx_hz = vfo_rx_hz = vfo_tx_hz = view.ch.rx_hz + dir * view.vfo_step_hz;
	changed();
}
void radio_req_vfo_set(uint32_t hz) { retune(); view.ch.rx_hz = view.ch.tx_hz = vfo_rx_hz = vfo_tx_hz = hz; changed(); }
// Copy to VFO as the radio core does (core/radio/radio_sm.c vfo_copy): what is in effect now is the VFO's own
uint8_t ui_svc_vfo_copy(const radio_view_t *v)
{
	if (v->vfo)
		return VFO_COPY_ALREADY;
	if (v->ch.mode == RADIO_MODE_DMR ? !CONFIG_VFO_DMR : !CONFIG_VFO_FM)
		return VFO_COPY_NO_MODE;
	if (v->ch.rx_hz < 136000000 || v->ch.rx_hz > 480000000 || (v->ch.rx_hz > 174000000 && v->ch.rx_hz < 400000000))
		return VFO_COPY_OUT_OF_BAND;
	if (view.scan == 3) {	// dual watch: a VFO the other channel isn't on
		const ui_dual_ref_t *o = &sim_dual[!sim_sel];
		uint8_t idx = view.vfo_index;
		sim_vfo_use(o->vfo && o->pos == idx ? (uint8_t)!idx : idx);
	}
	retune();
	vfo_rx_hz = v->ch.rx_hz;
	vfo_tx_hz = v->ch.tx_hz ? v->ch.tx_hz : v->ch.rx_hz;
	vfo_bw = v->ch.bw;
	vfo_mode = v->ch.mode;
	if (v->ch.mode == RADIO_MODE_DMR) {
		vfo_cc = v->ch.color_code;
		vfo_ts = v->ch.timeslot;
		vfo_talk = v->dmr.target_id;
		vfo_talk_private = v->dmr.target_private;
	} else {
		vfo_rx_tone = v->ch.rx_tone;
		vfo_tx_tone = v->ch.tx_tone;
	}
	sim_vfo(true);
	return VFO_COPY_OK;
}
void radio_req_monitor(bool down)
{
	if (down)
		to_selected();
	// As the radio core (#19): on DMR a short press toggles digital monitor on its release; FM holds it open
	if (view.ch.mode == RADIO_MODE_DMR && !view.monitor) {
		if (!down) {
			view.dmr.hear_all_latch = !view.dmr.hear_all_latch;
			view.dmr.hear_all = view.dmr.hear_all_latch;
		}
	} else {
		view.monitor = down;
	}
	changed();
}
void radio_req_power(int8_t dir)
{
	to_selected();
	view.power = (uint8_t)((view.power + board.power_count + dir) % board.power_count);
	if (!view.vfo)
		set_temp(RADIO_TEMP_POWER);
	changed();
}
void radio_req_squelch(int8_t dir)
{
	to_selected();
	int s = view.squelch + dir;
	view.squelch = (uint8_t)(s < 0 ? 0 : s > 9 ? 9 : s);
	if (!view.vfo)
		set_temp(RADIO_TEMP_SQUELCH);
	changed();
}
void radio_req_clear_temporary(void)
{
	to_selected();
	if (view.vfo)
		return;
	view.ch = ch_saved;
	view.power = board.power_default;
	view.squelch = 5;
	view.temp = 0;
	view.temporary = false;
	view.dmr.active = view.ch.mode == RADIO_MODE_DMR;
	view.dmr.cc = view.ch.color_code;
	view.dmr.ts = view.ch.timeslot;
	temp_target = 0;
	default_target();
	changed();
}
// A setting that retunes ends dual watch on the selected channel, as the radio core does (radio-features.md 2.1): the
// change is made there, and the UI starts the watch again
static void sim_tune(int pane);
static void retune(void)
{
	if (view.scan == 3) {
		sim_tune(sim_sel);
		view.scan = 0;
	}
}
void radio_req_bandwidth(radio_bw_t bw) { retune(); view.ch.bw = bw; if (!view.vfo) set_temp(RADIO_TEMP_BW); changed(); }
void radio_req_tone(uint8_t which, subtone_t tone)
{
	retune();
	if (which & RADIO_TONE_TX) view.ch.tx_tone = tone;
	if (which & RADIO_TONE_RX) view.ch.rx_tone = tone;
	if (!view.vfo)
		set_temp((which & RADIO_TONE_TX ? RADIO_TEMP_TX_TONE : 0) | (which & RADIO_TONE_RX ? RADIO_TEMP_RX_TONE : 0));
	changed();
}
void radio_req_reply_cancel(void) { view.dmr.reply_active = false; reply_until = 0; default_target(); changed(); }

static void ptt(bool down)
{
	if (down) {
		to_selected();	// the radio core transmits on the selected channel
		if (view.ch.rx_only) {
			view.state = RS_TX_DENIED;
			view.tx_deny = TXDENY_RX_ONLY;
			changed();
			return;
		}
		view.state = RS_TX;
		view.tx_elapsed_ms = 0;
		if (view.scan != 3)
			view.scan = 0;	// as the radio core does: PTT ends a scan; dual watch goes on (SCAN_DUAL)
		if (view.dmr.active) {
			if (focus_id) {
				// one-shot: the focused row's target, kept for the reply window
				view.dmr.target_id = focus_id;
				view.dmr.target_private = focus_private;
				strcpy(view.dmr.target_name, contact_name(focus_id, !focus_private));
				view.dmr.reply_active = true;
			}
			view.dmr.state = RDMR_TX;
			sim_rec.tx_id = view.dmr.target_id;
			sim_rec.tx_private = view.dmr.target_private;
		} else {
			sim_rec.tx_id = view.ch.tx_hz;
			sim_rec.tx_private = false;
		}
		sim_rec.tx_count++;
	} else {
		if (view.state == RS_TX && view.dmr.active && view.dmr.reply_active)
			reply_until = now_ms + REPLY_S * 1000u;
		view.state = RS_IDLE;
		view.dmr.state = RDMR_IDLE;
	}
	changed();
}
void radio_req_ptt(bool down) { ptt(down); }

static void rx_start(uint32_t src, uint32_t dst, bool group, const char *alias)
{
	view.state = RS_RX;
	view.rssi_dbm = -71;
	view.dmr.state = RDMR_RX;
	view.dmr.rssi_dbm = -71;
	memset(&view.dmr.call, 0, sizeof view.dmr.call);
	view.dmr.call.src = src;
	view.dmr.call.dst = group ? dst : view.dmr.own_id;
	view.dmr.call.group = group;
	view.dmr.call.to_us = !group;
	strcpy(view.dmr.call.src_name, contact_name(src, false));
	if (group)
		strcpy(view.dmr.call.dst_name, contact_name(dst, true));
	strncpy(view.dmr.alias, alias != NULL ? alias : "", RADIO_ALIAS_MAX);
	rx_on = true;
	if (!group) {
		ui_event_t ev;
		memset(&ev, 0, sizeof ev);
		ev.kind = UI_EV_PRIVATE_CALL;
		ev.id = src;
		strcpy(ev.name, view.dmr.call.src_name);
		ui_notify(&ev);
	}
	changed();
}

static void rx_end(void)
{
	if (!rx_on)
		return;
	rx_on = false;
	view.dmr.last = view.dmr.call;
	view.dmr.last_ms = now_ms;
	view.state = RS_IDLE;
	view.dmr.state = RDMR_IDLE;
	view.dmr.alias[0] = 0;
	view.last_rx_ms = now_ms;
	// The reply window: PTT answers on their talkgroup (or them, privately)
	view.dmr.reply_active = true;
	view.dmr.target_id = view.dmr.last.group ? view.dmr.last.dst : view.dmr.last.src;
	view.dmr.target_private = !view.dmr.last.group;
	strcpy(view.dmr.target_name, view.dmr.last.group ? view.dmr.last.dst_name : view.dmr.last.src_name);
	reply_until = now_ms + REPLY_S * 1000u;
	changed();
}

// ---------------- services other packages own (ui_svc.h) ----------------

typedef struct { ui_msg_t m; uint32_t at; } sim_msg_t;
static sim_msg_t inbox[8];
static uint16_t n_inbox;

// The volume feature (a radio without a knob: the VOL key, Settings > Sound > Volume) only with the "softvol" token, so
// the other screens look as on the radios with a knob
static bool soft_volume;
uint32_t ui_svc_features(void) { return soft_volume ? 0xFFFFFFFFu : 0xFFFFFFFFu & ~UI_FEAT_VOLUME; }
void ui_svc_boot_text(char line1[17], char line2[17])
{
	strcpy(line1, "N0CALL");
	strcpy(line2, "Your Name");	// placeholders: the codeplug's boot text is the user's own
}
bool ui_svc_talk_to(uint32_t id, bool private_call, uint8_t life)
{
	to_selected();
	if (view.vfo && life != UI_TALK_DEFAULT) {
		vfo_talk = id;		// the VFO's own target, kept with it: nothing to write
		vfo_talk_private = private_call;
		default_target();
		changed();
		return true;
	}
	if (life == UI_TALK_DEFAULT) {
		temp_target = 0;
	} else {
		temp_target = id;
		temp_private = private_call;
		if (life == UI_TALK_PERMANENT) {
			sim_rec.codeplug_writes++;
			temp_target = 0;
			for (uint16_t i = 0; i < N_CONTACTS; i++)
				if (contacts[i].id == id && (contacts[i].type == CONTACT_PRIVATE) == private_call)
					ch_saved.contact = view.ch.contact = (uint16_t)(i + 1);
			if (!view.ch.contact || contacts[view.ch.contact - 1].id != id) {
				temp_target = id;	// not in the contacts: stays temporary in this model
			}
		}
	}
	view.dmr.reply_active = false;
	reply_until = 0;
	default_target();
	changed();
	return true;
}
bool ui_svc_save_talk_target(uint32_t id, bool private_call)
{
	return view.vfo && ui_svc_talk_to(id, private_call, UI_TALK_TEMPORARY);	// the VFO has no channel to write
}
void ui_svc_focus_target(uint32_t id, bool private_call)
{
	focus_id = id;
	focus_private = private_call;
	view.dmr.focus_id = id;
	view.dmr.focus_private = id != 0 && private_call;
}
bool ui_svc_dmr_timeslot(uint8_t ts)
{
	retune();
	view.dmr.ts = view.ch.timeslot = ts;
	if (view.vfo) {
		vfo_ts = ts;
		changed();
	} else {
		set_temp(0x20);
	}
	return true;
}
bool ui_svc_dmr_color_code(uint8_t cc)
{
	retune();
	view.dmr.cc = view.ch.color_code = cc;
	if (view.vfo) {
		vfo_cc = cc;
		changed();
	} else {
		set_temp(0x40);
	}
	return true;
}
bool ui_svc_mode(uint8_t mode)
{
	retune();
	view.ch.mode = mode;
	view.dmr.active = mode == RADIO_MODE_DMR;
	if (view.vfo) {
		vfo_mode = mode;
		default_target();
		changed();
	} else {
		set_temp(0x80);
	}
	return true;
}
bool ui_svc_hear_all(bool on)
{
	to_selected();
	view.dmr.hear_all_latch = on;
	view.dmr.hear_all = on || view.monitor;
	changed();
	return true;
}
bool ui_svc_privacy(bool on)
{
	view.dmr.privacy = on && view.dmr.active;
	changed();
	return view.dmr.privacy == on;
}
bool ui_svc_monitor(bool on) { view.monitor = on; changed(); return true; }
bool ui_svc_vox(bool on)
{
	view.vox_on = on && view.ch.mode == RADIO_MODE_FM;
	changed();
	return true;
}
bool ui_svc_save_channel(void)
{
	sim_rec.codeplug_writes++;
	ch_saved = view.ch;
	view.temp = 0;
	view.temporary = false;
	changed();
	return true;
}
bool ui_svc_scan(bool on)
{
	view.scan = on ? (view.vfo ? 2 : 1) : 0;	// scan_kind_t: zone, VFO
	changed();
	return true;
}
// Roaming as the radio core does it, without sites to search: roam now "moves" at once
bool ui_svc_roam(uint8_t cmd)
{
#if CONFIG_DMR_ROAMING
	radio_roam_view_t *r = &view.roam;
	switch (cmd) {
	case RROAM_ZONE_OFF: r->zone_on = false; break;
	case RROAM_ZONE_ON: r->zone_on = true; break;
	case RROAM_ZONE_TOGGLE: r->zone_on = !r->zone_on; break;
	case RROAM_NOW:
		if (r->state != RROAM_STATE_OFF) {
			r->searches++;
			r->roams++;
			r->state = RROAM_STATE_WATCH;
		}
		break;
	case RROAM_LOCK: if (r->state != RROAM_STATE_OFF) r->state = RROAM_STATE_LOCKED; break;
	case RROAM_UNLOCK: if (r->state == RROAM_STATE_LOCKED) r->state = RROAM_STATE_WATCH; break;
	default: break;
	}
	if (!r->zone_on || view.vfo) {
		r->state = RROAM_STATE_OFF;
		r->sites = 0;
	} else if (r->state == RROAM_STATE_OFF) {
		r->state = RROAM_STATE_WATCH;
		r->sites = 3;
		r->rssi_dbm = -92;
	}
	changed();
	return true;
#else
	return false;
#endif
}
void ui_svc_beep(uint8_t beep) { sim_rec.beeps++; sim_rec.last_beep = beep; }
void ui_svc_vibrate(uint8_t vibe) { sim_rec.vibes++; sim_rec.last_vibe = vibe; }
static bool ptt_blocked;	// ui_svc_ptt_block: the input layer keeps PTT presses from the radio
void ui_svc_ptt_block(bool blocked) { ptt_blocked = blocked; }
void ui_svc_apply(uint8_t id, int32_t value)
{
	if (id == UI_SET_LONG_PRESS_MS)
		sim_rec.long_press_ms = (uint32_t)value;
	else if (id == UI_SET_TOT_S)
		sim_rec.tot_s = (uint32_t)value;
	else if (id == UI_SET_BACKLIGHT_S)
		sim_rec.backlight_s = (uint32_t)value;
	else if (id == UI_SET_VFO_STEP) {
		sim_step = value;
		changed();
	}
}
bool ui_svc_scan_skip(void) { radio_req_channel_step(1); return true; }
// Dual watch as the radio core does it with a receiver per channel: the view stays on the selected channel (the
// keys act on it); `tune:a` / `tune:b` move it to a channel, as the alternating receiver does when a call holds it
// Each channel keeps its changes for now while the receiver is on the other (as radio_sm.c's dual_keep)
static uint8_t sim_at = 0xFF;		// the pane the receiver is on
static radio_view_t sim_keep[2];
static uint32_t sim_keep_target[2];
static bool sim_keep_ok[2];
static bool sim_same(const ui_dual_ref_t *a, const ui_dual_ref_t *b)
{
	return a->vfo == b->vfo && a->pos == b->pos && (a->vfo || a->zone == b->zone);
}
bool ui_svc_dual_get(ui_dual_ref_t ref[2], uint8_t *sel)
{
	if (view.scan != 3)
		return false;
	ref[0] = sim_dual[0];
	ref[1] = sim_dual[1];
	*sel = sim_sel;
	return true;
}
bool ui_svc_dual(const ui_dual_ref_t ref[2], uint8_t sel)
{
	if (ref == NULL) {
		if (view.scan == 3)
			sim_tune(sim_sel);	// the radio stops on the selected channel
		view.scan = 0;
		sim_keep_ok[0] = sim_keep_ok[1] = false;
		changed();
		return true;
	}
	for (int i = 0; i < 2; i++) {
		if (!ref[i].vfo) {
			zone_t z;
			if (!codeplug_zone(ref[i].zone, &z) || ref[i].pos >= z.count)
				return false;
		}
		if (!sim_same(&sim_dual[i], &ref[i]))
			sim_keep_ok[i] = false;
	}
	sim_dual[0] = ref[0];
	sim_dual[1] = ref[1];
	sim_sel = sel;
	view.dual_sel = sel;
	// As the radio core: the receiver stays where it is when that is one of the two (a new selection alone never
	// retunes), else it tunes the selected one
	ui_dual_ref_t here = { view.vfo, view.zone_index, view.vfo ? view.vfo_index : view.zone_pos };
	if (sim_same(&here, &ref[sel]))
		sim_at = sel;
	else if (sim_same(&here, &ref[!sel]))
		sim_at = (uint8_t)!sel;
	else
		sim_tune(sel);
	view.scan = 3;	// SCAN_DUAL
	changed();
	return true;
}
// The selected channel for a request that acts on it (power, squelch, the monitor, a talk target), as the radio core
static void to_selected(void)
{
	if (view.scan == 3 && sim_at != sim_sel)
		sim_tune(sim_sel);
}
static void sim_tune(int pane)
{
	const ui_dual_ref_t *r = &sim_dual[pane];
	if (sim_at < 2 && view.scan == 3) {
		sim_keep[sim_at] = view;
		sim_keep_target[sim_at] = temp_target;
		sim_keep_ok[sim_at] = true;
	}
	sim_at = (uint8_t)pane;
	if (r->vfo) {
		sim_vfo_use((uint8_t)r->pos);
		sim_vfo(true);
	} else {
		view.vfo = false;
		view.zone_index = r->zone;
		view.zone_pos = r->pos;
		load_channel();
	}
	if (sim_keep_ok[pane]) {
		const radio_view_t *k = &sim_keep[pane];
		view.ch = k->ch;
		view.power = k->power;
		view.squelch = k->squelch;
		view.temp = k->temp;
		view.temporary = k->temporary;
		view.dmr.hear_all_latch = k->dmr.hear_all_latch;
		temp_target = sim_keep_target[pane];
		default_target();
		changed();
	}
}
// receivers:2 makes the radio one with a receiver per dual watch channel; rxa: / rxb: then start a call on A's or
// B's own receiver (the first call is the one heard), rxaend / rxbend end it
static uint8_t sim_receivers = 1;
static ui_dual_rx_t sim_drx[2];
uint8_t ui_svc_receivers(void) { return sim_receivers; }
bool ui_svc_dual_rx(uint8_t pane, ui_dual_rx_t *out)
{
	if (sim_receivers < 2 || pane > 1)
		return false;
	*out = sim_drx[pane];
	return true;
}
bool ui_svc_vfo_get(uint8_t index, channel_t *out)
{
	sim_vfo_t o;
	if (index != view.vfo_index) {
		o = vfo_other;
	} else {
		sim_vfo_get(&o);
	}
	memset(out, 0, sizeof *out);
	strcpy(out->name, "VFO");
	out->rx_hz = o.rx_hz;
	out->tx_hz = o.tx_hz;
	out->rx_tone = o.rx_tone;
	out->tx_tone = o.tx_tone;
	out->bw = o.bw;
	out->mode = o.mode;
	out->color_code = o.cc;
	out->timeslot = o.ts;
	return true;
}
uint16_t ui_svc_msg_count(void) { return n_inbox; }
uint16_t ui_svc_msg_unread(void)
{
	uint16_t n = 0;
	for (uint16_t i = 0; i < n_inbox; i++)
		n += inbox[i].m.unread;
	return n;
}
bool ui_svc_msg_get(uint16_t i, ui_msg_t *out)
{
	if (i >= n_inbox)
		return false;
	*out = inbox[i].m;
	out->age_ms = now_ms - inbox[i].at;
	return true;
}
void ui_svc_msg_read(uint16_t i) { if (i < n_inbox) inbox[i].m.unread = false; }
bool ui_svc_msg_delete(uint16_t i)
{
	if (i >= n_inbox)
		return false;
	memmove(&inbox[i], &inbox[i + 1], (n_inbox - i - 1u) * sizeof inbox[0]);
	n_inbox--;
	return true;
}
bool ui_svc_msg_send(uint32_t dst, bool group, const char *text)
{
	(void)group;
	sim_rec.messages_sent++;
	strncpy(sim_rec.last_text, text, sizeof sim_rec.last_text - 1);
	sim_rec.tx_id = dst;
	return true;
}
bool ui_svc_radio_check(uint32_t id) { sim_rec.radio_checks++; check_id = id; check_due = now_ms + 800; return true; }
bool ui_svc_call_alert(uint32_t id) { (void)id; sim_rec.call_alerts++; return true; }
bool ui_svc_position(ui_position_t *out)
{
	out->fix = true;
	out->sats = 9;
	out->lat_e7 = 423601000;
	out->lon_e7 = -710589000;
	out->alt_m = 43;
	return true;
}
static bool emergency_on;
bool ui_svc_emergency(bool on) { emergency_on = on; return true; }
bool ui_svc_emergency_active(void) { return emergency_on; }
bool ui_svc_factory_reset(void) { return true; }

// The crash log (ui_svc.h): "crash:on" a record from before this start nobody has seen, "crash:ask" / "crash:auto"
// the same with the report's mode and the first contact (USA, TG 3100) as its target. A report goes at once;
// "crashtx:refuse" makes the next sends fail to start (one still going), "crashtx:fail" makes them end failed
enum { CRASH_TX_SIM_OK, CRASH_TX_SIM_REFUSE, CRASH_TX_SIM_FAIL };
static struct { bool have, seen, sent; uint8_t tx, tx_mode; unsigned sends; } crash_sim;
bool ui_svc_crash_get(ui_crash_t *out)
{
	if (!crash_sim.have)
		return false;
	memset(out, 0, sizeof *out);
	out->seen = crash_sim.seen;
	out->sent = crash_sim.sent;
	out->count = 2;
	out->uptime_ms = 754000;
	out->build = 0x34c825a6u;
	strcpy(out->summary, "Fault 3 at 0802C4A9 in radio");
	return true;
}
void ui_svc_crash_seen(void) { crash_sim.seen = true; }
void ui_svc_crash_clear(void) { crash_sim.have = false; }
bool ui_svc_crash_send(void)
{
	if (!crash_sim.have || ui_svc_setting_get(UI_SET_CRASH_CONTACT) <= 0 || crash_sim.tx_mode == CRASH_TX_SIM_REFUSE)
		return false;
	crash_sim.sends++;
	crash_sim.tx = UI_CRASH_TX_QUEUED;
	return true;
}
uint8_t ui_svc_crash_tx_state(void)
{
	if (crash_sim.tx == UI_CRASH_TX_QUEUED) {
		if (crash_sim.tx_mode == CRASH_TX_SIM_FAIL) {
			crash_sim.tx = UI_CRASH_TX_FAILED;
		} else {
			crash_sim.tx = UI_CRASH_TX_OK;
			crash_sim.sent = crash_sim.seen = true;
		}
	}
	return crash_sim.tx;
}

static void add_message(uint32_t src, const char *text)
{
	if (n_inbox == 8)
		n_inbox--;
	memmove(&inbox[1], &inbox[0], n_inbox * sizeof inbox[0]);
	n_inbox++;
	memset(&inbox[0], 0, sizeof inbox[0]);
	inbox[0].m.src = src;
	inbox[0].m.unread = true;
	inbox[0].at = now_ms;
	strcpy(inbox[0].m.name, contact_name(src, false));
	strncpy(inbox[0].m.text, text, UI_MSG_TEXT_MAX);
	ui_event_t ev;
	memset(&ev, 0, sizeof ev);
	ev.kind = UI_EV_MESSAGE;
	ev.id = src;
	ev.index = 0;
	strcpy(ev.name, inbox[0].m.name);
	strncpy(ev.text, text, sizeof ev.text - 1);
	ui_notify(&ev);
}

// ---------------- time, keys, scripts ----------------

static int presses;
static sim_map_t cur_map;
static const char *shot_dir, *shot_prefix;
int sim_presses(void) { return presses; }
void sim_count_reset(void) { presses = 0; }
void sim_set_shot_dir(const char *dir, const char *prefix) { shot_dir = dir; shot_prefix = prefix; }

static void step(void)
{
	now_ms += TICK_MS;
	if (view.state == RS_TX)
		view.tx_elapsed_ms += TICK_MS;
	if (ptt_up_at && now_ms >= ptt_up_at) {
		ptt_up_at = 0;
		input_event_t ev = { ACT_PTT, GESTURE_UP, 0 };
		ui_app_key(&ev);
		ptt(false);
	}
	if (view.dmr.reply_active && view.state != RS_TX && reply_until) {
		if (now_ms >= reply_until) {
			view.dmr.reply_active = false;
			reply_until = 0;
			default_target();
			changed();
		} else {
			uint8_t s = (uint8_t)((reply_until - now_ms + 999u) / 1000u);
			if (s != view.dmr.reply_s) {
				view.dmr.reply_s = s;
				changed();
			}
		}
	}
	if (check_due && now_ms >= check_due) {
		check_due = 0;
		ui_event_t ev;
		memset(&ev, 0, sizeof ev);
		ev.kind = UI_EV_RADIO_CHECK;
		ev.ok = true;
		ev.id = check_id;
		ev.ms = 800;
		strcpy(ev.name, contact_name(check_id, false));
		ui_notify(&ev);
	}
	view.hotspot = hotspot_on;
	if (ui_app_tick(&view, now_ms))
		ui_app_draw();
}

static void advance(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += TICK_MS)
		step();
}

void sim_reset(const sim_geom_t *g, sim_map_t map)
{
	emergency_on = false;
	disp.width = g->w;
	disp.height = g->h;
	disp.bpp = g->bpp;
	disp.inset_left = disp.inset_right = g->inset;
	disp.inset_top = disp.inset_bottom = g->inset_tb;
	font_aa_on = g->aa;
	font_big_on = g->big;
	worst = false;
	layout_reset();
	board.keymap = maps[map].m;
	board.keymap_len = maps[map].n;
	board.side_keys = maps[map].side;
	board.side_key_count = maps[map].side_n;
	board.caps = maps[map].caps;
	power.supply_mv = NULL;
	cur_map = map;
	gfx_init(&disp);
	gfx_invalidate();
	ui_layout_init();
	memset(&view, 0, sizeof view);
	memset(&sim_rec, 0, sizeof sim_rec);
	vfo_mode = RADIO_MODE_FM;	// a fresh VFO
	vfo_cc = vfo_ts = 1;
	vfo_bw = RADIO_BW_25K;
	vfo_rx_hz = vfo_tx_hz = 146520000;
	sim_receivers = 1;
	memset(sim_drx, 0, sizeof sim_drx);
	vfo_rx_tone = vfo_tx_tone = (subtone_t){ TONE_NONE, 0 };
	vfo_talk = 0;
	vfo_talk_private = false;
	vfo_other = (sim_vfo_t){ RADIO_MODE_FM, 1, 1, RADIO_BW_25K, 146520000, 146520000, 0, { TONE_NONE, 0 },
				 { TONE_NONE, 0 }, false };
	view.vfo_step_hz = 12500;
	view.tot_s = 180;
	view.rssi_dbm = -95;
	view.source = CODEPLUG_OPENFW;
	view.dmr.own_id = 3101276;
	view.zone_index = 0;
	view.zone_pos = 2;	// 3 COUNTY DMR
	n_inbox = 0;
	hotspot_on = rx_on = usb_net = usb_up = false;
	board.netif = &sim_netif;
	aprs_sim_reset();
	n_edits = 0;
	sim_cp_serial = 0;
	sim_cp_insert = false;
	sim_cp_deleted = 0;
	memset(sim_zone_size, 0, sizeof sim_zone_size);
	focus_id = 0;
	check_due = 0;
	ptt_up_at = 0;
	battery = 84;
	now_ms = 1000;
	load_channel();
	soft_volume = false;
	ui_svc_setting_set(UI_SET_VOLUME, RADIO_VOLUME_DEFAULT);
	ui_svc_setting_set(UI_SET_HINTS, 1);
	ui_svc_setting_set(UI_SET_LARGE_TEXT, 0);
	ui_svc_setting_set(UI_SET_BRIGHTNESS, 100);
	ui_svc_setting_set(UI_SET_TOT_S, 180);
	ui_svc_setting_set(UI_SET_BACKLIGHT_S, 30);
	ui_svc_setting_set(UI_SET_LONG_PRESS_MS, 600);	// settings a flow changes start from their defaults
	ui_svc_setting_set(UI_SET_AUTO_LOCK_S, 0);
	ui_svc_setting_set(UI_SET_PTT_LOCKED, 1);
	ui_svc_setting_set(UI_SET_HEAR_ALL_DEFAULT, 0);
	ui_svc_setting_set(UI_SET_NOISE_REDUCTION, 1);
	ui_svc_setting_set(UI_SET_MIC_AGC, 0);
	ui_svc_setting_set(UI_SET_UPDOWN_HOME, 0);
	ui_svc_setting_set(UI_SET_VFO_STEP, 0);
	sim_step = 0;
	for (uint8_t k = UI_SET_KEY_SIDE1; k <= UI_SET_KEY_TOP_HOLD; k++)
		ui_svc_setting_set(k, 0);	// the side keys as the board has them
	ui_svc_setting_set(UI_SET_AUTO_OFF_MIN, 0);
	ui_svc_setting_set(UI_SET_LOW_BATTERY, 15);
	ui_svc_setting_set(UI_SET_THEME, 0);
	ui_svc_setting_set(UI_SET_ACCENT, 0);
	ui_svc_setting_set(UI_SET_STRIP, 1);
	ui_svc_setting_set(UI_SET_ALIAS_SHOW, 1);
	ui_svc_setting_set(UI_SET_MESSAGE_TONE, 1);
	ui_svc_setting_set(UI_SET_VIBRATE, 1);
	ui_svc_setting_set(UI_SET_PRIVACY_TYPE, 0);
	ui_svc_setting_set(UI_SET_PRIVACY_KEY, 0);
	ui_svc_setting_set(UI_SET_ROAM_SURVEY, 1);
	ui_svc_setting_set(UI_SET_ROAM_BEACON_S, 60);
	ui_svc_setting_set(UI_SET_ROAM_BEACON_LEN, 4);
	ui_svc_setting_set(UI_SET_ROAM_PROBE, 0);
	ui_svc_setting_set(UI_SET_CRASH_MODE, UI_CRASH_SEND_OFF);
	ui_svc_setting_set(UI_SET_CRASH_CONTACT, 0);
#if CONFIG_DUAL_WATCH
	ui_svc_setting_set(UI_SET_DUAL_TALK_BACK, 0);
	ui_svc_setting_set(UI_SET_DUAL_B, 0);
	memset(sim_dual, 0, sizeof sim_dual);
	sim_at = 0xFF;
	sim_keep_ok[0] = sim_keep_ok[1] = false;
#endif
	memset(&crash_sim, 0, sizeof crash_sim);
	ptt_blocked = false;
	ui_app_init();
	presses = 0;
	advance(200);
}

static const struct { const char *name; uint8_t action; } keys[] = {
	{ "up", ACT_NAV_UP }, { "down", ACT_NAV_DOWN }, { "left", ACT_NAV_LEFT }, { "right", ACT_NAV_RIGHT },
	{ "ok", ACT_SELECT }, { "back", ACT_BACK }, { "quick", ACT_QUICK }, { "ctx", ACT_CONTEXT },
	{ "tt", ACT_TALK_TARGET }, { "mon", ACT_MONITOR }, { "emerg", ACT_EMERGENCY }, { "user1", ACT_USER_1 },
	{ "user2", ACT_USER_2 }, { "user3", ACT_USER_3 }, { "user4", ACT_USER_4 },
	// what a reassigned side key sends (core/input/keyassign.h)
	{ "powerkey", ACT_POWER }, { "zone+", ACT_ZONE_UP }, { "zone-", ACT_ZONE_DOWN }, { "scankey", ACT_SCAN },
	{ "lockkey", ACT_LOCK }, { "cbkey", ACT_CALL_BACK },
	{ "homekey", ACT_HOME_ZONE }, { "copykey", ACT_COPY_VFO },
	{ "dualkey", ACT_DUAL_WATCH }, { "abkey", ACT_AB },	// dual watch's side key functions
	{ "vfokey", ACT_VFO_TOGGLE },		// the VFO/MR key: a key map entry of the board (DM-1801 Pro), not a side key function
	{ "volkey", ACT_VOLUME },
	{ "0", ACT_DIGIT_0 }, { "1", ACT_DIGIT_1 }, { "2", ACT_DIGIT_2 }, { "3", ACT_DIGIT_3 }, { "4", ACT_DIGIT_4 },
	{ "5", ACT_DIGIT_5 }, { "6", ACT_DIGIT_6 }, { "7", ACT_DIGIT_7 }, { "8", ACT_DIGIT_8 }, { "9", ACT_DIGIT_9 },
};

static bool on_map(uint8_t action, bool lng)
{
	if ((action >= ACT_POWER && action <= ACT_VOLUME) || action == ACT_VFO_TOGGLE || action == ACT_DUAL_WATCH ||
	    action == ACT_AB || action == ACT_TALK_TARGET)
		// any side key can be set to send these; the few-key map has none, but a radio with side keys and no QUICK key
		// is the case the key lock needs on it
		return board.side_key_count != 0 || cur_map == SIM_MAP_F;

	for (int i = 0; i < board.keymap_len; i++) {
		const keymap_entry_t *k = &board.keymap[i];
		if ((!lng && k->short_action == action) || (lng && k->long_action == action))
			return true;
	}
	return false;
}

static bool press(const char *name, bool lng, char *err, size_t len)
{
	if (strcmp(name, "enc+") == 0 || strcmp(name, "enc-") == 0) {
		if (!(board.caps & CAP_ENCODER)) {
			snprintf(err, len, "no encoder on this key map");
			return false;
		}
		input_event_t ev = { ACT_ENC_STEP, GESTURE_DOWN, (int8_t)(name[3] == '+' ? 1 : -1) };
		presses++;
		ui_app_key(&ev);
		advance(PRESS_MS);
		return true;
	}
	// "sk2": the key itself, whatever the key map gives it (keys[] are actions, which any side key can send)
	if (strcmp(name, "sk2") == 0) {
		for (int i = 0; i < board.keymap_len; i++) {
			const keymap_entry_t *k = &board.keymap[i];
			if (k->bit != K_SK2)
				continue;
			input_event_t ev = { lng ? k->long_action : k->short_action,
					     (uint8_t)(lng ? GESTURE_LONG : GESTURE_PRESS), 0 };
			presses++;
			advance(lng ? LONG_MS : PRESS_MS / 2);
			ui_app_key(&ev);
			advance(PRESS_MS / 2);
			return true;
		}
		snprintf(err, len, "no SK2 on this key map");
		return false;
	}
	for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
		if (strcmp(name, keys[i].name) != 0)
			continue;
		uint8_t a = keys[i].action;
		if (!on_map(a, lng) && !(a == ACT_MONITOR && on_map(a, false))) {
			snprintf(err, len, "no key for %s%s on this key map", lng ? "long " : "", name);
			return false;
		}
		presses++;
		if (a == ACT_MONITOR) {
			input_event_t ev = { ACT_MONITOR, GESTURE_DOWN, 0 };
			ui_app_key(&ev);
			radio_req_monitor(true);
			advance(lng ? LONG_MS : PRESS_MS);
			ev.gesture = GESTURE_UP;
			ui_app_key(&ev);
			radio_req_monitor(false);
			return true;
		}
		input_event_t ev = { a, (uint8_t)(lng ? GESTURE_LONG : GESTURE_PRESS), 0 };
		advance(lng ? LONG_MS : PRESS_MS / 2);
		ui_app_key(&ev);
		advance(PRESS_MS / 2);
		return true;
	}
	snprintf(err, len, "unknown key %s", name);
	return false;
}

static const char *screen(void)
{
	ui_app_status_t s;
	ui_app_status(&s);
	return s.screen;
}

static bool check(const char *kv, char *err, size_t len)
{
	char key[32], want[64], got[64];
	const char *eq = strchr(kv, '=');
	if (eq == NULL || eq - kv >= (long)sizeof key) {
		snprintf(err, len, "bad expect %s", kv);
		return false;
	}
	memcpy(key, kv, (size_t)(eq - kv));
	key[eq - kv] = 0;
	snprintf(want, sizeof want, "%s", eq + 1);
	for (char *p = want; *p; p++)
		if (*p == '_')
			*p = ' ';
	ui_app_status_t s;
	ui_app_status(&s);
	bool prefix = false;
	if (strcmp(key, "screen") == 0) snprintf(got, sizeof got, "%s", s.screen);
	else if (strcmp(key, "focus") == 0) { snprintf(got, sizeof got, "%s", ui_app_focus_label()); prefix = true; }
	else if (strcmp(key, "zone") == 0) snprintf(got, sizeof got, "%u", view.zone_index);
	else if (strcmp(key, "pos") == 0) snprintf(got, sizeof got, "%u", view.zone_pos + 1u);
	else if (strcmp(key, "vfo") == 0) snprintf(got, sizeof got, "%u", view.vfo);
	else if (strcmp(key, "freq") == 0) snprintf(got, sizeof got, "%u", (unsigned)view.ch.rx_hz);
	else if (strcmp(key, "mode") == 0) snprintf(got, sizeof got, "%s", view.ch.mode == RADIO_MODE_DMR ? "dmr" : "fm");
	else if (strcmp(key, "tx") == 0) snprintf(got, sizeof got, "%u", (unsigned)sim_rec.tx_count);
	else if (strcmp(key, "txid") == 0) snprintf(got, sizeof got, "%u", (unsigned)sim_rec.tx_id);
	else if (strcmp(key, "txpriv") == 0) snprintf(got, sizeof got, "%u", sim_rec.tx_private);
	else if (strcmp(key, "talk") == 0) snprintf(got, sizeof got, "%u", (unsigned)view.dmr.target_id);
	else if (strcmp(key, "vol") == 0) snprintf(got, sizeof got, "%d", (int)ui_svc_setting_get(UI_SET_VOLUME));
	else if (strcmp(key, "locked") == 0) snprintf(got, sizeof got, "%u", s.locked);
	else if (strcmp(key, "query") == 0) snprintf(got, sizeof got, "%s", s.query);
	else if (strcmp(key, "banner") == 0) { snprintf(got, sizeof got, "%s", s.banner[0] ? s.banner : "none"); prefix = true; }
	else if (strcmp(key, "toast") == 0) { snprintf(got, sizeof got, "%s", s.toast[0] ? s.toast : "none"); prefix = true; }
	else if (strcmp(key, "power") == 0) snprintf(got, sizeof got, "%u", view.power);
	else if (strcmp(key, "sq") == 0) snprintf(got, sizeof got, "%u", view.squelch);
	else if (strcmp(key, "ts") == 0) snprintf(got, sizeof got, "%u", view.dmr.ts);
	else if (strcmp(key, "cc") == 0) snprintf(got, sizeof got, "%u", view.dmr.cc);
	else if (strcmp(key, "temporary") == 0) snprintf(got, sizeof got, "%u", view.temporary);
	else if (strcmp(key, "writes") == 0) snprintf(got, sizeof got, "%u", (unsigned)sim_rec.codeplug_writes);
	else if (strcmp(key, "checks") == 0) snprintf(got, sizeof got, "%u", (unsigned)sim_rec.radio_checks);
	else if (strcmp(key, "sent") == 0) snprintf(got, sizeof got, "%u", (unsigned)sim_rec.messages_sent);
	else if (strcmp(key, "text") == 0) snprintf(got, sizeof got, "%s", sim_rec.last_text);
	else if (strcmp(key, "scanning") == 0) snprintf(got, sizeof got, "%u", view.scan != 0 && view.scan != 3);
#if CONFIG_DUAL_WATCH
	else if (strcmp(key, "dual") == 0) snprintf(got, sizeof got, "%u", ui_st.dual.on && view.scan == 3);
	else if (strcmp(key, "azone") == 0) snprintf(got, sizeof got, "%u", sim_dual[0].zone);
	else if (strcmp(key, "apos") == 0) snprintf(got, sizeof got, "%u", sim_dual[0].pos + 1u);
	else if (strcmp(key, "bzone") == 0) snprintf(got, sizeof got, "%u", sim_dual[1].zone);
	else if (strcmp(key, "bvfo") == 0) snprintf(got, sizeof got, "%u", sim_dual[1].vfo);
	else if (strcmp(key, "bpos") == 0) snprintf(got, sizeof got, "%u", sim_dual[1].pos + 1u);
	else if (strcmp(key, "avfo") == 0) snprintf(got, sizeof got, "%u", sim_dual[0].vfo);
	// What the UI works from (ui_view): in dual watch, the selected channel wherever the receiver is
	else if (strcmp(key, "uipos") == 0) snprintf(got, sizeof got, "%u", ui_view()->zone_pos + 1u);
	else if (strcmp(key, "uitemp") == 0) snprintf(got, sizeof got, "%u", ui_view()->temporary);
	else if (strcmp(key, "entry") == 0) snprintf(got, sizeof got, "%s", ui_st.entry ? ui_st.entry_digits : "none");
	else if (strcmp(key, "sel") == 0) snprintf(got, sizeof got, "%c", ui_st.dual.sel ? 'B' : 'A');
	else if (strcmp(key, "rx") == 0) snprintf(got, sizeof got, "%c", "AB-"[ui_st.dual.rx > 1 ? 2 : ui_st.dual.rx]);
	else if (strcmp(key, "last") == 0) snprintf(got, sizeof got, "%c", "AB-"[ui_st.dual.last > 1 ? 2 : ui_st.dual.last]);
	else if (strcmp(key, "calls") == 0) snprintf(got, sizeof got, "%u", ui_st.dual.calls);
#endif
	else if (strcmp(key, "privacy") == 0) snprintf(got, sizeof got, "%u", view.dmr.privacy);
	else if (strcmp(key, "hearall") == 0) snprintf(got, sizeof got, "%u", view.dmr.hear_all_latch);
	else if (strcmp(key, "vox") == 0) snprintf(got, sizeof got, "%u", view.vox_on);
	else if (strcmp(key, "focusid") == 0) snprintf(got, sizeof got, "%u", (unsigned)view.dmr.focus_id);
	else if (strcmp(key, "longpress") == 0) snprintf(got, sizeof got, "%u", (unsigned)sim_rec.long_press_ms);
	else if (strcmp(key, "monitor") == 0) snprintf(got, sizeof got, "%u", view.monitor);
	else if (strcmp(key, "beeps") == 0) snprintf(got, sizeof got, "%u", (unsigned)sim_rec.beeps);
	else if (strcmp(key, "beep") == 0) snprintf(got, sizeof got, "%u", sim_rec.last_beep);	// ui_beep_t
	else if (strcmp(key, "vibes") == 0) snprintf(got, sizeof got, "%u", (unsigned)sim_rec.vibes);
	else if (strcmp(key, "vibe") == 0) snprintf(got, sizeof got, "%u", sim_rec.last_vibe);	// ui_vibe_t
	else if (strcmp(key, "shows") == 0) {	// a text on the screen (as drawn, after fitting) contains the value
		got[0] = 0;
		for (int i = 0; i < live_n; i++) {
			if (strstr(live[i].s, want) != NULL) {
				snprintf(got, sizeof got, "%s", want);
				break;
			}
			if (strlen(got) + strlen(live[i].s) + 3 < sizeof got) {	// else what is there, for the message
				strcat(got, "|");
				strcat(got, live[i].s);
			}
		}
	}
	else if (strcmp(key, "markafter") == 0 || strcmp(key, "nomarkafter") == 0) {
		// A mark drawn (not a text) within a character cell right of the text that contains the value (an icon
		// beside a word), or none
		bool mark = false;
		for (int i = 0; i < live_n; i++) {
			if (strstr(live[i].s, want) == NULL || live[i].s[0] == 0)
				continue;
			int cw = (live[i].x1 - live[i].x0) / (int)strlen(live[i].s);
			color_t paper = gfx_get_pixel(live[i].x0, live[i].y0);
			for (int py = live[i].y0; py < live[i].y1; py++)
				for (int px = live[i].x1; px < live[i].x1 + cw && px < gfx_width(); px++)
					mark |= gfx_get_pixel(px, py) != paper;
		}
		snprintf(got, sizeof got, "%s", mark == (key[0] == 'm') ? want : mark ? "a mark after it" : "nothing after it");
	}
	else if (strcmp(key, "hides") == 0) {	// no text on the screen contains the value
		snprintf(got, sizeof got, "%s", want);
		for (int i = 0; i < live_n; i++)
			if (strstr(live[i].s, want) != NULL)
				snprintf(got, sizeof got, "shown in \"%s\"", live[i].s);
	}
	else if (strcmp(key, "pttblock") == 0) snprintf(got, sizeof got, "%u", ptt_blocked);
	else if (strcmp(key, "poweroff") == 0) snprintf(got, sizeof got, "%u", ui_app_power_off_due());
	else if (strncmp(key, "set", 3) == 0 && key[3] >= '0' && key[3] <= '9')	// set<N>: ui_setting_t N's value
		snprintf(got, sizeof got, "%d", (int)ui_svc_setting_get((uint8_t)atoi(key + 3)));
	else if (strcmp(key, "radio_tot") == 0) snprintf(got, sizeof got, "%u", (unsigned)sim_rec.tot_s);
	else if (strcmp(key, "radio_backlight") == 0) snprintf(got, sizeof got, "%u", (unsigned)sim_rec.backlight_s);
	else if (strcmp(key, "count") == 0) snprintf(got, sizeof got, "%u", s.count);
	else if (strcmp(key, "depth") == 0) snprintf(got, sizeof got, "%u", s.depth);
	else if (strcmp(key, "hotspot") == 0) snprintf(got, sizeof got, "%u", hotspot_on);
	else if (strcmp(key, "usbnet") == 0) snprintf(got, sizeof got, "%u", usb_net);
	else if (strcmp(key, "aprs_sent") == 0) snprintf(got, sizeof got, "%u", (unsigned)aprs_sim.sent);
	else if (strcmp(key, "crash_sends") == 0) snprintf(got, sizeof got, "%u", crash_sim.sends);
	else if (strcmp(key, "crash_seen") == 0) snprintf(got, sizeof got, "%u", crash_sim.seen);
	else if (strcmp(key, "crash_have") == 0) snprintf(got, sizeof got, "%u", crash_sim.have);
	else if (strcmp(key, "aprs_auto") == 0) snprintf(got, sizeof got, "%u", aprs_sim.cfg.tx_on);
	else if (strcmp(key, "aprs_region") == 0) snprintf(got, sizeof got, "%u", aprs_sim.cfg.region);
	else if (strcmp(key, "aprs_interval") == 0) snprintf(got, sizeof got, "%u", aprs_sim.cfg.interval_s);
	else if (strcmp(key, "aprs_smart") == 0) snprintf(got, sizeof got, "%u", aprs_sim.cfg.smart);
	else if (strcmp(key, "aprs_via") == 0) snprintf(got, sizeof got, "%u", aprs_sim.cfg.via);
	else if (strcmp(key, "aprs_pos") == 0) snprintf(got, sizeof got, "%u", aprs_sim.cfg.pos_src);
	else if (strcmp(key, "aprs_power") == 0) snprintf(got, sizeof got, "%u", aprs_sim.cfg.power);
	else if (strcmp(key, "aprs_path") == 0) snprintf(got, sizeof got, "%u", aprs_sim.cfg.path);
	else if (strcmp(key, "aprs_lat") == 0) snprintf(got, sizeof got, "%d", (int)aprs_sim.cfg.fix_lat);
	else if (strcmp(key, "aprs_lon") == 0) snprintf(got, sizeof got, "%d", (int)aprs_sim.cfg.fix_lon);
	else if (strcmp(key, "aprs_dmrgps") == 0) snprintf(got, sizeof got, "%u", aprs_sim.cfg.dmr_gps);
	else if (strcmp(key, "aprs_kiss") == 0) snprintf(got, sizeof got, "%u", aprs_sim.cfg.kiss);
#if CONFIG_DMR_ROAMING
	else if (strcmp(key, "roam") == 0) snprintf(got, sizeof got, "%u", view.roam.state);	// RROAM_STATE_*
	else if (strcmp(key, "roamzone") == 0) snprintf(got, sizeof got, "%u", view.roam.zone_on);
	else if (strcmp(key, "roams") == 0) snprintf(got, sizeof got, "%u", (unsigned)view.roam.roams);
#endif
	else {
		snprintf(err, len, "unknown expect key %s", key);
		return false;
	}
	bool ok = strcmp(want, "*") == 0 || (prefix ? strncmp(got, want, strlen(want)) == 0 : strcmp(got, want) == 0);
	if (!ok)
		snprintf(err, len, "expected %s=%s, got %s (screen %s)", key, want, got, s.screen);
	return ok;
}

static bool run_token(char *t, char *err, size_t len)
{
	char *arg = strchr(t, ':');
	if (arg != NULL)
		*arg++ = 0;
	if (strcmp(t, "long") == 0)
		return press(arg, true, err, len);
	if (strcmp(t, "ptt") == 0) {
		uint32_t ms = arg != NULL ? (uint32_t)atoi(arg) : 1000u;
		if (!on_map(ACT_PTT, false)) {
			snprintf(err, len, "no PTT");
			return false;
		}
		presses++;
		input_event_t ev = { ACT_PTT, GESTURE_DOWN, 0 };
		ui_app_key(&ev);
		if (!ptt_blocked)
			ptt(true);
		ptt_up_at = now_ms + ms;
		advance(ms + TICK_MS);
		return true;
	}
	if (strcmp(t, "pttdown") == 0) {	// PTT held until pttup (for a look at the TX screen)
		presses++;
		input_event_t ev = { ACT_PTT, GESTURE_DOWN, 0 };
		ui_app_key(&ev);
		if (!ptt_blocked)
			ptt(true);
		advance(1500);
		return true;
	}
	if (strcmp(t, "pttup") == 0) {
		input_event_t ev = { ACT_PTT, GESTURE_UP, 0 };
		ui_app_key(&ev);
		ptt(false);
		advance(200);
		return true;
	}
	if (strcmp(t, "autotx") == 0) {		// the radio keys by itself (an APRS beacon): TX without a PTT event
		ptt(true);
		advance(500);
		return true;
	}
	if (strcmp(t, "autotxend") == 0) {
		ptt(false);
		advance(200);
		return true;
	}
	if (strcmp(t, "mondown") == 0 || strcmp(t, "monup") == 0) {	// the monitor key held, as `mon` sends it
		bool down = t[3] == 'd';
		if (down)
			presses++;
		input_event_t ev = { ACT_MONITOR, down ? GESTURE_DOWN : GESTURE_UP, 0 };
		ui_app_key(&ev);
		radio_req_monitor(down);
		advance(200);
		return true;
	}
	if (strcmp(t, "wait") == 0) {
		advance(arg != NULL ? (uint32_t)atoi(arg) : 1000u);
		return true;
	}
	if (strcmp(t, "rx") == 0 || strcmp(t, "call") == 0) {
		unsigned src = 0, dst = 0;
		char kind = 'g', alias[32] = "";
		if (arg == NULL || sscanf(arg, "%u,%u,%c,%31[^\n]", &src, &dst, &kind, alias) < 2) {
			snprintf(err, len, "bad %s", t);
			return false;
		}
		for (char *p = alias; *p; p++)
			if (*p == '_')
				*p = ' ';
		rx_start(src, dst, kind != 'p', alias);
		if (t[0] == 'c') {
			advance(2000);
			rx_end();
			advance(1000);
		} else {
			advance(300);
		}
		return true;
	}
	if (strcmp(t, "receivers") == 0 && arg != NULL) {
		sim_receivers = (uint8_t)atoi(arg);
		return true;
	}
	if ((strcmp(t, "rxa") == 0 || strcmp(t, "rxb") == 0) && arg != NULL) {
		unsigned src = 0, dst = 0;
		char kind = 'g';
		if (sscanf(arg, "%u,%u,%c", &src, &dst, &kind) < 2) {
			snprintf(err, len, "bad %s", t);
			return false;
		}
		ui_dual_rx_t *x = &sim_drx[t[2] == 'b'];
		memset(x, 0, sizeof *x);
		x->active = x->dmr = true;
		x->heard = !sim_drx[t[2] != 'b'].active;
		x->group = kind != 'p';
		x->src = src;
		x->dst = dst;
		x->rssi_dbm = t[2] == 'b' ? -92 : -71;
		strncpy(x->src_name, contact_name(src, false), sizeof x->src_name - 1);
		changed();
		advance(300);
		return true;
	}
	if (strcmp(t, "rxaend") == 0 || strcmp(t, "rxbend") == 0) {
		ui_dual_rx_t *x = &sim_drx[t[2] == 'b'], *o = &sim_drx[t[2] != 'b'];
		x->active = x->heard = false;
		o->heard = o->active;	// the other call's audio now
		changed();
		advance(300);
		return true;
	}
	if (strcmp(t, "cpwrite") == 0 || (strcmp(t, "cpdel") == 0 && arg != NULL) ||
			(strcmp(t, "zonesize") == 0 && arg != NULL)) {	// the codeplug changes under the radio
		if (t[2] == 'w') {
			sim_cp_insert = true;
		} else if (t[2] == 'd') {
			sim_cp_deleted = (uint16_t)atoi(arg);
		} else {
			unsigned zi = 0, n = 0;
			if (sscanf(arg, "%u,%u", &zi, &n) == 2 && zi < 3)
				sim_zone_size[zi] = (uint8_t)n;
		}
		sim_cp_serial++;
		if (view.scan != 3 && !view.vfo)
			load_channel();
		advance(300);
		return true;
	}
	if (strcmp(t, "dlpos") == 0 && arg != NULL) {	// the debug link (or any other task) asks for channel position N
		radio_req_zone_pos((uint16_t)(atoi(arg) - 1));
		advance(100);
		return true;
	}
	if (strcmp(t, "dldual") == 0 && arg != NULL && view.scan == 0) {	// the debug link's scan dual: the channel
		sim_dual[0] = (ui_dual_ref_t){ view.vfo, view.zone_index,	// here (A, selected) and position N
					       view.vfo ? view.vfo_index : view.zone_pos };
		sim_dual[1] = (ui_dual_ref_t){ false, view.zone_index, (uint16_t)(atoi(arg) - 1) };
		sim_sel = 0;
		sim_at = 0;
		view.dual_sel = 0;
		view.scan = 3;
		changed();
		advance(100);
		return true;
	}
	if (strcmp(t, "dlstop") == 0) {	// the debug link's scan stop: dual watch ends on the selected channel, counted
		if (view.scan == 3) {
			sim_tune(sim_sel);
			view.scan = 0;
			view.dual_stops++;
			changed();
		}
		advance(100);
		return true;
	}
	if (strcmp(t, "dlsel") == 0 && arg != NULL && view.scan == 3) {	// the debug link selects A or B
		sim_sel = arg[0] == 'b';
		view.dual_sel = sim_sel;
		changed();	// no UI tick here: a request right after it races the UI, as on the radio
		return true;
	}
	if (strcmp(t, "tune") == 0 && arg != NULL && (arg[0] == 'a' || arg[0] == 'b')) {	// dual watch: the receiver
		sim_tune(arg[0] == 'b');							// on A or B
		advance(100);
		return true;
	}
	if (strcmp(t, "busy") == 0 && arg != NULL) {	// dual watch: the looks in the wait found a signal there
		view.dual_busy = arg[0] == 'a' ? 1 : arg[0] == 'b' ? 2 : 0;
		changed();
		advance(100);
		return true;
	}
	if (strcmp(t, "fmrx") == 0) {		// an FM signal opens the squelch; fmrxend closes it
		view.state = RS_RX;
		view.rssi_dbm = -84;
		changed();
		advance(300);
		return true;
	}
	if (strcmp(t, "fmrxend") == 0) {
		view.state = RS_IDLE;
		view.last_rx_ms = now_ms;
		changed();
		advance(200);
		return true;
	}
	if (strcmp(t, "rxend") == 0) {
		rx_end();
		advance(200);
		return true;
	}
	if (strcmp(t, "msg") == 0 && arg != NULL) {
		char text[UI_MSG_TEXT_MAX + 1];
		unsigned src = 0;
		if (sscanf(arg, "%u,%143[^\n]", &src, text) != 2) {
			snprintf(err, len, "bad msg");
			return false;
		}
		for (char *p = text; *p; p++)
			if (*p == '_')
				*p = ' ';
		add_message(src, text);
		advance(200);
		return true;
	}
	if ((strcmp(t, "alert") == 0 || strcmp(t, "emergency") == 0 || strcmp(t, "privcall") == 0) && arg != NULL) {
		ui_event_t ev;
		memset(&ev, 0, sizeof ev);
		ev.kind = t[0] == 'a' ? UI_EV_CALL_ALERT : t[0] == 'e' ? UI_EV_EMERGENCY : UI_EV_PRIVATE_CALL;
		ev.id = (uint32_t)atoi(arg);
		strcpy(ev.name, contact_name(ev.id, false));
		ui_notify(&ev);
		advance(200);
		return true;
	}
	if (strcmp(t, "aprs") == 0 && arg != NULL) {	// aprs:off (not built), aprs:nocall, aprs:custom (a custom frequency set)
		aprs_sim.off = strcmp(arg, "off") == 0;
		aprs_sim.no_call = strcmp(arg, "nocall") == 0;
		if (strcmp(arg, "custom") == 0)
			aprs_sim.cfg.freq_hz = 144930000;
		return true;
	}
	if (strcmp(t, "crash") == 0 && arg != NULL) {	// crash:on, crash:ask, crash:auto (see crash_sim)
		crash_sim.have = true;
		crash_sim.seen = crash_sim.sent = false;
		if (strcmp(arg, "on") != 0) {
			ui_svc_setting_set(UI_SET_CRASH_MODE, strcmp(arg, "ask") == 0 ? UI_CRASH_SEND_ASK : UI_CRASH_SEND_AUTO);
			ui_svc_setting_set(UI_SET_CRASH_CONTACT, 1);
		}
		return true;
	}
	if (strcmp(t, "crashtx") == 0 && arg != NULL) {	// crashtx:ok, crashtx:refuse, crashtx:fail (see crash_sim)
		crash_sim.tx_mode = strcmp(arg, "refuse") == 0 ? CRASH_TX_SIM_REFUSE :
				    strcmp(arg, "fail") == 0 ? CRASH_TX_SIM_FAIL : CRASH_TX_SIM_OK;
		return true;
	}
	if (strcmp(t, "usbnet") == 0 && arg != NULL) {
		usb_net = strcmp(arg, "off") != 0;
		usb_up = strcmp(arg, "up") == 0;
		// No changed(): the radio's view doesn't change with the USB link, the UI's own poll must see it
		advance(100);
		return true;
	}
	if (strcmp(t, "hotspot") == 0 && arg != NULL) {
		hotspot_on = strcmp(arg, "on") == 0;
		if (hotspot_on && view.scan != 0)
			view.scan = 0;	// as the radio core: hotspot mode ends any scan, dual watch too
		advance(300);
		return true;
	}
	if (strcmp(t, "reboot") == 0) {		// power off and on: the screens start over, the settings stay
		to_selected();	// the saved position is the selected channel's (radio-features.md 2.1)
		view.scan = 0;
		ui_app_init();
		advance(200);
		return true;
	}
	if (strcmp(t, "splash") == 0) {		// the boot screen (last in a script: nothing redraws over it)
		char l1[17], l2[17];
		ui_svc_boot_text(l1, l2);
		ui_draw_splash(board.name, "a1b2c3d4", l1, l2);
		return true;
	}
	if (strcmp(t, "offfailed") == 0) {	// the housekeeping task couldn't switch off (the knob holds the supply)
		ui_event_t ev;
		memset(&ev, 0, sizeof ev);
		ev.kind = UI_EV_POWER_OFF_FAILED;
		ui_notify(&ev);
		advance(200);
		return true;
	}
	if (strcmp(t, "nobattery") == 0) {
		board.caps |= CAP_NO_BATTERY;
		advance(1100);
		return true;
	}
	if (strcmp(t, "supply") == 0 && arg != NULL) {
		board.caps |= CAP_NO_BATTERY;
		power.supply_mv = p_supply;
		view.supply_mv = (uint16_t)atoi(arg);
		advance(1100);
		return true;
	}
	if ((strcmp(t, "color") == 0 || strcmp(t, "strip") == 0 || strcmp(t, "theme") == 0) && arg != NULL) {
		bool color = t[0] == 'c', theme = t[0] == 't';
		int v = color ? sim_color_find(arg) : theme ? sim_theme_find(arg) :
			strcmp(arg, "on") == 0 ? 1 : strcmp(arg, "off") == 0 ? 0 : -1;
		if (v < 0) {
			snprintf(err, len, "%s: no %s", t, arg);
			return false;
		}
		uint8_t id = color ? UI_SET_ACCENT : theme ? UI_SET_THEME : UI_SET_STRIP;
		ui_svc_setting_set(id, v);
		ui_apply_setting(id, v);
		advance(200);
		return true;
	}
	if (strcmp(t, "battery") == 0 && arg != NULL) {
		battery = (uint8_t)atoi(arg);
		advance(1100);
		return true;
	}
	if (strcmp(t, "shot") == 0 && arg != NULL) {
		if (shot_dir != NULL) {
			char path[512];
			advance(TICK_MS);
			ui_app_draw();
			snprintf(path, sizeof path, "%s/%s%s.png", shot_dir, shot_prefix ? shot_prefix : "", arg);
			if (!sim_save_png(path, 1)) {
				snprintf(err, len, "can't write %s", path);
				return false;
			}
		}
		return true;
	}
	if (strcmp(t, "softvol") == 0) {	// this radio has no volume knob: the board sets the speaker volume
		soft_volume = true;
		advance(200);
		return true;
	}
	if (strcmp(t, "worst") == 0) {		// worst-case codeplug content from here on (sim_worst)
		sim_worst(true);
		advance(200);
		return true;
	}
	if (strcmp(t, "power") == 0 && arg != NULL) {	// power:<level>, the board's level index
		view.power = (uint8_t)atoi(arg);
		changed();
		advance(200);
		return true;
	}
	if (strcmp(t, "expect") == 0 && arg != NULL)
		return check(arg, err, len);
	return press(t, false, err, len);
}

const char *const sim_themes[SIM_THEME_COUNT] = { "dark", "light", "high_contrast" };	// ui_theme_set's order

int sim_theme_find(const char *name)
{
	for (int i = 0; i < SIM_THEME_COUNT; i++)
		if (strcmp(name, sim_themes[i]) == 0)
			return i;
	return -1;
}

int sim_color_find(const char *name)
{
	// Settings > Display > Highlight color, lower case, a space as _ (gallery names are [A-Za-z0-9_])
	static const char *const names[UI_ACCENT_COUNT] = {
		[UI_ACCENT_DEFAULT] = "default", [UI_ACCENT_RED] = "red", [UI_ACCENT_ORANGE] = "orange",
		[UI_ACCENT_YELLOW] = "yellow", [UI_ACCENT_TEAL] = "teal", [UI_ACCENT_BLUE] = "light_blue",
		[UI_ACCENT_PURPLE] = "purple", [UI_ACCENT_PINK] = "pink",
	};
	for (int i = 0; i < UI_ACCENT_COUNT; i++)
		if (strcmp(name, names[i]) == 0)
			return i;
	return -1;
}

bool sim_run(const char *script, char *err, size_t err_len)
{
	char buf[1024];
	snprintf(buf, sizeof buf, "%s", script);
	char *save = NULL;
	for (char *t = strtok_r(buf, " \t\n", &save); t != NULL; t = strtok_r(NULL, " \t\n", &save)) {
		char tok[256];
		snprintf(tok, sizeof tok, "%s", t);
		if (!run_token(tok, err, err_len)) {
			char tmp[256];
			snprintf(tmp, sizeof tmp, "%s (at '%s', screen %s)", err, t, screen());
			snprintf(err, err_len, "%s", tmp);
			return false;
		}
	}
	(void)cur_map;
	return true;
}
