// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UI drawing on the host: text helpers, row-change flushing, and Home / quick settings rendered at each display
// class (160x120 and 160x128 color, 128x64 mono, 240x320 layout only), written to out/*.ppm for a look. The 16-color
// frame buffer (bpp 4: palette, reuse, nearest color, captures) and anti-aliased text.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "board.h"
#include "ui/gfx.h"
#include "ui/fmt.h"
#include "ui/ui_screens.h"
#include "ui/ui_svc.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int flushes, flushed_rows;
static void d_init(void) {}
static void d_flush(const void *fb, uint16_t y0, uint16_t y1) { (void)fb; flushes++; flushed_rows += y1 - y0; }
static uint16_t sent_pal[16];
static void d_flush_indexed(const void *fb, const uint16_t *pal, uint16_t y0, uint16_t y1)
{
	(void)fb;
	memcpy(sent_pal, pal, sizeof sent_pal);
	flushes++;
	flushed_rows += y1 - y0;
}
static void d_backlight(uint8_t p) { (void)p; }
static hal_display_t disp = { .width = 160, .height = 120, .bpp = 16, .init = d_init, .flush = d_flush, .set_backlight = d_backlight };

static const band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
static const char *const power_names[] = { "50mW", "250mW", "500mW", "750mW", "1W", "2W", "3W", "4W", "5W" };
const board_t board = {
	.name = "Test radio", .bands = bands, .band_count = 2, .power_count = 9, .power_names = power_names,
	.power_default = 4, .display = &disp,
};

static void save(const char *name)
{
	char path[128];
	snprintf(path, sizeof path, "out/%s.ppm", name);
	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return;
	int w = gfx_width(), h = gfx_height();
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			color_t c = gfx_get_pixel(x, y);
			unsigned char rgb[3];
			if (gfx_bpp() == 1) {
				rgb[0] = rgb[1] = rgb[2] = c ? 20 : 200;
			} else {
				rgb[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
				rgb[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
				rgb[2] = (unsigned char)((c & 31) * 255 / 31);
			}
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

static void base_view(radio_view_t *v)
{
	memset(v, 0, sizeof *v);
	v->state = RS_IDLE;
	v->zone_pos = 11;
	v->zone_size = 16;
	v->channel_number = 12;
	strcpy(v->zone_name, "Simplex");
	strcpy(v->ch.name, "2M CALLING FREQ");
	v->ch.rx_hz = v->ch.tx_hz = 146520000;
	v->ch.mode = RADIO_MODE_FM;
	v->ch.bw = RADIO_BW_25K;
	v->ch.tx_tone.type = TONE_CTCSS;
	v->ch.tx_tone.value = 1000;
	v->ch.power = CODEPLUG_POWER_DEFAULT;
	v->power = 4;
	v->squelch = 5;
	v->rssi_dbm = -112;
	v->last_rx_ms = 1000;
	v->tot_s = 180;
	v->vfo_step_hz = 12500;
}

static void render_set(const char *cls)
{
	radio_view_t v;
	ui_state_t s;
	char name[64];
	memset(&s, 0, sizeof s);
	s.now_ms = 1000 + 14 * 60000;
	s.battery_pct = 84;
	s.brightness = 100;

	base_view(&v);
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_home_idle", cls); save(name);

	v.state = RS_RX; v.rssi_dbm = -71;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_home_rx", cls); save(name);

	v.state = RS_TX; v.tx_elapsed_ms = 7400; v.power = 7;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_home_tx", cls); save(name);

	v.state = RS_TX_DENIED; v.tx_deny = TXDENY_BAND;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_home_denied", cls); save(name);

	base_view(&v);
	v.vfo = true; v.ch.rx_hz = v.ch.tx_hz = 446006250; strcpy(v.ch.name, "VFO"); v.ch.tx_tone.type = TONE_DCS_INV;
	v.ch.tx_tone.value = 023; v.ch.bw = RADIO_BW_12K5;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_home_vfo", cls); save(name);

	s.entry = true; strcpy(s.entry_digits, "4460"); s.entry_len = 4;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_home_entry", cls); save(name);
	s.entry = false; s.entry_len = 0;

	base_view(&v);
	s.toast = "Menu: coming soon";
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_home_toast", cls); save(name);
	s.toast = NULL;

	s.quick_focus = QT_SQUELCH; s.quick_edit = true;
	ui_draw_quick(&v, &s);
	snprintf(name, sizeof name, "%s_quick", cls); save(name);

	// DMR: Home idle with the talk target and last heard, reply window, no codec, Receiving, Transmitting, refused
	base_view(&v);
	strcpy(v.zone_name, "Local repeaters");
	strcpy(v.ch.name, "COUNTY DMR");
	v.channel_number = 3;
	v.ch.mode = RADIO_MODE_DMR;
	v.ch.rx_hz = 467375000; v.ch.tx_hz = 467375000;
	v.ch.color_code = 2; v.ch.timeslot = 1;
	v.ch.tx_tone.type = TONE_NONE;
	v.dmr.active = true; v.dmr.codec = true; v.dmr.cc = 2; v.dmr.ts = 1; v.dmr.own_id = 3101276;
	v.dmr.target_id = 201; strcpy(v.dmr.target_name, "Main Ops");
	v.dmr.last.src = 3101234; strcpy(v.dmr.last.src_name, "Joe"); v.dmr.last.dst = 91; v.dmr.last.group = true;
	v.dmr.last_ms = s.now_ms - 120000;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_dmr_home", cls); save(name);
	v.dmr.reply_active = true; v.dmr.reply_s = 8; v.dmr.target_id = 91; v.dmr.target_name[0] = 0;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_dmr_reply", cls); save(name);
	v.dmr.reply_active = false; v.dmr.target_id = 201; strcpy(v.dmr.target_name, "Main Ops");
	v.dmr.codec = false;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_dmr_nocodec", cls); save(name);
	v.state = RS_RX; v.dmr.state = RDMR_RX; v.dmr.rssi_dbm = -71;
	v.dmr.call.src = 3101234; strcpy(v.dmr.call.src_name, "JOE SMITH"); v.dmr.call.dst = 201; v.dmr.call.group = true;
	strcpy(v.dmr.call.dst_name, "Main Ops");
	strcpy(v.dmr.alias, "Joe - Boston, MA");
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_dmr_rx_nocodec", cls); save(name);
	v.dmr.codec = true;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_dmr_rx", cls); save(name);
	v.dmr.state = RDMR_RX_OTHER; v.dmr.call.dst = 3100; v.dmr.call.dst_name[0] = 0; v.dmr.call.src_name[0] = 0;
	v.dmr.alias[0] = 0;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_dmr_rx_other", cls); save(name);
	v.state = RS_TX; v.dmr.state = RDMR_TX; v.tx_elapsed_ms = 7400;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_dmr_tx", cls); save(name);
	v.dmr.state = RDMR_TX_WAIT; v.dmr.rmo = true;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_dmr_tx_wake", cls); save(name);
	v.dmr.rmo = false;
	v.state = RS_TX_DENIED; v.tx_deny = TXDENY_NO_CODEC;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_dmr_denied", cls); save(name);
	v.tx_deny = TXDENY_BUSY;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_dmr_busy", cls); save(name);

	// Hotspot screen
	hotspot_view_t h;
	memset(&h, 0, sizeof h);
	h.running = true; h.rx_hz = h.tx_hz = 438800000; h.cc = 1; h.src = 3101276; h.dst = 91; h.group = true;
	h.from_net = true; h.transmitting = true;
	base_view(&v);
	v.hotspot = true;
	ui_draw_hotspot(&v, &h, &s);
	snprintf(name, sizeof name, "%s_hotspot", cls); save(name);
	h.transmitting = false; h.tx_hz = 433800000;
	ui_draw_hotspot(&v, &h, &s);
	snprintf(name, sizeof name, "%s_hotspot_split", cls); save(name);

	// Temporary tone and bandwidth: dots on Home, the Tone editor (TX only) and the tiles
	base_view(&v);
	v.ch.bw = RADIO_BW_12K5;
	v.ch.rx_tone = v.ch.tx_tone;
	v.temp = RADIO_TEMP_BW | RADIO_TEMP_TX_TONE | RADIO_TEMP_RX_TONE;
	v.temporary = true;
	ui_draw_home(&v, &s);
	snprintf(name, sizeof name, "%s_home_temp", cls); save(name);
	char label[20], value[12];
	CHECK(ui_quick_tile(QT_BANDWIDTH, &v, &s, label, sizeof label, value, sizeof value));
	CHECK(strcmp(label, "8Width") == 0 && strcmp(value, GLYPH_DOT "12.5k") == 0);
	s.quick_edit = false;
	CHECK(ui_quick_tile(QT_TONE, &v, &s, label, sizeof label, value, sizeof value));
	CHECK(strcmp(label, "3Tone") == 0 && strcmp(value, GLYPH_DOT "100.0") == 0);
	v.ch.rx_tone.type = TONE_NONE;
	CHECK(ui_quick_tile(QT_TONE, &v, &s, label, sizeof label, value, sizeof value));
	CHECK(strcmp(value, GLYPH_DOT "T100.0") == 0);
	s.quick_focus = QT_TONE; s.quick_edit = true; s.tone_target = RADIO_TONE_TX;
	s.tone_edit.type = TONE_DCS_INV; s.tone_edit.value = 0754;
	CHECK(ui_quick_tile(QT_TONE, &v, &s, label, sizeof label, value, sizeof value));
	CHECK(strcmp(label, "3Tone TX") == 0 && strcmp(value, "D754I") == 0);
	ui_draw_quick(&v, &s);
	snprintf(name, sizeof name, "%s_quick_tone", cls); save(name);
	s.quick_edit = false;
	v.ch.mode = RADIO_MODE_DMR;
	CHECK(!ui_quick_tile(QT_TONE, &v, &s, label, sizeof label, value, sizeof value));
	// On DMR the same positions hold the DMR tiles (information-architecture.md 5): 3 color code, 8 talk target
	CHECK(ui_quick_kind(2, &v) == QT_CC && ui_quick_kind(7, &v) == QT_TALK_TO);
	CHECK(ui_quick_tile(7, &v, &s, label, sizeof label, value, sizeof value) && strncmp(label, "8" TEXT_ALT_OPEN "Talk to", 9) == 0);

	// Layout sanity: regions in order and inside the screen
	const ui_layout_t *L = &ui_layout;
	CHECK(L->channel_y >= L->strip_h);
	CHECK(L->talk_y >= L->channel_y + L->large->h);
	CHECK(L->activity_y >= L->chips_y + L->small->h);
	CHECK(L->activity_h >= L->small->h);
	CHECK(L->hint_y + L->small->h == gfx_height());
	// P5: the channel name and the talk target are the two largest text items
	CHECK(L->large->h >= L->medium->h && L->medium->h >= L->small->h);
}

#define C(i)	RGB565((i) * 15, 255 - (i) * 15, (i) * 7)	// 17 distinct colors

static void test_indexed(void)
{
	hal_display_t d = disp;
	d.width = 160; d.height = 120; d.bpp = 4;
	d.inset_top = d.inset_bottom = d.inset_left = d.inset_right = 0;
	d.flush_indexed = NULL;
	CHECK(!gfx_init(&d));		// bpp 4 needs flush_indexed
	d.flush_indexed = d_flush_indexed;
	CHECK(gfx_init(&d) && gfx_bpp() == 4);

	const color_t red = RGB565(255, 0, 0), green = RGB565(0, 255, 0), blue = RGB565(0, 0, 255);
	gfx_clear(red);
	gfx_fill(10, 10, 5, 5, green);
	gfx_pixel(11, 20, blue);	// odd column: the low nibble
	CHECK(gfx_get_pixel(0, 0) == red && gfx_get_pixel(12, 12) == green && gfx_get_pixel(11, 20) == blue);
	CHECK(gfx_get_pixel(10, 20) == red && gfx_get_pixel(12, 20) == red);
	// A capture reads RGB565, little endian
	uint8_t px[4];
	gfx_read_rgb565((20u * 160u + 10u) * 2u, px, 4);
	CHECK(px[0] == (red & 0xFF) && px[1] == (red >> 8) && px[2] == (blue & 0xFF) && px[3] == (blue >> 8));
	gfx_read_rgb565(160u * 120u * 2u, px, 1);
	CHECK(px[0] == 0);		// past the frame
	// A capture at the panel's depth: the frame buffer's nibbles (high first) index the palette
	const uint8_t *fbb = gfx_framebuffer();
	CHECK(gfx_palette((uint8_t)(fbb[20 * 80 + 5] & 0x0F)) == blue && gfx_palette((uint8_t)(fbb[20 * 80 + 5] >> 4)) == red);
	CHECK(gfx_palette((uint8_t)(fbb[12 * 80 + 6] >> 4)) == green && gfx_palette(15) == 0);	// 15: not set
	flushes = flushed_rows = 0;
	gfx_flush();
	CHECK(flushed_rows == 120 && sent_pal[0] == red);

	// Past 16 colors on the screen a color gets the nearest one already there
	gfx_clear(C(0));
	for (int i = 1; i < 16; i++)
		gfx_pixel(i, 0, C(i));
	gfx_pixel(20, 0, C(16));
	CHECK(gfx_get_pixel(15, 0) == C(15) && gfx_get_pixel(20, 0) == C(15));
	gfx_flush();
	// After a clear the entries are free again: new colors are exact, and the rows go out again even where their
	// indexes didn't change (an entry now stands for another color)
	gfx_clear(C(0));
	gfx_pixel(1, 0, red);
	CHECK(gfx_get_pixel(1, 0) == red);
	flushes = flushed_rows = 0;
	gfx_flush();
	CHECK(flushed_rows == 120);
	flushes = flushed_rows = 0;
	gfx_flush();
	CHECK(flushed_rows == 0);

	// Anti-aliased edges never take the last free entries: with 12 colors on the screen the edges reuse them, and a
	// 13th solid color is still exact
	const font_t *f = &font_8x16;
	CHECK(f->bpp == 2);		// host builds have CONFIG_FONT_AA
	gfx_clear(C(0));
	for (int i = 1; i < 12; i++)
		gfx_pixel(150 + (i & 7), 100 + i / 8, C(i));
	gfx_text(f, 0, 0, "Wg@", RGB565(255, 255, 255));
	bool own = true;
	for (int y = 0; y < f->h; y++)
		for (int x = 0; x < 3 * f->w; x++) {
			color_t c = gfx_get_pixel(x, y);
			bool known = c == RGB565(255, 255, 255);
			for (int i = 0; i < 12; i++)
				known = known || c == C(i);
			own = own && known;
		}
	CHECK(own);
	gfx_pixel(140, 110, red);
	CHECK(gfx_get_pixel(140, 110) == red);
	gfx_init(&disp);
}

// High contrast: the idle pill is white, so its word must be dark (white on white showed a blank pill on the radio)
static void test_idle_pill_contrast(void)
{
	hal_display_t d = disp;
	d.width = 160; d.height = 120; d.bpp = 16;
	d.inset_top = d.inset_bottom = d.inset_left = d.inset_right = 0;
	CHECK(gfx_init(&d));
	ui_layout_init();
	ui_theme_set(2);
	radio_view_t v;
	ui_state_t st;
	memset(&st, 0, sizeof st);
	st.battery_pct = 84;
	base_view(&v);
	gfx_clear(ui_col.bg);
	ui_draw_top(&v, &st, NULL, NULL);
	int dark = 0;
	for (int y = 0; y < ui_layout.small->h; y++)
		for (int x = 2; x < 4 * ui_layout.small->w; x++)
			dark += gfx_get_pixel(x, y) == ui_col.pill_dark_fg;
	CHECK(gfx_get_pixel(1, 1) == ui_col.idle && dark > 10);
	ui_theme_set(0);
	gfx_init(&disp);
}

// CONFIG_FONT_BIG: the 240 px class's lists and labels in the 9x18 font, the other classes unchanged
// The high-contrast theme has no gray: its default filled strip is white; a chosen highlight color stays
static void test_hc_strip_white(void)
{
	ui_accent_set(UI_ACCENT_DEFAULT);
	ui_theme_set(2);
	CHECK(ui_col.strip_bg == RGB565(255, 255, 255));
	ui_accent_set(UI_ACCENT_RED);
	CHECK(ui_col.strip_bg == RGB565(240, 112, 112));
	ui_accent_set(UI_ACCENT_DEFAULT);
	ui_theme_set(0);
	CHECK(ui_col.strip_bg == RGB565(216, 216, 216));
}

static void test_big_text(void)
{
	hal_display_t d = disp;
	d.inset_top = d.inset_bottom = d.inset_left = d.inset_right = 0;
	d.width = 240; d.height = 160; d.bpp = 4;	// the 240 px class in 16 colors (the frame buffer here holds 160 rows)
	d.flush_indexed = d_flush_indexed;
	CHECK(gfx_init(&d));
	ui_layout_init();
	CHECK(ui_layout.small->w == (CONFIG_FONT_BIG ? 9 : 8) && ui_layout.small->h == (CONFIG_FONT_BIG ? 18 : 16));
	CHECK(ui_layout.tiny->w == (CONFIG_FONT_BIG ? 8 : 6) && ui_layout.medium->w == 12);
	CHECK(ui_layout.row_h == ui_layout.small->h + 6);
	d.width = 160; d.height = 128;
	CHECK(gfx_init(&d));
	ui_layout_init();
	CHECK(ui_layout.small->w == 6 && ui_layout.medium->w == 8);
	gfx_init(&disp);
	ui_layout_init();
}

// The splash footer (board and version) stays on a 128 px display with an 11-character board name and the longest
// release name, a beta's: the board name goes before the version is cut
static void test_splash_footer(void)
{
	hal_display_t d = disp;
	d.inset_top = d.inset_bottom = d.inset_left = d.inset_right = 0;
	d.width = 128; d.height = 64; d.bpp = 1;
	CHECK(gfx_init(&d));
	ui_layout_init();
	const char *const versions[] = { "a1b2c3d4-dirty", "v26.12-beta.1", "v26.11.10-beta.10" };
	for (unsigned v = 0; v < 3; v++) {
		ui_draw_splash("Radio 12345", versions[v], "", "");
		int lit = 0;
		for (int y = 64 - ui_layout.tiny->h; y < 64; y++) {
			CHECK(gfx_get_pixel(0, y) == ui_col.bg && gfx_get_pixel(127, y) == ui_col.bg);
			for (int x = 0; x < 128; x++)
				lit += gfx_get_pixel(x, y) != ui_col.bg;
		}
		CHECK(lit > 0);
	}
	gfx_init(&disp);
	ui_layout_init();
}

static void test_aa_text(void)
{
	// RGB565: edges are blends between the text and what is under it
	hal_display_t d = disp;
	d.width = 160; d.height = 120; d.bpp = 16;
	d.inset_top = d.inset_bottom = d.inset_left = d.inset_right = 0;
	CHECK(gfx_init(&d));
	gfx_clear(0);
	gfx_text(&font_12x24, 0, 0, "O", RGB565(255, 255, 255));
	int full = 0, edge = 0;
	for (int y = 0; y < 24; y++)
		for (int x = 0; x < 12; x++) {
			color_t c = gfx_get_pixel(x, y);
			full += c == RGB565(255, 255, 255);
			edge += c != 0 && c != RGB565(255, 255, 255);
			CHECK(((c >> 11) & 31) == (c & 31));	// gray: the blend of white over black
		}
	CHECK(full > 10 && edge > 10);
	// gfx_text_bg: the edges blend with bg
	gfx_text_bg(&font_12x24, 20, 0, "O", RGB565(255, 255, 255), RGB565(0, 0, 255));
	CHECK(gfx_get_pixel(20, 0) == RGB565(0, 0, 255));
	// Mono: ink from 2 of 3 coverage, nothing else
	d.bpp = 1;
	CHECK(gfx_init(&d));
	gfx_clear(0);
	gfx_text(&font_12x24, 0, 0, "O", 1);
	int ink = 0;
	for (int y = 0; y < 24; y++)
		for (int x = 0; x < 12; x++)
			ink += gfx_get_pixel(x, y) == 1;
	CHECK(ink >= full && ink < full + edge);
	gfx_init(&disp);
}

// The UI task's pacing while a capture holds redraws: it waits a frame each pass rather than spinning with a wait of 0,
// which kept the idle task from feeding the watchdog during a 40-chunk color screenshot (the radio restarted)
static void test_pass_wait(void)
{
	CHECK(gfx_pass_wait_ms(false, false, 0, 50, 100) == 100);	// nothing to draw
	CHECK(gfx_pass_wait_ms(false, true, 500, 50, 100) == 100);
	CHECK(gfx_pass_wait_ms(true, false, 20, 50, 100) == 30);	// dirty: until the next frame
	CHECK(gfx_pass_wait_ms(true, false, 50, 50, 100) == 0);	// due now
	CHECK(gfx_pass_wait_ms(true, false, 4000, 50, 100) == 0);
	CHECK(gfx_pass_wait_ms(true, true, 4000, 50, 100) == 50);	// due, but held: a frame, never 0
	CHECK(gfx_pass_wait_ms(true, true, 20, 50, 100) == 50);
	// With a hold running the UI loop's pass sees held; once it ends, the redraw is due again at once
	gfx_hold(1000);
	CHECK(gfx_pass_wait_ms(true, gfx_held(999), 4000, 50, 100) == 50);
	CHECK(gfx_pass_wait_ms(true, gfx_held(1000), 4000, 50, 100) == 0);
	gfx_hold(0);
	CHECK(!gfx_held(5));
}

int main(void)
{
	mkdir("out", 0755);

	char buf[32];
	sbuf_t b;
	sb_init(&b, buf, sizeof buf); sb_freq(&b, 146520000); CHECK(strcmp(buf, "146.520") == 0);
	sb_init(&b, buf, sizeof buf); sb_freq(&b, 446006250); CHECK(strcmp(buf, "446.00625") == 0);
	sb_init(&b, buf, sizeof buf); sb_freq(&b, 145312500); CHECK(strcmp(buf, "145.3125") == 0);
	subtone_t t = { TONE_DCS, 023 };
	sb_init(&b, buf, sizeof buf); sb_tone(&b, &t); CHECK(strcmp(buf, "D023N") == 0);
	t.type = TONE_CTCSS; t.value = 885;
	sb_init(&b, buf, sizeof buf); sb_tone(&b, &t); CHECK(strcmp(buf, "88.5") == 0);
	sb_init(&b, buf, sizeof buf); sb_mmss(&b, 187); CHECK(strcmp(buf, "3:07") == 0);
	sb_init(&b, buf, 6); sb_str(&b, "truncated"); CHECK(strcmp(buf, "trunc") == 0);

	CHECK(gfx_init(&disp));
	char fit[32];
	gfx_text_fit(&font_12x24, "COUNTY DMR REPEATER", 120, fit, sizeof fit);
	CHECK(strlen(fit) == 10 && fit[5] == GLYPH_ELLIPSIS[0] && memcmp(fit, "COUNT", 5) == 0);
	gfx_text_fit(&font_12x24, "SHORT", 120, fit, sizeof fit);
	CHECK(strcmp(fit, "SHORT") == 0);

	// Ages: the longest alternate that fits, then an ellipsis on the shortest (12 px per glyph)
	char age[64];
	sb_init(&b, age, sizeof age); sb_str(&b, "TG9 "); sb_age(&b, 14u * 60000u);
	CHECK(gfx_text_width(&font_12x24, age) == 11 * 12);	// "TG9 14m ago"
	static const struct { int w; const char *want; } ages[] = {
		{ 12 * 12, "TG9 14m ago" }, { 11 * 12, "TG9 14m ago" }, { 10 * 12, "TG9 14m" }, { 7 * 12, "TG9 14m" },
	};
	for (size_t i = 0; i < sizeof ages / sizeof ages[0]; i++) {
		gfx_text_fit(&font_12x24, age, ages[i].w, fit, sizeof fit);
		CHECK(strcmp(fit, ages[i].want) == 0);
	}
	gfx_text_fit(&font_12x24, age, 5 * 12, fit, sizeof fit);
	CHECK(strlen(fit) == 5 && fit[2] == GLYPH_ELLIPSIS[0] && strcmp(fit + 3, "4m") == 0);
	sb_init(&b, age, sizeof age); sb_age(&b, 2u * 86400000u);
	gfx_text_fit(&font_12x24, age, 5 * 12, fit, sizeof fit);
	CHECK(strcmp(fit, "2d") == 0);
	sb_init(&b, age, sizeof age); sb_age_long(&b, 30000u);
	CHECK(strcmp(age, "30s ago") == 0);

	// 160x120 color
	ui_layout_init();
	render_set("160x120");

	// Flushing sends only rows whose content changed
	radio_view_t v;
	ui_state_t s;
	memset(&s, 0, sizeof s);
	s.battery_pct = 84; s.brightness = 100; s.now_ms = 5000;
	base_view(&v);
	gfx_invalidate();
	ui_draw_home(&v, &s);
	flushes = flushed_rows = 0;
	gfx_flush();
	CHECK(flushes == 1 && flushed_rows == 120);
	ui_draw_home(&v, &s);
	flushes = flushed_rows = 0;
	gfx_flush();
	CHECK(flushes == 0);
	v.rssi_dbm = -80;
	ui_draw_home(&v, &s);
	flushes = flushed_rows = 0;
	gfx_flush();
	CHECK(flushes == 1 && flushed_rows > 0 && flushed_rows <= ui_layout.strip_h);
#if CONFIG_NET_WEBCTL
	// The web control's copy: the rows that changed, kept until taken; not readable while a frame is drawn
	uint32_t seq, seq2;
	gfx_rows_take(0, 119);
	CHECK(gfx_row_next(0) < 0);
	gfx_frame_begin();
	CHECK(!gfx_frame_idle(&seq));
	gfx_fill(10, 50, 20, 4, RGB565(255, 0, 255));
	flushed_rows = 0;
	gfx_flush();
	CHECK(gfx_frame_idle(&seq2) && seq2 == seq);
	int first = gfx_row_next(0), last = first, marked = 0;
	for (int y = first; y >= 0; y = gfx_row_next(y + 1), marked++)
		last = y;
	CHECK(first >= 0 && marked == 4 && flushed_rows == 4 && last == first + 3);	// the rows the panel was sent
	gfx_rows_take(first, first);
	CHECK(gfx_row_next(0) == (last > first ? gfx_row_next(first + 1) : -1));
	gfx_rows_take(0, 119);
	gfx_rows_mark_all();
	CHECK(gfx_row_next(0) == 0 && gfx_row_next(119) == 119);
	gfx_rows_take(0, 119);
	CHECK(gfx_row_next(0) < 0);
#endif
	// The state pill changes color with the state (and says the word)
	color_t idle = gfx_get_pixel(2, 3);
	v.state = RS_TX;
	ui_draw_home(&v, &s);
	CHECK(gfx_get_pixel(2, 3) != idle && gfx_get_pixel(2, 3) == RGB565(224, 32, 32));

	disp.height = 128;
	CHECK(gfx_init(&disp));
	ui_layout_init();
	render_set("160x128");

	// A board safe area (a lens over the panel): drawing moves inside it and the margins stay background
	disp.height = 120;
	disp.inset_top = disp.inset_bottom = disp.inset_left = disp.inset_right = 5;
	CHECK(gfx_init(&disp));
	CHECK(gfx_width() == 150 && gfx_height() == 110);
	ui_layout_init();
	render_set("160x120_inset");
	{
		const uint16_t *fb = gfx_framebuffer();
		bool margins_clear = true;
		for (int y = 0; y < 120; y++)
			for (int x = 0; x < 160; x++)
				if ((x < 5 || x >= 155 || y < 5 || y >= 115) && fb[y * 160 + x] != 0)
					margins_clear = false;
		CHECK(margins_clear);
	}
	disp.inset_top = disp.inset_bottom = disp.inset_left = disp.inset_right = 0;

	disp.width = 128; disp.height = 64; disp.bpp = 1;
	CHECK(gfx_init(&disp));
	ui_layout_init();
	render_set("128x64");

	// 240x320 color doesn't fit the frame buffer: gfx refuses it instead of overrunning
	disp.width = 240; disp.height = 320; disp.bpp = 16;
	CHECK(!gfx_init(&disp));

	// At 16 colors it does (37.5 KB), given the rows
	disp.bpp = 4;
	disp.flush_indexed = d_flush_indexed;
	CHECK(gfx_init(&disp) == (GFX_MAX_ROWS >= 320));

	test_indexed();
	test_aa_text();
	test_splash_footer();
	test_pass_wait();
	test_idle_pill_contrast();
	test_big_text();
	test_hc_strip_white();

	printf(fails ? "# FAILED gfxTest: %d\n" : "# PASSED gfxTest\n", fails);
	return fails != 0;
}
