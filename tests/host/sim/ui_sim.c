// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI host simulator (ui_sim.h). Everything core/ui needs from the firmware is simulated here: the board
// descriptor, hal_millis, the radio core (radio_get_view / radio_req_*), the codeplug, the host link and the
// services other packages own (ui_svc.h; the settings store is the weak one from ui_svc_stub.c).
//
// Script tokens (separated by spaces):
//   up down left right ok back quick ctx 0..9 tt mon user1..user4 emerg enc+ enc-   a short press (counted)
//   long:<key>          a long press (counted)
//   ptt / ptt:<ms>      PTT pressed for 1 s / ms (counted); pttdown ... pttup holds it
//   wait:<ms>           time passes
//   rx:<src>,<dst>,g|p[,name]   a DMR call starts (g = group, p = private to us); rxend ends it
//   call:<src>,<dst>,g|p        a 2 s call, then 1 s of quiet
//   msg:<src>,<text>    a text message arrives (_ = space); alert:<src>; emergency:<src>; privcall:<src>
//   hotspot:on|off      the host link enters / leaves hotspot mode
//   shot:<name>         save the screen (sim_set_shot_dir)
//   expect:<key>=<value>   check (see check()); value "*" = anything
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
#include "input/input.h"
#include "hostlink/hostlink.h"
#include "hotspot/hotspot.h"

#define TICK_MS		50
#define PRESS_MS	150
#define LONG_MS		700
#define REPLY_S		10

sim_record_t sim_rec;

const sim_geom_t sim_geoms[] = {
	{ "160x120i5", 160, 120, 16, 5 },	// DM-1701 with its lens inset (CONFIG_DISPLAY_INSET=5)
	{ "160x128", 160, 128, 16, 0 },		// MD-UV380 / MD-9600 class
	{ "128x64", 128, 64, 1, 0 },		// GD-77 class, mono
	{ "240x320", 240, 320, 16, 0 },		// DM-32 class
};
const int sim_geom_count = sizeof sim_geoms / sizeof sim_geoms[0];

const sim_geom_t *sim_geom_find(const char *name)
{
	static sim_geom_t custom;
	for (int i = 0; i < sim_geom_count; i++)
		if (strcmp(name, sim_geoms[i].name) == 0)
			return &sim_geoms[i];
	unsigned w, h, inset = 0;
	char tail = 0;
	int n = sscanf(name, "%ux%u%c%u", &w, &h, &tail, &inset);
	if (n < 2 || w < 64 || h < 48 || w > 320 || h > 320)
		return NULL;
	custom.name = name;
	custom.w = (uint16_t)w;
	custom.h = (uint16_t)h;
	custom.bpp = tail == 'm' ? 1 : 16;
	custom.inset = tail == 'i' ? (uint8_t)inset : 0;
	return &custom;
}

// ---------------- the board ----------------

enum { K_GREEN, K_RED, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_STAR, K_HASH, K_0, K_9 = K_0 + 9, K_SK1, K_SK2, K_TOP,
       K_FN1, K_FN2, K_PTT, K_P1, K_P2 };

static const keymap_entry_t map_k[] = {	// DM-1701 (7.1)
	{ K_GREEN, ACT_SELECT, ACT_SELECT, false, false }, { K_RED, ACT_BACK, ACT_BACK, false, false },
	{ K_UP, ACT_NAV_UP, ACT_NAV_UP, false, true }, { K_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ K_STAR, ACT_QUICK, ACT_QUICK, false, false }, { K_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ K_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false }, { K_0 + 1, ACT_DIGIT_1, ACT_DIGIT_1, false, false },
	{ K_0 + 2, ACT_DIGIT_2, ACT_DIGIT_2, false, false }, { K_0 + 3, ACT_DIGIT_3, ACT_DIGIT_3, false, false },
	{ K_0 + 4, ACT_DIGIT_4, ACT_DIGIT_4, false, false }, { K_0 + 5, ACT_DIGIT_5, ACT_DIGIT_5, false, false },
	{ K_0 + 6, ACT_DIGIT_6, ACT_DIGIT_6, false, false }, { K_0 + 7, ACT_DIGIT_7, ACT_DIGIT_7, false, false },
	{ K_0 + 8, ACT_DIGIT_8, ACT_DIGIT_8, false, false }, { K_0 + 9, ACT_DIGIT_9, ACT_DIGIT_9, false, false },
	{ K_SK1, ACT_MONITOR, ACT_MONITOR, true, false }, { K_SK2, ACT_TALK_TARGET, ACT_USER_2, false, false },
	{ K_TOP, ACT_USER_3, ACT_USER_3, false, false }, { K_FN1, ACT_USER_1, ACT_USER_1, false, false },
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
static const struct { const keymap_entry_t *m; uint8_t n; uint32_t caps; } maps[SIM_MAP_COUNT] = {
	{ map_k, sizeof map_k / sizeof map_k[0], CAP_KEYPAD | CAP_DMR },
	{ map_m, sizeof map_m / sizeof map_m[0], CAP_KEYPAD | CAP_DMR },
	{ map_v, sizeof map_v / sizeof map_v[0], CAP_KEYPAD | CAP_ENCODER | CAP_DMR },
	{ map_f, sizeof map_f / sizeof map_f[0], CAP_ENCODER | CAP_DMR },
};

static void d_init(void) {}
static void d_flush(const void *fb, uint16_t y0, uint16_t y1) { (void)fb; (void)y0; (void)y1; }
static void d_backlight(uint8_t p) { (void)p; }
static hal_display_t disp = { .width = 160, .height = 120, .bpp = 16, .init = d_init, .flush = d_flush,
			      .set_backlight = d_backlight };
static uint8_t battery = 84;
static uint8_t p_battery(void) { return battery; }
static hal_power_t power = { .battery_pct = p_battery };

static const band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
static const char *const power_names[] = { "50m", "250m", "500m", "750m", "1W", "2W", "3W", "4W", "5W" };

board_t board = {
	.name = "Simulator", .bands = bands, .band_count = 2, .power_count = 9, .power_names = power_names,
	.power_default = 4, .display = &disp, .power = &power,
};

static uint32_t now_ms = 1000;
uint32_t hal_millis(void) { return now_ms; }
uint32_t sim_now(void) { return now_ms; }

// Stand-ins for firmware pieces core/ui calls
static bool hotspot_on;
hostlink_mode_t hostlink_mode(void) { return hotspot_on ? HOSTLINK_HOTSPOT : HOSTLINK_IDLE; }
void hostlink_hotspot_exit(void) { hotspot_on = false; }
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

codeplug_source_t codeplug_source(void) { return CODEPLUG_LEGACY; }
uint16_t codeplug_channel_count(void) { return 40; }

bool codeplug_channel(uint16_t number, channel_t *out)
{
	if (number < 1 || number > 40)
		return false;
	const sim_ch_t *c = &chans[number - 1];
	memset(out, 0, sizeof *out);
	strncpy(out->name, c->name, CODEPLUG_NAME_LEN);
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

bool codeplug_zone(uint16_t index, zone_t *out)
{
	if (index >= 3)
		return false;
	memset(out, 0, sizeof *out);
	strncpy(out->name, zones[index].name, CODEPLUG_NAME_LEN);
	out->count = zones[index].count;
	for (uint16_t i = 0; i < out->count; i++)
		out->channels[i] = (uint16_t)(zones[index].first + i);
	return true;
}

uint32_t codeplug_dmr_id(void) { return 3101276; }

bool codeplug_contact(uint16_t index, contact_t *out)
{
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
static uint32_t reply_until;
static uint32_t ptt_up_at;
static bool rx_on;
static bool scanning;
static uint32_t check_due, check_id;

const radio_view_t *sim_view(void) { return &view; }
void radio_get_view(radio_view_t *v) { *v = view; }
uint32_t radio_view_serial(void) { return serial; }
static void changed(void) { serial++; }

static void default_target(void)
{
	contact_t c;
	view.dmr.target_manual = temp_target != 0;
	if (temp_target) {
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
	view.dmr.active = view.ch.mode == RADIO_MODE_DMR;
	view.dmr.codec = true;
	view.dmr.cc = view.ch.color_code;
	view.dmr.ts = view.ch.timeslot;
	view.dmr.reply_active = false;
	reply_until = 0;
	default_target();
	changed();
}

static void set_temp(uint8_t bit)
{
	view.temp |= bit;
	view.temporary = true;
	changed();
}

void radio_req_channel_step(int8_t dir)
{
	view.vfo = false;
	view.zone_pos = (uint16_t)((view.zone_pos + view.zone_size + dir) % view.zone_size);
	load_channel();
}
void radio_req_zone_pos(uint16_t pos) { view.vfo = false; view.zone_pos = pos; load_channel(); }
void radio_req_zone(uint16_t index) { view.zone_index = index; view.zone_pos = 0; load_channel(); }
void radio_req_vfo(bool on)
{
	view.vfo = on;
	if (on) {
		uint32_t hz = 146520000;
		memset(&view.ch, 0, sizeof view.ch);
		strcpy(view.ch.name, "VFO");
		view.ch.rx_hz = view.ch.tx_hz = hz;
		view.ch.bw = RADIO_BW_25K;
		view.ch.power = CODEPLUG_POWER_DEFAULT;
		view.dmr.active = false;
		view.channel_number = 0;
		changed();
	} else {
		load_channel();
	}
}
void radio_req_vfo_step(int8_t dir) { view.ch.rx_hz = view.ch.tx_hz = view.ch.rx_hz + dir * view.vfo_step_hz; changed(); }
void radio_req_vfo_set(uint32_t hz) { view.ch.rx_hz = view.ch.tx_hz = hz; changed(); }
void radio_req_monitor(bool down) { view.monitor = down; changed(); }
void radio_req_power(int8_t dir)
{
	view.power = (uint8_t)((view.power + board.power_count + dir) % board.power_count);
	if (!view.vfo)
		set_temp(RADIO_TEMP_POWER);
	changed();
}
void radio_req_squelch(int8_t dir)
{
	int s = view.squelch + dir;
	view.squelch = (uint8_t)(s < 0 ? 0 : s > 9 ? 9 : s);
	if (!view.vfo)
		set_temp(RADIO_TEMP_SQUELCH);
	changed();
}
void radio_req_clear_temporary(void)
{
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
void radio_req_bandwidth(radio_bw_t bw) { view.ch.bw = bw; if (!view.vfo) set_temp(RADIO_TEMP_BW); changed(); }
void radio_req_tone(uint8_t which, subtone_t tone)
{
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
		if (view.ch.rx_only) {
			view.state = RS_TX_DENIED;
			view.tx_deny = TXDENY_RX_ONLY;
			changed();
			return;
		}
		view.state = RS_TX;
		view.tx_elapsed_ms = 0;
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
static bool hear_all, privacy;

uint32_t ui_svc_features(void) { return 0xFFFFFFFFu; }
bool ui_svc_talk_to(uint32_t id, bool private_call, uint8_t life)
{
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
void ui_svc_focus_target(uint32_t id, bool private_call) { focus_id = id; focus_private = private_call; }
bool ui_svc_dmr_timeslot(uint8_t ts) { view.dmr.ts = view.ch.timeslot = ts; set_temp(0x20); return true; }
bool ui_svc_dmr_color_code(uint8_t cc) { view.dmr.cc = view.ch.color_code = cc; set_temp(0x40); return true; }
bool ui_svc_mode(uint8_t mode)
{
	view.ch.mode = mode;
	view.dmr.active = mode == RADIO_MODE_DMR;
	set_temp(0x80);
	return true;
}
bool ui_svc_hear_all(bool on) { hear_all = on; return true; }
bool ui_svc_privacy(bool on) { privacy = on && view.dmr.active; return privacy == on; }
bool ui_svc_monitor(bool on) { view.monitor = on; changed(); return true; }
bool ui_svc_vox(bool on) { (void)on; return true; }
bool ui_svc_save_channel(void)
{
	sim_rec.codeplug_writes++;
	ch_saved = view.ch;
	view.temp = 0;
	view.temporary = false;
	changed();
	return true;
}
bool ui_svc_scan(bool on) { scanning = on; return true; }
bool ui_svc_scan_active(void) { return scanning; }
bool ui_svc_scan_skip(void) { radio_req_channel_step(1); return true; }
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
bool ui_svc_emergency(bool on) { (void)on; return true; }
bool ui_svc_factory_reset(void) { return true; }

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
	disp.width = g->w;
	disp.height = g->h;
	disp.bpp = g->bpp;
	disp.inset_top = disp.inset_bottom = disp.inset_left = disp.inset_right = g->inset;
	board.keymap = maps[map].m;
	board.keymap_len = maps[map].n;
	board.caps = maps[map].caps;
	cur_map = map;
	gfx_init(&disp);
	gfx_invalidate();
	ui_layout_init();
	memset(&view, 0, sizeof view);
	memset(&sim_rec, 0, sizeof sim_rec);
	view.vfo_step_hz = 12500;
	view.tot_s = 180;
	view.rssi_dbm = -95;
	view.source = CODEPLUG_LEGACY;
	view.dmr.own_id = 3101276;
	view.zone_index = 0;
	view.zone_pos = 2;	// 3 COUNTY DMR
	n_inbox = 0;
	hear_all = privacy = scanning = hotspot_on = rx_on = false;
	focus_id = 0;
	check_due = 0;
	ptt_up_at = 0;
	battery = 84;
	now_ms = 1000;
	load_channel();
	ui_svc_setting_set(UI_SET_HINTS, 1);
	ui_svc_setting_set(UI_SET_LARGE_TEXT, 0);
	ui_svc_setting_set(UI_SET_BRIGHTNESS, 100);
	ui_app_init();
	presses = 0;
	advance(200);
}

static const struct { const char *name; uint8_t action; } keys[] = {
	{ "up", ACT_NAV_UP }, { "down", ACT_NAV_DOWN }, { "left", ACT_NAV_LEFT }, { "right", ACT_NAV_RIGHT },
	{ "ok", ACT_SELECT }, { "back", ACT_BACK }, { "quick", ACT_QUICK }, { "ctx", ACT_CONTEXT },
	{ "tt", ACT_TALK_TARGET }, { "mon", ACT_MONITOR }, { "emerg", ACT_EMERGENCY }, { "user1", ACT_USER_1 },
	{ "user2", ACT_USER_2 }, { "user3", ACT_USER_3 }, { "user4", ACT_USER_4 },
	{ "0", ACT_DIGIT_0 }, { "1", ACT_DIGIT_1 }, { "2", ACT_DIGIT_2 }, { "3", ACT_DIGIT_3 }, { "4", ACT_DIGIT_4 },
	{ "5", ACT_DIGIT_5 }, { "6", ACT_DIGIT_6 }, { "7", ACT_DIGIT_7 }, { "8", ACT_DIGIT_8 }, { "9", ACT_DIGIT_9 },
};

static bool on_map(uint8_t action, bool lng)
{
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
	else if (strcmp(key, "scanning") == 0) snprintf(got, sizeof got, "%u", scanning);
	else if (strcmp(key, "privacy") == 0) snprintf(got, sizeof got, "%u", privacy);
	else if (strcmp(key, "hearall") == 0) snprintf(got, sizeof got, "%u", hear_all);
	else if (strcmp(key, "monitor") == 0) snprintf(got, sizeof got, "%u", view.monitor);
	else if (strcmp(key, "count") == 0) snprintf(got, sizeof got, "%u", s.count);
	else if (strcmp(key, "depth") == 0) snprintf(got, sizeof got, "%u", s.depth);
	else if (strcmp(key, "hotspot") == 0) snprintf(got, sizeof got, "%u", hotspot_on);
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
		ptt(true);
		ptt_up_at = now_ms + ms;
		advance(ms + TICK_MS);
		return true;
	}
	if (strcmp(t, "pttdown") == 0) {	// PTT held until pttup (for a look at the TX screen)
		presses++;
		input_event_t ev = { ACT_PTT, GESTURE_DOWN, 0 };
		ui_app_key(&ev);
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
	if (strcmp(t, "hotspot") == 0 && arg != NULL) {
		hotspot_on = strcmp(arg, "on") == 0;
		advance(300);
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
	if (strcmp(t, "expect") == 0 && arg != NULL)
		return check(arg, err, len);
	return press(t, false, err, len);
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
