// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// UI drawing on the host: text helpers, row-change flushing, and Home / quick settings rendered at each display
// class (160x120 and 160x128 color, 128x64 mono, 240x320 layout only), written to out/*.ppm for a look.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "board.h"
#include "ui/gfx.h"
#include "ui/fmt.h"
#include "ui/ui_screens.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int flushes, flushed_rows;
static void d_init(void) {}
static void d_flush(const void *fb, uint16_t y0, uint16_t y1) { (void)fb; flushes++; flushed_rows += y1 - y0; }
static void d_backlight(uint8_t p) { (void)p; }
static hal_display_t disp = { .width = 160, .height = 120, .bpp = 16, .init = d_init, .flush = d_flush, .set_backlight = d_backlight };

static const band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
static const char *const power_names[] = { "50m", "250m", "500m", "750m", "1W", "2W", "3W", "4W", "5W" };
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
	char label[12], value[12];
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
	CHECK(ui_quick_tile(7, &v, &s, label, sizeof label, value, sizeof value) && strcmp(label, "8Talk to") == 0);

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

	// 160x120 color: the DM-1701
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
	// The state pill changes color with the state (and says the word)
	color_t idle = gfx_get_pixel(2, 3);
	v.state = RS_TX;
	ui_draw_home(&v, &s);
	CHECK(gfx_get_pixel(2, 3) != idle && gfx_get_pixel(2, 3) == RGB565(224, 32, 32));

	disp.height = 128;
	CHECK(gfx_init(&disp));
	ui_layout_init();
	render_set("160x128");

	// A board safe area (the DM-1701's lens): drawing moves inside it and the margins stay background
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

	printf(fails ? "# FAILED gfxTest: %d\n" : "# PASSED gfxTest\n", fails);
	return fails != 0;
}
