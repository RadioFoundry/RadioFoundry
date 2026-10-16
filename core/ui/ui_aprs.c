// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Tools > APRS (docs/spec/aprs.md 10): what APRS is doing (callsign, frequency, position, next beacon, or why nothing
// can be sent) and the everyday choices, one row each: send now, automatic beacons, the frequency (region), the RF
// power (as the channel, or a level of its own), the digipeater path (the hops), the interval, SmartBeaconing, the
// way out (radio / DMR / both), the position source, our position in DMR voice calls (ETSI GPS Info, CONFIG_DMR_GPS),
// KISS on the host port, and the stations heard (receive builds). Position opens its own page: the source and the fixed
// latitude and longitude, typed with the digits. The rest (symbol, comment, digital destination, iGate, digipeater) is
// set with qdmr, the debug link or tools/odr_debug.py (aprs.md 11).
#include <string.h>
#include "ui_nav.h"
#include "ui_svc_aprs.h"
#include "fmt.h"
#include "config.h"
#include "board.h"

// The services when APRS isn't built (and in the UI simulator): nothing there
#define WEAK __attribute__((weak))
WEAK bool ui_svc_aprs_view(aprs_view_t *v) { memset(v, 0, sizeof(*v)); return false; }
WEAK void ui_svc_aprs_cfg(aprs_cfg_t *c) { memset(c, 0, sizeof(*c)); }
WEAK void ui_svc_aprs_set_cfg(const aprs_cfg_t *c) { (void)c; }
WEAK bool ui_svc_aprs_send_now(void) { return false; }
WEAK uint8_t ui_svc_aprs_station_count(void) { return 0; }
WEAK bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out) { (void)i; (void)out; return false; }

enum { AP_SEND, AP_AUTO, AP_FREQ, AP_POWER, AP_PATH, AP_INTERVAL, AP_SMART, AP_VIA, AP_POS, AP_DMR_GPS, AP_KISS, AP_HEARD, AP_COUNT };

static const uint16_t intervals[] = { 0, 60, 120, 300, 600, 1800 };
static const char *const interval_names[] = { "Off", "1 min", "2 min", "5 min", "10 min", "30 min" };
#define N_INTERVALS	(sizeof(intervals) / sizeof(intervals[0]))
// aprs_path_t order; the text is aprs_path_text()'s, kept here so the page builds without APRS (the simulator)
static const char *const path_names[APRS_PATH_COUNT] = { "None", "WIDE1-1", "WIDE1-1,WIDE2-1", "WIDE2-1", "WIDE2-2" };
static const char *const via_names[] = { "Radio", "DMR", "Both" };
static const char *const pos_names[] = { "Auto", "GPS", "Computer", "Fixed" };
static const char *const region_names[APRS_REGION_COUNT] = { "N America", "Europe", "Australia", "New Zealand",
							      "Japan", "China", "Custom" };

// The radio can send APRS on RF (a board with an AFSK transmit path; some only receive)
static bool rf_send(void)
{
	return board.afsk != NULL && board.afsk->tx_start != NULL;
}

static const char *why_text(uint8_t block)
{
	switch (block) {
	case APRS_NO_CALL: return "Set your callsign (the radio name) in qdmr.";
	case APRS_NO_FREQ: return "No APRS frequency: pick a region.";
	case APRS_NOT_AMATEUR: return "Pick a frequency in a ham band this radio sends on.";
	case APRS_NO_AFSK: return "No APRS tones on this radio; DMR positions work.";
	default: return NULL;
	}
}

static const char *aprs_body(void)
{
	static char t[160];
	sbuf_t b;
	aprs_view_t v;
	sb_init(&b, t, sizeof t);
	if (!ui_svc_aprs_view(&v)) {
		sb_str(&b, "APRS: not in this build");
		return t;
	}
	sb_str(&b, v.call[0] != '\0' ? v.call : "No callsign");
	sb_str(&b, " on ");
	sb_freq(&b, v.freq_hz);
	if (v.rf_active) {
		sb_str(&b, " (sending)");
	}
	const char *why = why_text(v.block);
	if (why != NULL) {
		sb_chr(&b, '\n');
		sb_str(&b, why);
	}
	sb_chr(&b, '\n');
	if (v.pos_valid) {
		sb_str(&b, pos_names[v.pos_src < 4 ? v.pos_src : 0]);
		sb_chr(&b, ' ');
		sb_degrees(&b, v.lat);
		sb_chr(&b, ' ');
		sb_degrees(&b, v.lon);
	} else {
		sb_str(&b, "No position");
	}
	if (v.next_s != 0xFFFFu) {
		sb_str(&b, "\nNext in ");
		sb_mmss(&b, v.next_s);
	}
	if (v.rf_sent + v.dmr_sent != 0) {
		sb_str(&b, "  Sent ");
		sb_u32(&b, v.rf_sent + v.dmr_sent, 1);
	}
	return t;
}

static uint16_t aprs_count(void) { return AP_COUNT; }

static uint8_t interval_index(uint16_t s)
{
	for (uint8_t i = 0; i < N_INTERVALS; i++) {
		if (intervals[i] >= s) {
			return i;
		}
	}
	return (uint8_t)(N_INTERVALS - 1u);
}

static void aprs_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { "Send position now", "Automatic beacons", "Frequency", "Power", "Path",
					     "Interval", "SmartBeaconing", "Send via", "Position", "Position in DMR calls",
					     "KISS on USB", "Stations heard" };
	aprs_view_t v;
	aprs_cfg_t c;
	bool built = ui_svc_aprs_view(&v);
	ui_svc_aprs_cfg(&c);
	strcpy(it->label, names[i]);
	it->num = i < 10u ? (char)('0' + (i + 1u) % 10u) : 0;	// rows after the 10th have no digit
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (!built) {
		it->flags |= UI_ITEM_DIM;
		return;
	}
	switch (i) {
	case AP_SEND:
		if (v.block != APRS_OK && c.via == APRS_VIA_RF) {
			it->flags |= UI_ITEM_DIM;
		}
		break;
	case AP_AUTO: sb_str(&b, c.tx_on ? "On" : "Off"); break;
	case AP_FREQ:
		if (board.afsk == NULL && !CONFIG_APRS_RX) {
			it->flags |= UI_ITEM_DIM;	// no APRS on RF here
		} else if (c.region == APRS_REGION_CUSTOM) {
			sb_freq(&b, c.freq_hz);
		} else {
			sb_str(&b, region_names[c.region < APRS_REGION_COUNT ? c.region : 0]);
		}
		break;
	case AP_POWER:
		if (!rf_send()) {
			it->flags |= UI_ITEM_DIM;	// no APRS on RF here (no transmit tone path)
		} else {
			sb_str(&b, c.power == 0 ? "As channel" : ui_power_name((uint8_t)(c.power - 1u)));
		}
		break;
	case AP_PATH:
		if (!rf_send()) {
			it->flags |= UI_ITEM_DIM;	// the path is for RF packets only
		} else {
			sb_str(&b, path_names[c.path < APRS_PATH_COUNT ? c.path : 0]);
		}
		break;
	case AP_INTERVAL: sb_str(&b, interval_names[interval_index(c.interval_s)]); break;
	case AP_SMART: sb_str(&b, c.smart ? "On" : "Off"); break;
	case AP_VIA: sb_str(&b, via_names[c.via < APRS_VIA_COUNT ? c.via : 0]); break;
	case AP_POS: sb_str(&b, pos_names[c.pos_src < APRS_POS_COUNT ? c.pos_src : 0]); break;
	case AP_DMR_GPS:
		if (!CONFIG_DMR_GPS) {
			it->flags |= UI_ITEM_DIM;
		} else {
			sb_str(&b, c.dmr_gps ? "On" : "Off");
		}
		break;
	case AP_KISS: sb_str(&b, c.kiss ? "On" : "Off"); break;
	case AP_HEARD:
		if (!CONFIG_APRS_RX) {
			it->flags |= UI_ITEM_DIM;
		} else {
			sb_u32(&b, v.stations, 1);
		}
		break;
	default: break;
	}
}

static void aprs_select(uint16_t i, bool hold)
{
	(void)hold;
	aprs_view_t v;
	aprs_cfg_t c;
	if (!ui_svc_aprs_view(&v)) {
		ui_toast("APRS: not in this build");
		return;
	}
	ui_svc_aprs_cfg(&c);
	switch (i) {
	case AP_SEND:
		if (ui_svc_aprs_send_now()) {
			ui_toast("Sending position");
		} else {
			ui_toast(v.block == APRS_NO_CALL ? "Set your callsign first" : "Can't send now");
		}
		return;
	case AP_AUTO:
		c.tx_on = !c.tx_on;
		ui_toast(c.tx_on ? "Beacons on" : "Beacons off");
		break;
	case AP_FREQ:
		if (board.afsk == NULL && !CONFIG_APRS_RX) {
			return;
		}
		c.region = (uint8_t)((c.region + 1u) % APRS_REGION_COUNT);
		if (c.region == APRS_REGION_CUSTOM && c.freq_hz == 0) {
			c.region = 0;	// no custom frequency set (qdmr / debug link): skip it
		}
		break;
	case AP_POWER:
		if (!rf_send()) {
			return;
		}
		c.power = (uint8_t)((c.power + 1u) % (board.power_count + 1u));	// as the channel, then each level
		break;
	case AP_PATH:
		if (!rf_send()) {
			return;
		}
		c.path = (uint8_t)((c.path + 1u) % APRS_PATH_COUNT);	// none, then more hops; WIDE2-2 wraps to none
		break;
	case AP_INTERVAL: c.interval_s = intervals[(interval_index(c.interval_s) + 1u) % N_INTERVALS]; break;
	case AP_SMART: c.smart = !c.smart; break;
	case AP_VIA: c.via = (uint8_t)((c.via + 1u) % APRS_VIA_COUNT); break;
	case AP_POS: ui_push(SCR_APRS_POS, 0); return;
	case AP_DMR_GPS:
		if (!CONFIG_DMR_GPS) {
			ui_toast("DMR position: not in this build");
			return;
		}
		c.dmr_gps = !c.dmr_gps;
		ui_toast(c.dmr_gps ? "Position goes out in DMR calls" : "No position in DMR calls");
		break;
	case AP_KISS: c.kiss = !c.kiss; break;
	case AP_HEARD:
		if (!CONFIG_APRS_RX) {
			ui_toast("This radio can't receive APRS");
		} else {
			ui_push(SCR_APRS_HEARD, 0);
		}
		return;
	default: return;
	}
	ui_svc_aprs_set_cfg(&c);
	ui_dirty();
}

// --- Position: the source and the fixed position ---
// Latitude and longitude are typed as decimal degrees to 4 places (about 11 m): the digits fill "DD.DDDD" or
// "DDD.DDDD" from the left (a longitude's first digit of 2-9 gets its leading zero by itself), Up / Down (or the knob)
// switch N / S or E / W, BACK takes the last digit back, OK saves (the digits not typed are zeros). Opening the editor
// shows the stored value; the first digit starts over.

enum { PP_SRC, PP_LAT, PP_LON, PP_COUNT };
#define POS_DEC		4u		// decimal places typed
#define POS_DIGITS_MAX	7u

static bool pos_editing;
static uint8_t pos_n;			// digits typed (0 = showing the stored value)
static char pos_digits[POS_DIGITS_MAX + 1];
static uint32_t pos_stored;		// the stored value, 1e-4 degree
static bool pos_neg;			// south / west

static uint8_t pos_int_digits(uint16_t row) { return row == PP_LAT ? 2u : 3u; }
static uint32_t pos_max(uint16_t row) { return row == PP_LAT ? 900000u : 1800000u; }	// 1e-4 degree

static int32_t pos_get(const aprs_cfg_t *c, uint16_t row) { return row == PP_LAT ? c->fix_lat : c->fix_lon; }

// 1e-7 degree to the 1e-4 degree typed, without the sign
static uint32_t pos_e4(int32_t e7) { return (uint32_t)(((e7 < 0 ? -(int64_t)e7 : e7) + 500) / 1000); }

// The typed digits as 1e-4 degree, the ones not typed yet as zeros
static uint32_t pos_typed(uint16_t row)
{
	uint32_t v = 0;
	uint8_t total = (uint8_t)(pos_int_digits(row) + POS_DEC);
	for (uint8_t k = 0; k < total; k++) {
		v = v * 10u + (k < pos_n ? (uint32_t)(pos_digits[k] - '0') : 0u);
	}
	return v;
}

// "33.7490 N", "84.3880 W"; the editor shows every place ("084.3880 W"), the ones still to type as underscores
// ("33.7___ N")
static void pos_text(sbuf_t *b, uint16_t row, uint32_t v, bool neg, bool editing, bool typing)
{
	uint8_t ints = pos_int_digits(row);
	char d[POS_DIGITS_MAX + 1];
	for (int k = ints + (int)POS_DEC - 1; k >= 0; k--, v /= 10u) {
		d[k] = (char)('0' + v % 10u);
	}
	uint8_t k = 0;
	while (!editing && k + 1u < ints && d[k] == '0') {
		k++;	// no leading zeros outside the editor
	}
	for (; k < ints + POS_DEC; k++) {
		if (k == ints) {
			sb_chr(b, '.');
		}
		sb_chr(b, typing && k >= pos_n ? '_' : d[k]);
	}
	sb_chr(b, ' ');
	sb_chr(b, row == PP_LAT ? (neg ? 'S' : 'N') : (neg ? 'W' : 'E'));
}

static uint16_t pos_count(void) { return PP_COUNT; }

static void pos_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { "Source", "Latitude", "Longitude" };
	aprs_cfg_t c;
	ui_svc_aprs_cfg(&c);
	strcpy(it->label, names[i]);
	it->num = (char)('1' + i);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (i == PP_SRC) {
		sb_str(&b, pos_names[c.pos_src < APRS_POS_COUNT ? c.pos_src : 0]);
		return;
	}
	bool edit = pos_editing && i == ui_frame()->focus;
	if (edit) {
		pos_text(&b, i, pos_n != 0 ? pos_typed(i) : pos_stored, pos_neg, true, pos_n != 0);
		it->flags |= UI_ITEM_EDIT;
		return;
	}
	int32_t e7 = pos_get(&c, i);
	if (c.fix_lat == 0 && c.fix_lon == 0) {
		sb_str(&b, "Not set");
		return;
	}
	pos_text(&b, i, pos_e4(e7), e7 < 0, false, false);
}

static void pos_open(uint16_t row)
{
	aprs_cfg_t c;
	ui_svc_aprs_cfg(&c);
	int32_t e7 = pos_get(&c, row);
	pos_editing = true;
	pos_n = 0;
	pos_neg = e7 < 0;
	pos_stored = pos_e4(e7);
}

static void pos_save(uint16_t row)
{
	aprs_cfg_t c;
	ui_svc_aprs_cfg(&c);
	uint32_t v = pos_n != 0 ? pos_typed(row) : pos_stored;
	int32_t e7 = (int32_t)(v * 1000u) * (pos_neg ? -1 : 1);
	if (row == PP_LAT) {
		c.fix_lat = e7;
	} else {
		c.fix_lon = e7;
	}
	ui_svc_aprs_set_cfg(&c);
	pos_editing = false;
}

static void pos_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == PP_SRC) {
		aprs_cfg_t c;
		ui_svc_aprs_cfg(&c);
		c.pos_src = (uint8_t)((c.pos_src + 1u) % APRS_POS_COUNT);
		ui_svc_aprs_set_cfg(&c);
	} else if (pos_editing) {
		pos_save(i);
	} else {
		pos_open(i);
	}
	ui_dirty();
}

static bool pos_key(const input_event_t *ev)
{
	uint16_t row = ui_frame()->focus;
	if (!pos_editing || row == PP_SRC) {
		pos_editing = false;
		return false;
	}
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	bool flip = ((ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN) && press) ||
		    (ev->action == ACT_ENC_STEP && ev->gesture != GESTURE_UP);
	if (flip) {
		pos_neg = !pos_neg;
		ui_dirty();
		return true;
	}
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		uint8_t total = (uint8_t)(pos_int_digits(row) + POS_DEC);
		char d = (char)('0' + (ev->action - ACT_DIGIT_0));
		if (row == PP_LON && pos_n == 0 && d >= '2') {
			pos_digits[pos_n++] = '0';	// 2-9 can't lead a longitude (180 at most): 84.3880 is typed 8 4 3 8 8 0
		}
		if (pos_n < total) {
			pos_digits[pos_n++] = d;
			if (pos_typed(row) > pos_max(row)) {
				pos_n--;	// past 90 / 180 degrees: refused as typed
				ui_toast("Up to 90 N/S, 180 E/W");
			}
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		if (pos_n != 0) {
			pos_n--;	// the last digit back
		} else {
			pos_editing = false;	// nothing typed: leave without a change
		}
		ui_dirty();
		return true;
	}
	return false;	// OK: pos_select saves
}

static void pos_enter(void) { pos_editing = false; }

const ui_scr_def_t ui_aprs_pos_screen = {
	.name = "aprs-pos", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = "Position", .hint = "OK change  Back",
	.count = pos_count, .item = pos_item, .select = pos_select, .key = pos_key, .enter = pos_enter,
};

const ui_scr_def_t ui_aprs_screen = {
	.name = "aprs", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = "APRS", .hint = "0-9 pick  Back",
	.count = aprs_count, .item = aprs_item, .select = aprs_select, .body = aprs_body,
};

// --- Stations heard (receive builds) ---

static uint16_t heard_count(void) { return ui_svc_aprs_station_count(); }

static void heard_item(uint16_t i, ui_item_t *it)
{
	aprs_station_t s;
	if (!ui_svc_aprs_station((uint8_t)i, &s)) {
		return;
	}
	strcpy(it->label, s.call);
	ui_sb_age(it->value, sizeof it->value, ui_st.now_ms - s.heard_ms);
	if (!s.direct) {
		it->flags |= UI_ITEM_TEMP;	// heard through a digipeater
	}
}

const ui_scr_def_t ui_aprs_heard_screen = {
	.name = "aprs-heard", .flags = SF_LIST | SF_FIELD, .prefix = "Heard", .hint = "Back", .empty = "Nothing heard yet",
	.count = heard_count, .item = heard_item,
};
