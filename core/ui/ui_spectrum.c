// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Sandro Kalatozishvili
#include "config.h"
#if CONFIG_SPECTRUM
#include <string.h>
#include "board.h"
#include "ui_nav.h"
#include "radio/spectrum.h"
#include "radio/radio_bands.h"

enum { FIELD_CENTER, FIELD_SQUELCH, FIELD_STEP, FIELD_BANDWIDTH, FIELD_DWELL, FIELD_AUTO, FIELD_FILTER, FIELD_COUNT };
static uint8_t selected, length;
static bool editing, stepping;
static int32_t stepped_value;
static char digits[UI_ENTRY_MAX + 1];

static void frequency_text(sbuf_t *b, uint32_t hz)
{
	sb_u32(b, hz / 1000000u, 1);
	sb_chr(b, '.');
	sb_u32(b, (hz % 1000000u) / 10u, 5);
}

static int level_y(int16_t dbm, int top, int bottom)
{
	int floor = -130 + ui_view()->spectrum.scale_db;
	if (dbm < floor) dbm = (int16_t)floor;
	if (dbm > floor + 100) dbm = (int16_t)(floor + 100);
	return bottom - (dbm - floor) * (bottom - top) / 100;
}

static void center_text(const font_t *f, int y, const char *text)
{
	if (editing)
		gfx_fill(0, y - 1, gfx_text_width(f, text) + 2, f->h + 2, ui_col.focus);
	gfx_text(f, 1, y, text, editing ? ui_col.focus_fg : ui_col.fg);
}

static void value_text(sbuf_t *b, unsigned field)
{
	const spectrum_t *s = &ui_view()->spectrum;
	bool spin = editing && stepping && selected == field;
	if (editing && !stepping && selected == field) {
		if (field == FIELD_SQUELCH && (length == 0 || strspn(digits, "0") != length))
			sb_chr(b, '-');
		for (unsigned i = 0; i < length; i++) {
			if (field == FIELD_CENTER && i == UI_ENTRY_MHZ) sb_chr(b, '.');
			sb_chr(b, digits[i]);
		}
		sb_chr(b, '_');
	} else if (field == FIELD_CENTER) {
		frequency_text(b, spin ? (uint32_t)stepped_value : s->center_hz);
	} else if (field == FIELD_STEP) {
		uint32_t hz = spin ? (uint32_t)stepped_value : s->step_hz;
		sb_u32(b, hz / 1000, 1);
		sb_chr(b, '.');
		sb_u32(b, (hz % 1000) / 10, 2);
	} else if (field == FIELD_BANDWIDTH) {
		uint32_t span = s->step_hz * (s->bins - 1u);
		sb_u32(b, span / 1000000u, 1);
		sb_chr(b, '.');
		uint32_t frac = span % 1000000u, div = 100000u;
		do {
			sb_chr(b, (char)('0' + frac / div));
			frac %= div;
			div /= 10;
		} while (frac && div >= 10);
	} else if (field == FIELD_DWELL) {
		sb_u32(b, spin ? (uint32_t)stepped_value : s->dwell_ms, 1);
	} else if (field == FIELD_AUTO) {
		sb_str(b, s->auto_peak == SPECTRUM_AUTO_IMMEDIATE ? TR(SPECTRUM_VALUE_IMMEDIATE, "Immediate") :
			s->auto_peak == SPECTRUM_AUTO_SWEEP ? TR(SPECTRUM_VALUE_AFTER_SWEEP, "After sweep") : TR(VALUE_OFF, "Off"));
	} else if (field == FIELD_FILTER) {
		sb_str(b, s->narrow ? TR(SPECTRUM_VALUE_NARROW, "Narrow") : TR(SPECTRUM_VALUE_WIDE, "Wide"));
	} else {
		sb_i32(b, spin ? stepped_value : s->squelch_dbm);
	}
}

static void spectrum_draw(void)
{
	const spectrum_t *s = &ui_view()->spectrum;
	const font_t *f = ui_layout.tiny;
	bool compact = gfx_height() < 100;
	gfx_clear(ui_col.bg);
	const char *title = gfx_height() >= UI_TALL_PX ? TR(SPECTRUM_TITLE_ANALYZER, "Spectrum Analyzer") : NULL;
	int y = ui_draw_top_font(ui_view(), &ui_st, title, NULL, f) + (compact ? 1 : 2);
	if (!s->active) {
		char fit[64];
		gfx_text_fit(f, TR(SPECTRUM_WHY_UNAVAILABLE, "Unavailable on this channel or radio busy"), gfx_width(), fit, sizeof fit);
		gfx_text(f, 0, y, fit, ui_col.warn);
		ui_draw_bottom_font(&ui_st, TR(HINT_BACK, "Back"), f);
		return;
	}
	char text[64], tuner[32];
	sbuf_t b;
	sb_init(&b, text, sizeof text);
	value_text(&b, FIELD_CENTER);
	sb_init(&b, tuner, sizeof tuner);
	frequency_text(&b, spectrum_frequency(s, s->tuner));
	int number_width = gfx_text_width(f, text) + gfx_text_width(f, tuner) + 5;
	int unit_width = gfx_text_width(f, " MHz");
	if (number_width + 2 * unit_width <= gfx_width())
		sb_str(&b, " MHz");
	if (number_width + unit_width <= gfx_width()) {
		sb_init(&b, text, sizeof text);
		value_text(&b, FIELD_CENTER);
		sb_str(&b, " MHz");
	}
	center_text(f, y, text);
	gfx_text(f, gfx_width() - gfx_text_width(f, tuner), y, tuner, ui_col.dim);
	y += f->h + (compact ? 1 : 3);

	unsigned peak = spectrum_strongest(s);
	if (spectrum_peak_level(s, peak) != SPECTRUM_EMPTY) {
		sb_init(&b, text, sizeof text);
		sb_str(&b, TR(SPECTRUM_LABEL_PEAK, "Peak "));
		frequency_text(&b, spectrum_frequency(s, peak));
		sb_chr(&b, ' ');
		sb_i32(&b, spectrum_level_dbm(spectrum_peak_level(s, peak)));
		if (gfx_text_width(f, text) + gfx_text_width(f, " dBm") <= gfx_width()) sb_str(&b, " dBm");
		gfx_text(f, 0, y, text, ui_col.dim);
		y += f->h + (compact ? 1 : 2);
	}
	int bottom = (compact ? gfx_height() : ui_layout.hint_y) - f->h - 5;
	int left = 0;
	int right = gfx_width() - 1;
	if (bottom > y + 4) {
		for (int dbm = -120 + s->scale_db; dbm <= -40 + s->scale_db; dbm += 40) {
			int gy = level_y((int16_t)dbm, y, bottom);
			gfx_hline(left, gy, right - left + 1, ui_col.line);
		}
		for (unsigned i = 0; i < s->bins; i++) {
			int x = left + (int)i * (right - left) / (s->bins - 1);
			if (s->level[i] != SPECTRUM_EMPTY) {
				int ly = level_y(spectrum_level_dbm(s->level[i]), y, bottom);
				gfx_vline(x, ly, bottom - ly + 1, ui_col.rx);
			}
			uint8_t level = spectrum_peak_level(s, i);
			if (level != SPECTRUM_EMPTY)
				gfx_pixel(x, level_y(spectrum_level_dbm(level), y, bottom), ui_col.warn);
		}
		// Labels sit inside the plot instead of reserving a frequency column along its left edge.
		for (int dbm = -120 + s->scale_db; dbm <= -40 + s->scale_db; dbm += 40) {
			int height = bottom - y + 1;
			if (dbm == -80 + s->scale_db ? height < f->h : height < 3 * f->h) continue;
			sb_init(&b, text, sizeof text);
			sb_i32(&b, dbm);
			int label_y = level_y((int16_t)dbm, y, bottom) - f->h / 2;
			if (label_y < y) label_y = y;
			if (label_y > bottom - f->h + 1) label_y = bottom - f->h + 1;
			gfx_fill(0, label_y, gfx_text_width(f, text) + 4, f->h, ui_col.bg);
			gfx_text(f, 2, label_y, text, ui_col.dim);
		}
		int x = left + s->tuner * (right - left) / (s->bins - 1);
		for (int d = 0; d < 3; d++)
			gfx_hline(x - d, bottom + d, 2 * d + 1, ui_col.fg);
	}
	sb_init(&b, text, sizeof text);
	frequency_text(&b, spectrum_frequency(s, 0));
	gfx_text(f, left, bottom + 4, text, ui_col.dim);
	sb_init(&b, text, sizeof text);
	frequency_text(&b, spectrum_frequency(s, s->bins - 1));
	gfx_text(f, right - gfx_text_width(f, text), bottom + 4, text, ui_col.dim);
	if (!compact)
			ui_draw_bottom_font(&ui_st, editing ? TR(SPECTRUM_HINT_ENTRY, "Digits  # .  OK save  Back") :
				(board.caps & CAP_KEYPAD) ? TR(SPECTRUM_HINT_MAIN, "OK settings  Type freq  Back") :
				TR(SPECTRUM_HINT_NO_KEYPAD, "OK settings  Back"), f);
	ui_draw_banner(&ui_st);
}

static bool save_entry(void)
{
	uint32_t value = 0;
	if (stepping) {
		value = (uint32_t)(selected == FIELD_SQUELCH ? -stepped_value : stepped_value);
	} else if (selected == FIELD_CENTER) {
		value = ui_freq_entry_hz(digits, length);
		if (!value) return false;
		if (radio_listen_band(value) && !radio_am_tx_refused(value)) return false;
	} else {
		unsigned decimals = 0;
		bool point = false;
		for (unsigned i = 0; i < length; i++) {
			if (digits[i] == '.') {
				point = true;
				continue;
			}
			value = value * 10u + (unsigned)(digits[i] - '0');
			if (point) decimals++;
		}
		if (!length || (selected == FIELD_STEP && (decimals > 2 || value == 0))) return false;
		if (selected == FIELD_STEP) {
			uint64_t hz = value;
			for (; decimals < 3; decimals++) hz *= 10;
			if (hz < 1000 || hz > 100000) return false;
			value = (uint32_t)hz;
			const spectrum_t *s = &ui_view()->spectrum;
			if ((uint64_t)value * (s->bins - 1) > (uint64_t)s->hi_hz - s->lo_hz) return false;
		} else if (selected == FIELD_DWELL) {
			if (value < SPECTRUM_SETTLE_MS || value > SPECTRUM_DWELL_MAX_MS) return false;
		} else if (value > 160) return false;
	}
	int32_t request = selected == FIELD_SQUELCH ? -(int32_t)value : (int32_t)value;
	ui_svc_spectrum(selected == FIELD_CENTER ? SPECTRUM_CENTER :
		selected == FIELD_STEP ? SPECTRUM_STEP : selected == FIELD_DWELL ? SPECTRUM_DWELL : SPECTRUM_SQUELCH,
		request);
	return true;
}

static void begin_entry(unsigned field)
{
	selected = (uint8_t)field;
	editing = true;
	stepping = !(board.caps & CAP_KEYPAD);
	length = 0;
	digits[0] = 0;
	const spectrum_t *s = &ui_view()->spectrum;
	stepped_value = field == FIELD_CENTER ? (int32_t)s->center_hz : field == FIELD_STEP ? (int32_t)s->step_hz :
		field == FIELD_DWELL ? s->dwell_ms : s->squelch_dbm;
}

static int32_t step_center(int32_t value, int steps)
{
	const spectrum_t *s = &ui_view()->spectrum;
	uint64_t span = (uint64_t)s->step_hz * (s->bins - 1u);
	int64_t next = (int64_t)value + (int64_t)steps * s->step_hz;
	int64_t best = steps > 0 ? INT64_MAX : INT64_MIN;
	// Cross gaps through the board's active bands; leave the separate broadcast receiver out.
	for (uint8_t i = 0; i < board_band_count(); i++) {
		const band_t *band = &board.bands[i];
		if (span > (uint64_t)band->hi_hz - band->lo_hz ||
		    (radio_listen_band(band->lo_hz) && !radio_am_tx_refused(band->lo_hz)))
			continue;
		int64_t lo = band->lo_hz + span / 2, hi = band->hi_hz - span / 2;
		int64_t candidate = steps > 0 && next < lo ? lo : steps < 0 && next > hi ? hi : next;
		if (candidate < lo || candidate > hi)
			continue;
		if ((steps > 0 && candidate < best) || (steps < 0 && candidate > best))
			best = candidate;
	}
	return best == INT64_MAX || best == INT64_MIN ? value : (int32_t)best;
}

static bool entry_key(const input_event_t *ev)
{
	int steps = ev->action == ACT_NAV_UP ? 1 : ev->action == ACT_NAV_DOWN ? -1 :
		ev->action == ACT_ENC_STEP ? ev->value : 0;
	if (stepping && steps != 0 && ev->gesture != GESTURE_UP) {
		if (selected == FIELD_CENTER) {
			stepped_value = step_center(stepped_value, steps);
		} else {
			int32_t lo = selected == FIELD_SQUELCH ? -160 : selected == FIELD_STEP ? 1000 : (int32_t)SPECTRUM_SETTLE_MS;
			int32_t hi = selected == FIELD_SQUELCH ? 0 : selected == FIELD_STEP ? 100000 : (int32_t)SPECTRUM_DWELL_MAX_MS;
			if (selected == FIELD_STEP) {
				const spectrum_t *s = &ui_view()->spectrum;
				uint32_t fit = (s->hi_hz - s->lo_hz) / (s->bins - 1u) / 10u * 10u;
				if (fit < (uint32_t)hi) hi = (int32_t)fit;
			}
			int32_t value = stepped_value + steps * (selected == FIELD_STEP ? 10 : 1);
			stepped_value = value < lo ? lo : value > hi ? hi : value;
		}
	} else if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9) {
		// An optional mic keypad may send digits even when the panel has no CAP_KEYPAD.
		stepping = false;
		char d = (char)('0' + ev->action - ACT_DIGIT_0);
		if (selected == FIELD_CENTER)
			ui_freq_entry_digit(digits, &length, d);
		else if (length < UI_ENTRY_MAX) {
			digits[length++] = d;
			digits[length] = 0;
		}
	} else if (ev->action == ACT_CONTEXT) {
		if (selected == FIELD_STEP && length && length < UI_ENTRY_MAX && strchr(digits, '.') == NULL) {
			digits[length++] = '.';
			digits[length] = 0;
		}
	} else if (ev->action == ACT_BACK) {
		if (stepping) editing = false;
		else if (length) digits[--length] = 0;
		else editing = false;
	} else if (ev->action == ACT_SELECT) {
		if (save_entry()) editing = false;
		else ui_toast(TR(SPECTRUM_WHY_VALUE, "Value out of range"));
	}
	ui_dirty();
	return true;
}

static bool spectrum_key(const input_event_t *ev)
{
	if (!ui_view()->spectrum.active)
		return false;
	bool left = ev->action == ACT_NAV_LEFT || ev->action == ACT_USER_1;
	bool right = ev->action == ACT_NAV_RIGHT || ev->action == ACT_USER_4;
	if ((left || right) && (ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG || ev->gesture == GESTURE_REPEAT)) {
		ui_svc_spectrum(SPECTRUM_TUNER, left ? -1 : 1);
		return true;
	}
	if (ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_REPEAT && ev->action != ACT_ENC_STEP)
		return false;
	if (ev->action == ACT_ENC_STEP) {
		ui_svc_spectrum(SPECTRUM_TUNER, ev->value);
		return true;
	}
	if (editing)
		return entry_key(ev);
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9) {
		begin_entry(FIELD_CENTER);
		return entry_key(ev);
	}
	if (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN) {
		int scale = ui_view()->spectrum.scale_db + (ev->action == ACT_NAV_UP ? 20 : -20);
		if (scale >= -40 && scale <= 40)
			ui_svc_spectrum(SPECTRUM_SCALE, scale);
	} else if (ev->action == ACT_SELECT) {
		ui_push(SCR_SPECTRUM_SETTINGS, 0);
	} else {
		return false;
	}
	ui_dirty();
	return true;
}

static uint16_t settings_count(void) { return FIELD_COUNT - 1u + !(board.caps & CAP_KEYPAD); }
static unsigned settings_field(uint16_t i) { return i == FIELD_COUNT - 1u ? FIELD_CENTER : i + 1u; }

static void settings_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t labels[] = { TRN(SPECTRUM_ROW_SQUELCH, "Squelch"), TRN(SPECTRUM_VALUE_SWEEPING, "Sweeping"),
		TRN(SPECTRUM_ROW_BANDWIDTH, "Bandwidth"), TRN(SPECTRUM_ROW_DWELL, "Dwell time"),
		TRN(SPECTRUM_ROW_AUTO_PEAK, "Auto peak"), TRN(SPECTRUM_ROW_FILTER, "Filter"),
		TRN(SPECTRUM_ROW_CENTER, "Center") };
	if (i >= settings_count()) return;
	str_copy(it->label, sizeof it->label, tr(labels[i]));
	unsigned field = settings_field(i);
	if (editing && selected == field)
		it->flags |= UI_ITEM_EDIT;
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	value_text(&b, field);
	if (field == FIELD_BANDWIDTH) sb_str(&b, " MHz");
	else if (field == FIELD_STEP) sb_str(&b, " kHz");
	else if (field == FIELD_DWELL) sb_str(&b, " ms");
	else if (field == FIELD_SQUELCH) sb_str(&b, " dBm");
	else if (field == FIELD_CENTER) sb_str(&b, " MHz");
}

static void settings_select(uint16_t i, bool hold)
{
	if (hold || i >= settings_count()) return;
	unsigned field = settings_field(i);
	const spectrum_t *s = &ui_view()->spectrum;
	if (field == FIELD_BANDWIDTH) {
		unsigned bins = s->bins == 65 ? 97 : s->bins == 97 ? 129 : 65;
		if ((uint64_t)s->step_hz * (bins - 1) > (uint64_t)s->hi_hz - s->lo_hz)
			ui_toast(TR(SPECTRUM_WHY_VALUE, "Value out of range"));
		else ui_svc_spectrum(SPECTRUM_BANDWIDTH, (int32_t)bins);
	} else if (field == FIELD_AUTO) {
		ui_svc_spectrum(SPECTRUM_AUTO_PEAK, (s->auto_peak + 1u) % SPECTRUM_AUTO_COUNT);
	} else if (field == FIELD_FILTER) {
		ui_svc_spectrum(SPECTRUM_FILTER, !s->narrow);
	} else {
		begin_entry(field);
	}
	ui_dirty();
}

static bool settings_key(const input_event_t *ev)
{
	if (!editing || (ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_REPEAT && ev->action != ACT_ENC_STEP))
		return false;
	return entry_key(ev);
}

static const char *settings_hint(void)
{
	return editing ? (stepping ? TR(SPECTRUM_HINT_ADJUST, "Up/Down  OK save  Back") :
		TR(SPECTRUM_HINT_ENTRY, "Digits  # .  OK save  Back")) :
		TR(SPECTRUM_HINT_SETTINGS, "OK change  Back");
}

static void settings_reset(void)
{
	selected = FIELD_CENTER;
	editing = stepping = false;
	length = 0;
}

bool ui_spectrum_typing(void)
{
	return editing && (ui_frame()->scr == SCR_SPECTRUM || ui_frame()->scr == SCR_SPECTRUM_SETTINGS);
}

static void spectrum_enter(void)
{
	settings_reset();
	ui_svc_spectrum(SPECTRUM_START, 0);
}

static void spectrum_leave(void) { ui_svc_spectrum(SPECTRUM_STOP, 0); }

const ui_scr_def_t ui_spectrum_screen = {
	.name = "spectrum", .flags = SF_FIELD, .draw = spectrum_draw, .key = spectrum_key,
	.enter = spectrum_enter, .leave = spectrum_leave,
};

const ui_scr_def_t ui_spectrum_settings_screen = {
	.name = "spectrum-settings", .flags = SF_LIST | SF_HINT_FN,
	.prefix = TRN(SPECTRUM_TITLE_SETTINGS, "Spectrum Settings"), .hint_fn = settings_hint,
	.count = settings_count, .item = settings_item, .select = settings_select, .key = settings_key,
	.enter = settings_reset, .leave = settings_reset,
};
#endif
