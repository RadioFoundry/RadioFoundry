// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tools > APRS (docs/spec/aprs.md 10): what APRS is doing (callsign, frequency, position, next beacon, or why nothing
// can be sent) and the everyday choices, one row each: send now, automatic beacons, the frequency (region), the RF
// power (as the channel, or a level of its own), the digipeater path (the hops), the interval, SmartBeaconing, the
// way out (radio / DMR / both), KISS on the host port, and the stations heard (receive builds). Where the position
// comes from is the position service's (Settings > Location, ui_position.c). The rest (symbol, comment, digital
// destination, iGate, digipeater) is set with qdmr, the debug link or tools/rfoundry_debug.py (aprs.md 11).
#include <string.h>
#include "ui_nav.h"
#include "ui_svc_aprs.h"
#include "fmt.h"
#include "i18n.h"
#include "config.h"
#include "board.h"

// The services when APRS isn't built (and in the UI simulator): nothing there
#define WEAK __attribute__((weak))
WEAK bool ui_svc_aprs_view(aprs_view_t *v) { memset(v, 0, sizeof(*v)); return false; }
WEAK void ui_svc_aprs_cfg(aprs_cfg_t *c) { memset(c, 0, sizeof(*c)); }
WEAK void ui_svc_aprs_set_cfg(const aprs_cfg_t *c) { (void)c; }
WEAK bool ui_svc_aprs_send_now(void) { return false; }
WEAK bool ui_svc_aprs_send_message(const char *to, const char *text, uint8_t flags)
{
	(void)to;
	(void)text;
	(void)flags;
	return false;
}
WEAK uint8_t ui_svc_aprs_msg_count(void) { return 0; }
WEAK bool ui_svc_aprs_msg(uint8_t i, aprs_msg_t *out, bool read)
{
	(void)i; (void)out; (void)read; return false;
}
WEAK bool ui_svc_aprs_msg_delete(uint8_t i) { (void)i; return false; }
WEAK uint8_t ui_svc_aprs_station_count(void) { return 0; }
WEAK bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out) { (void)i; (void)out; return false; }
WEAK bool ui_svc_aprs_station_find(const char *call, aprs_station_t *out) { (void)call; (void)out; return false; }
WEAK bool ui_svc_aprs_range(const aprs_station_t *s, uint32_t *m, uint16_t *deg) { (void)s; (void)m; (void)deg; return false; }

enum { AP_SEND, AP_AUTO, AP_FREQ, AP_POWER, AP_PATH, AP_INTERVAL, AP_SMART, AP_VIA, AP_KISS, AP_HEARD, AP_COUNT };

// The rows this build shows, in order: the packet rows only with the AX.25 backend (CONFIG_APRS_AFSK), "Send via"
// only when there is a choice (both backends built)
static const uint8_t rows[] = {
	AP_SEND, AP_AUTO,
#if CONFIG_APRS_AFSK
	AP_FREQ, AP_POWER, AP_PATH,
#endif
	AP_INTERVAL, AP_SMART,
#if CONFIG_APRS_AFSK && CONFIG_APRS_DMR
	AP_VIA,
#endif
	AP_KISS,
#if CONFIG_APRS_AFSK
	AP_HEARD,
#endif
};
#define N_ROWS	((uint16_t)(sizeof(rows) / sizeof(rows[0])))

// Beacons go out on RF only: no DMR backend, or the setting says so
static bool rf_only(const aprs_cfg_t *c)
{
	return !CONFIG_APRS_DMR || c->via == APRS_VIA_RF;
}

static const uint16_t intervals[] = { 0, 60, 120, 300, 600, 1800 };
static const char *const interval_names[] = { TRN("Off"), TRN("1 min"), TRN("2 min"), TRN("5 min"),
						       TRN("10 min"), TRN("30 min") };
#define N_INTERVALS	(sizeof(intervals) / sizeof(intervals[0]))
// aprs_path_t order; the text is aprs_path_text()'s, kept here so the page builds without APRS (the simulator)
static const char *const path_names[APRS_PATH_COUNT] = { TRN("None"), "WIDE1-1", "WIDE1-1,WIDE2-1", "WIDE2-1",
							   "WIDE2-2" };
static const char *const via_names[] = { "AFSK", "DMR", TRN("Both") };
static const char *const pos_names[] = { TRN("Auto"), "GPS", TRN("Computer"), TRN("Fixed") };
static const char *const region_names[APRS_REGION_COUNT] = { TRN("N America"), TRN("Europe"), TRN("Australia"),
							      TRN("New Zealand"), TRN("Japan"), TRN("China"),
							      TRN("Custom") };

// The radio can send APRS on RF (a board with an AFSK transmit path; some only receive)
static bool rf_send(void)
{
	return board.afsk != NULL && board.afsk->tx_start != NULL;
}

static const char *why_text(uint8_t block)
{
	switch (block) {
	case APRS_NO_CALL: return TR("Set your callsign (the radio name) in qdmr.");
	case APRS_NO_FREQ: return TR("No APRS frequency: pick a region.");
	case APRS_NOT_AMATEUR: return TR("Pick a frequency in a ham band this radio sends on.");
	case APRS_NO_AFSK: return TR("No APRS tones on this radio; DMR positions work.");
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
		sb_str(&b, TR("APRS: not in this build"));
		return t;
	}
	if (CONFIG_APRS_AFSK) {		// who and where on RF: the AX.25 backend's
		sb_str(&b, v.call[0] != '\0' ? v.call : TR("No callsign"));
		sb_str(&b, TR(" on "));
		sb_freq(&b, v.freq_hz);
		if (v.rf_active) {
			sb_str(&b, TR(" (sending)"));
		}
		const char *why = why_text(v.block);
		if (why != NULL) {
			sb_chr(&b, '\n');
			sb_str(&b, why);
		}
		sb_chr(&b, '\n');
	}
	if (v.pos_valid) {
		sb_str(&b, tr(pos_names[v.pos_src < 4 ? v.pos_src : 0]));
		sb_chr(&b, ' ');
		sb_degrees(&b, v.lat);
		sb_chr(&b, ' ');
		sb_degrees(&b, v.lon);
	} else {
		sb_str(&b, TR("No position"));
	}
	if (v.next_s != 0xFFFFu) {
		sb_chr(&b, '\n');
		sb_str(&b, TR("Next in "));
		sb_mmss(&b, v.next_s);
	}
	if (v.rf_sent + v.dmr_sent != 0) {
		sb_str(&b, TR("  Sent "));
		sb_u32(&b, v.rf_sent + v.dmr_sent, 1);
	}
	return t;
}

static uint16_t aprs_count(void) { return N_ROWS; }

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
	static const char *const names[] = { TRN("Send position now"), TRN("Automatic beacons"), TRN("Frequency"),
					     TRN("Power"), TRN("Path"), TRN("Interval"), TRN("SmartBeaconing"),
					     TRN("Send via"), TRN("KISS on USB"), TRN("Stations heard") };
	aprs_view_t v;
	aprs_cfg_t c;
	bool built = ui_svc_aprs_view(&v);
	ui_svc_aprs_cfg(&c);
	it->num = i < 10u ? (char)('0' + (i + 1u) % 10u) : 0;	// rows after the 10th have no digit
	i = i < N_ROWS ? rows[i] : AP_COUNT;
	if (i >= AP_COUNT) {
		return;
	}
	// Without the AX.25 backend the host port is no TNC: it only takes NMEA positions
	str_copy(it->label, sizeof it->label, i == AP_KISS && !CONFIG_APRS_AFSK ? TR("NMEA on USB") : tr(names[i]));
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (!built) {
		it->flags |= UI_ITEM_DIM;
		return;
	}
	switch (i) {
	case AP_SEND:
		if (v.block != APRS_OK && rf_only(&c)) {
			it->flags |= UI_ITEM_DIM;
		}
		break;
	case AP_AUTO: sb_str(&b, c.tx_on ? TR("On") : TR("Off")); break;
	case AP_FREQ:
		if (board.afsk == NULL && !CONFIG_APRS_RX) {
			it->flags |= UI_ITEM_DIM;	// no APRS on RF here
		} else if (c.region == APRS_REGION_CUSTOM) {
			sb_freq(&b, c.freq_hz);
		} else {
			sb_str(&b, tr(region_names[c.region < APRS_REGION_COUNT ? c.region : 0]));
		}
		break;
	case AP_POWER:
		if (!rf_send()) {
			it->flags |= UI_ITEM_DIM;	// no APRS on RF here (no transmit tone path)
		} else {
			sb_str(&b, c.power == 0 ? TR("As channel") : ui_power_name((uint8_t)(c.power - 1u), v.freq_hz));
		}
		break;
	case AP_PATH:
		if (!rf_send()) {
			it->flags |= UI_ITEM_DIM;	// the path is for RF packets only
		} else {
			sb_str(&b, tr(path_names[c.path < APRS_PATH_COUNT ? c.path : 0]));
		}
		break;
	case AP_INTERVAL: sb_str(&b, tr(interval_names[interval_index(c.interval_s)])); break;
	case AP_SMART: sb_str(&b, c.smart ? TR("On") : TR("Off")); break;
	case AP_VIA: sb_str(&b, tr(via_names[c.via < APRS_VIA_COUNT ? c.via : 0])); break;
	case AP_KISS: sb_str(&b, c.kiss ? TR("On") : TR("Off")); break;
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
		ui_toast(TR("APRS: not in this build"));
		return;
	}
	ui_svc_aprs_cfg(&c);
	switch (i < N_ROWS ? rows[i] : AP_COUNT) {
	case AP_SEND:
		if (ui_svc_aprs_send_now()) {
			ui_toast(TR("Sending position"));
		} else {
			ui_toast(v.block == APRS_NO_CALL ? TR("Set your callsign first") : TR("Can't send now"));
		}
		return;
	case AP_AUTO:
		c.tx_on = !c.tx_on;
		ui_toast(c.tx_on ? TR("Beacons on") : TR("Beacons off"));
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
	case AP_KISS: c.kiss = !c.kiss; break;
	case AP_HEARD:
		if (!CONFIG_APRS_RX) {
			ui_toast(TR("This radio can't receive APRS"));
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
	.name = "aprs", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = "APRS", .hint = TRN("0-9 pick  Back"),
	.count = aprs_count, .item = aprs_item, .select = aprs_select, .body = aprs_body,
};

// --- Stations heard (receive builds) ---

#if CONFIG_APRS_RX
static char station_call[AX25_CALL_LEN + 4];
#endif

static uint16_t heard_count(void) { return ui_svc_aprs_station_count(); }

static void heard_select(uint16_t i, bool hold)
{
	(void)hold;
#if CONFIG_APRS_RX
	aprs_station_t s;
	if (ui_svc_aprs_station((uint8_t)i, &s)) {
		str_copy(station_call, sizeof station_call, s.call);
		ui_push(SCR_APRS_STATION, 0);
	}
#else
	ui_push(SCR_APRS_STATION, i);
#endif
}

static void heard_item(uint16_t i, ui_item_t *it)
{
	aprs_station_t s;
	if (!ui_svc_aprs_station((uint8_t)i, &s)) {
		return;
	}
	sbuf_t b;
	uint32_t m, at = s.heard_ms;
	uint16_t deg;
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, s.call);
	if (ui_svc_aprs_range(&s, &m, &deg)) {
		// How far and which way ("12.3 km NE"): an alternate, so a narrow row drops it before the call (font.h)
		sb_str(&b, TEXT_ALT_OPEN " ");
		sb_range(&b, m, deg);
		sb_str(&b, TEXT_ALT_NEXT TEXT_ALT_CLOSE);
		at = s.pos_ms;	// the age of the position the range is from, not of the station's last packet
	}
	ui_sb_age(it->value, sizeof it->value, ui_st.now_ms - at);
	if (!s.direct) {
		it->flags |= UI_ITEM_TEMP;	// heard through a digipeater
	}
}

const ui_scr_def_t ui_aprs_heard_screen = {
	.name = "aprs-heard", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Heard"), .hint = TRN("Back"),
	.empty = TRN("Nothing heard yet"),
	.count = heard_count, .item = heard_item, .select = heard_select,
};

#if CONFIG_APRS_RX
enum { ST_MESSAGE, ST_RANGE, ST_HEARD, ST_PATH, ST_POS_AGE, ST_SPEED, ST_COURSE, ST_SYMBOL, ST_TEXT, ST_PACKETS, ST_COUNT };

// Resolve the rows to show from the current record in one pass (the range once); no copy of the engine's station
// table lives in the UI.
static uint8_t station_rows(const aprs_station_t *s, uint8_t shown[ST_COUNT], uint32_t *m, uint16_t *deg)
{
	bool range = ui_svc_aprs_range(s, m, deg);
	uint8_t n = 0;
	for (uint8_t row = 0; row < ST_COUNT; row++) {
		if ((row == ST_RANGE && !range) ||
		    (row == ST_POS_AGE && (!s->has_pos || s->pos_ms == s->heard_ms)) ||
		    (row == ST_SPEED && (!s->has_pos || s->speed_kn == 0xFFFFu)) ||
		    (row == ST_COURSE && (!s->has_pos || s->course == 0xFFFFu)) ||
		    (row == ST_SYMBOL && (!s->sym_table || !s->sym_code)) ||
		    (row == ST_TEXT && !s->text[0])) {
			continue;
		}
		shown[n++] = row;
	}
	return n;
}

static uint16_t station_count(void)
{
	aprs_station_t s;
	if (!ui_svc_aprs_station_find(station_call, &s)) {
		return 2;
	}
	uint8_t shown[ST_COUNT];
	uint32_t m;
	uint16_t deg;
	return station_rows(&s, shown, &m, &deg);
}

static void station_item(uint16_t i, ui_item_t *it)
{
	aprs_station_t s;
	bool found = ui_svc_aprs_station_find(station_call, &s);
	if (i == 0) {
		str_copy(it->label, sizeof it->label, TR("Message"));
		return;
	}
	if (!found) {
		str_copy(it->label, sizeof it->label, TR("No longer in list"));
		return;
	}
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	const char *label = "";
	uint8_t shown[ST_COUNT];
	uint32_t m;
	uint16_t deg;
	uint8_t n = station_rows(&s, shown, &m, &deg);
	switch (i < n ? shown[i] : ST_COUNT) {
	case ST_RANGE:
		label = TR(TEXT_ALT_OPEN "Distance" TEXT_ALT_NEXT "Dist" TEXT_ALT_NEXT "" TEXT_ALT_CLOSE);
		sb_range(&b, m, deg);
		break;
	case ST_HEARD:
		label = TR("Heard");
		ui_sb_age(it->value, sizeof it->value, ui_st.now_ms - s.heard_ms);
		break;
	case ST_PATH: label = s.direct ? TR("Direct") : TR("Via digipeater"); break;
	case ST_POS_AGE:
		label = TR(TEXT_ALT_OPEN "Position age" TEXT_ALT_NEXT "Pos age" TEXT_ALT_CLOSE);
		ui_sb_age(it->value, sizeof it->value, ui_st.now_ms - s.pos_ms);
		break;
	case ST_SPEED:
		label = TR("Speed");
		sb_u32(&b, ((uint32_t)s.speed_kn * 1852u + 500u) / 1000u, 1);
		sb_str(&b, " km/h");
		break;
	case ST_COURSE:
		label = TR("Course");
		sb_u32(&b, s.course, 1);
		sb_str(&b, GLYPH_DEGREE " ");
		sb_compass(&b, s.course);
		break;
	case ST_SYMBOL:
		label = TR("Symbol");
		sb_chr(&b, s.sym_table);
		sb_chr(&b, s.sym_code);
		break;
	case ST_TEXT:
		label = s.text;
		it->flags |= UI_ITEM_PREVIEW;
		break;
	case ST_PACKETS:
		label = TR("Packets");
		sb_u32(&b, s.count, 1);
		break;
	default: break;
	}
	str_copy(it->label, sizeof it->label, label);
}

static const char *station_title(void) { return station_call; }
static void station_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == 0) {
		aprs_station_t s;
		bool found = ui_svc_aprs_station_find(station_call, &s);
		ui_aprs_compose(found ? s.call : station_call);
	}
}
#else
static uint16_t station_count(void) { return 1; }
static void station_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	str_copy(it->label, sizeof it->label, TR("Message"));
}
static const char *station_title(void)
{
	static char call[AX25_CALL_LEN + 4];
	aprs_station_t s;
	if (!ui_svc_aprs_station((uint8_t)ui_frame()->arg, &s))
		return "";
	str_copy(call, sizeof call, s.call);
	return call;
}
static void station_select(uint16_t i, bool hold)
{
	(void)i; (void)hold;
	aprs_station_t s;
	if (ui_svc_aprs_station((uint8_t)ui_frame()->arg, &s))
		ui_aprs_compose(s.call);
}
#endif
const ui_scr_def_t ui_aprs_station_screen = {
	.name = "aprs-station", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Heard"), .hint = TRN("OK choose  Back"),
	.count = station_count, .item = station_item, .select = station_select, .title = station_title,
};
