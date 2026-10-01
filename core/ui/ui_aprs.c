// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Tools > APRS (docs/spec/aprs.md 10): what APRS is doing (callsign, frequency, position, next beacon, or why nothing
// can be sent) and the everyday choices, one row each: send now, automatic beacons, the frequency (region), the
// interval, SmartBeaconing, the way out (radio / DMR / both), the position source, our position in DMR voice calls
// (ETSI GPS Info, CONFIG_DMR_GPS), KISS on the host port, and the stations heard (receive builds). The rest (symbol, path, comment, digital destination, iGate, digipeater) is set
// with qdmr, the debug link or tools/odr_debug.py (aprs.md 11).
#include <string.h>
#include "ui_nav.h"
#include "ui_svc_aprs.h"
#include "fmt.h"
#include "config.h"

// The services when APRS isn't built (and in the UI simulator): nothing there
#define WEAK __attribute__((weak))
WEAK bool ui_svc_aprs_view(aprs_view_t *v) { memset(v, 0, sizeof(*v)); return false; }
WEAK void ui_svc_aprs_cfg(aprs_cfg_t *c) { memset(c, 0, sizeof(*c)); }
WEAK void ui_svc_aprs_set_cfg(const aprs_cfg_t *c) { (void)c; }
WEAK bool ui_svc_aprs_send_now(void) { return false; }
WEAK uint8_t ui_svc_aprs_station_count(void) { return 0; }
WEAK bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out) { (void)i; (void)out; return false; }

enum { AP_SEND, AP_AUTO, AP_FREQ, AP_INTERVAL, AP_SMART, AP_VIA, AP_POS, AP_DMR_GPS, AP_KISS, AP_HEARD, AP_COUNT };

static const uint16_t intervals[] = { 0, 60, 120, 300, 600, 1800 };
static const char *const interval_names[] = { "Off", "1 min", "2 min", "5 min", "10 min", "30 min" };
#define N_INTERVALS	(sizeof(intervals) / sizeof(intervals[0]))
static const char *const via_names[] = { "Radio", "DMR", "Both" };
static const char *const pos_names[] = { "Auto", "GPS", "Computer", "Fixed" };
static const char *const region_names[APRS_REGION_COUNT] = { "N America", "Europe", "Australia", "New Zealand",
							      "Japan", "China", "Custom" };

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
	static const char *const names[] = { "Send position now", "Automatic beacons", "Frequency", "Interval",
					     "SmartBeaconing", "Send via", "Position", "Position in DMR calls",
					     "KISS on USB", "Stations heard" };
	aprs_view_t v;
	aprs_cfg_t c;
	bool built = ui_svc_aprs_view(&v);
	ui_svc_aprs_cfg(&c);
	strcpy(it->label, names[i]);
	it->num = (char)('0' + (i + 1u) % 10u);
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
		if (c.region == APRS_REGION_CUSTOM) {
			sb_freq(&b, c.freq_hz);
		} else {
			sb_str(&b, region_names[c.region < APRS_REGION_COUNT ? c.region : 0]);
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
		c.region = (uint8_t)((c.region + 1u) % APRS_REGION_COUNT);
		if (c.region == APRS_REGION_CUSTOM && c.freq_hz == 0) {
			c.region = 0;	// no custom frequency set (qdmr / debug link): skip it
		}
		break;
	case AP_INTERVAL: c.interval_s = intervals[(interval_index(c.interval_s) + 1u) % N_INTERVALS]; break;
	case AP_SMART: c.smart = !c.smart; break;
	case AP_VIA: c.via = (uint8_t)((c.via + 1u) % APRS_VIA_COUNT); break;
	case AP_POS: c.pos_src = (uint8_t)((c.pos_src + 1u) % APRS_POS_COUNT); break;
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
