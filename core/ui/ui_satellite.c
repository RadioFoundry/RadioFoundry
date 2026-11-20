// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "config.h"
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "settings/settings.h"
#include "fmt.h"
#include "i18n.h"

#if CONFIG_SATELLITE_TLE
#include "satellite/tracking.h"

static uint8_t selected;
static satellite_tle_lines_t edit_lines;
typedef struct {
	uint32_t catalog_id;
	char name[SATELLITE_RECORD_BRIEF_NAME_MAX];
	uint8_t flags;
} satellite_ui_record_t;
static satellite_ui_record_t pass_record;
static uint16_t pass_tle_age_days;
static bool pass_tle_age_available;
static uint32_t pass_tle_age_generation;
static uint32_t stale_warning_generation;
static uint16_t stale_warning_mask;

static void satellite_cache_record(const satellite_record_brief_t *summary)
{
	pass_record.catalog_id = summary->catalog_id;
	pass_record.flags = summary->flags;
	memcpy(pass_record.name, summary->name, sizeof(pass_record.name));
}

static void satellite_copy_record_name(char name[SATELLITE_RECORD_BRIEF_NAME_MAX + 1u])
{
	memcpy(name, pass_record.name, sizeof(pass_record.name));
	name[sizeof(pass_record.name)] = '\0';
}

static void satellite_warn_stale_tle(uint8_t record_index)
{
	uint32_t generation = ui_svc_satellite_generation();
	if (generation != stale_warning_generation) {
		stale_warning_generation = generation;
		stale_warning_mask = 0u;
	}
	if (record_index >= SATELLITE_STORE_MAX_RECORDS)
		return;
	uint16_t bit = (uint16_t)(1u << record_index);
	if ((stale_warning_mask & bit) != 0u)
		return;
	stale_warning_mask |= bit;
	ui_toast(TR(SATELLITE_TLE_STALE, "TLE is 14+ days old; refresh from site"));
}

static void satellite_line2_kept(void)
{
	if (strlen(edit_lines.line2) != SATELLITE_TLE_LINE_LENGTH) {
		ui_toast(TR(TLE_UPDATE_FAILED_2, "TLE update failed"));
		return;
	}
	if (ui_svc_satellite_replace(selected, &edit_lines))
		ui_toast(TR(TLE_UPDATE_QUEUED, "TLE update queued"));
	else
		ui_toast(TR(SATELLITE_UNAVAILABLE_4, "Satellite unavailable"));
}

static void satellite_line1_kept(void)
{
	if (strlen(edit_lines.line1) != SATELLITE_TLE_LINE_LENGTH) {
		ui_toast(TR(TLE_UPDATE_FAILED_2, "TLE update failed"));
		return;
	}
	edit_lines.line2[0] = '\0';
	ui_menus_satellite_text(edit_lines.line2, 2u, satellite_line2_kept);
}

static void satellite_edit(void)
{
	if (ui_svc_satellite_update_busy() || selected >= ui_svc_satellite_count()) {
		ui_toast(TR(SATELLITE_UNAVAILABLE_4, "Satellite unavailable"));
		return;
	}
	edit_lines.line1[0] = '\0';
	edit_lines.line2[0] = '\0';
	ui_menus_satellite_text(edit_lines.line1, 1u, satellite_line1_kept);
}

static void row(int *y, const char *label, const char *value, color_t value_color)
{
	if (*y + ui_layout.small->h > ui_layout.hint_y)
		return;
	char label_fit[32], value_fit[40];
	int width = gfx_width();
	gfx_text_fit(ui_layout.small, value, width, value_fit, sizeof(value_fit));
	int value_width = gfx_text_width(ui_layout.small, value_fit);
	int label_width = width - value_width - ui_layout.small->w;
	gfx_text_fit(ui_layout.small, label, label_width, label_fit, sizeof(label_fit));
	gfx_text(ui_layout.small, 0, *y, label_fit, ui_col.dim);
	gfx_text(ui_layout.small, width - value_width, *y, value_fit, value_color);
	*y += ui_layout.row_h;
}

static void satellite_eta_minutes(sbuf_t *b, uint32_t minutes)
{
	sb_u32(b, minutes / 60u, 1);
	sb_chr(b, ':');
	sb_u32(b, minutes % 60u, 2);
}

static uint32_t satellite_eta_minutes_from_seconds(uint32_t seconds)
{
	return seconds / 60u + (seconds % 60u != 0u ? 1u : 0u);
}

static void satellite_countdown_pair(int *y, const char *rise_value, const char *set_value,
				     bool visible_now, bool short_labels)
{
	const font_t *font = ui_layout.small;
	const char *rise_label = visible_now || short_labels ? GLYPH_UP : TR(SATELLITE_LABEL_RISE, GLYPH_UP " in");
	const char *set_label = short_labels ? GLYPH_DOWN : TR(SATELLITE_LABEL_SET, GLYPH_DOWN " in");
	int width = gfx_width();
	int gap = font->w / 2 + 1;
	int rise_label_width = gfx_text_width(font, rise_label);
	int rise_value_width = gfx_text_width(font, rise_value);
	int set_label_width = gfx_text_width(font, set_label);
	int set_value_width = gfx_text_width(font, set_value);
	int set_x = width - set_label_width - gap - set_value_width;
	if (rise_label_width + gap + rise_value_width + font->w <= set_x) {
		gfx_text(font, 0, *y, rise_label, ui_col.dim);
		gfx_text(font, rise_label_width + gap, *y, rise_value, ui_col.fg);
		gfx_text(font, set_x, *y, set_label, ui_col.dim);
		gfx_text(font, width - set_value_width, *y, set_value, ui_col.fg);
		*y += ui_layout.row_h;
		return;
	}
	char rise_fit[40], set_fit[40];
	gfx_text_fit(font, rise_value, width / 2 - gap, rise_fit, sizeof(rise_fit));
	gfx_text_fit(font, set_value, width / 2 - gap, set_fit, sizeof(set_fit));
	gfx_text(font, 0, *y, rise_label, ui_col.dim);
	gfx_text(font, width - set_label_width, *y, set_label, ui_col.dim);
	*y += ui_layout.row_h;
	gfx_text(font, 0, *y, rise_fit, ui_col.fg);
	gfx_text(font, width - gfx_text_width(font, set_fit), *y, set_fit, ui_col.fg);
	*y += ui_layout.row_h;
}

static uint32_t satellite_target_hz(uint32_t nominal_hz, int64_t offset_hz)
{
	int64_t target_hz = (int64_t)nominal_hz + offset_hz;
	if (target_hz < 0)
		return 0u;
	if ((uint64_t)target_hz > UINT32_MAX)
		return UINT32_MAX;
	return (uint32_t)target_hz;
}

static void satellite_frequency_rows(int *y, char *value, size_t value_len, const radio_view_t *view,
				     const radio_satellite_tracking_view_t *satellite)
{
	sbuf_t b;
	sb_init(&b, value, value_len);
	sb_freq(&b, satellite_target_hz(satellite->nominal_rx_hz, satellite->rx_offset_hz));
	sb_str(&b, " MHz");
	row(y, "RX", value, ui_col.fg);
	sb_init(&b, value, value_len);
	uint32_t tx_hz = view->state == RS_TX ? view->ch.tx_hz :
		satellite_target_hz(satellite->nominal_tx_hz, satellite->tx_offset_hz);
	sb_freq(&b, tx_hz);
	sb_str(&b, " MHz");
	row(y, "TX", value, ui_col.fg);
}

static void satellite_small_status_row(int *y, const radio_satellite_tracking_view_t *satellite,
				       bool selected_tracking)
{
	char status_fit[40];
	const char *status = !satellite->tracking ? TR(VALUE_OFF, "Off") :
		!selected_tracking ? TR(OTHER_SATELLITE_2, "Other satellite") :
		satellite->status == SATELLITE_TRACKING_VISIBLE ? TR(VISIBLE_2, "Visible") :
		satellite->status == SATELLITE_TRACKING_BELOW_HORIZON ? TR(BELOW_HORIZON_2, "Below horizon") :
		TR(WAITING_FOR_POSITION_TIME_2, "Waiting for position/time");
	gfx_text_fit(ui_layout.small, status, gfx_width() - ui_layout.small->w,
		     status_fit, sizeof(status_fit));
	color_t status_color = selected_tracking && satellite->status == SATELLITE_TRACKING_VISIBLE ?
		ui_col.rx : satellite->tracking ? ui_col.warn : ui_col.dim;
	gfx_text(ui_layout.small, 0, *y, status_fit, status_color);
	*y += ui_layout.row_h;
}

static void satellite_small_frequency_row(int *y, const radio_view_t *view,
					  const radio_satellite_tracking_view_t *satellite)
{
	char rx[32], tx[32], rx_fit[32], tx_fit[32];
	sbuf_t b;
	sb_init(&b, rx, sizeof(rx));
	sb_str(&b, "RX ");
	sb_freq(&b, satellite_target_hz(satellite->nominal_rx_hz, satellite->rx_offset_hz));
	sb_str(&b, " MHz");
	uint32_t tx_hz = view->state == RS_TX ? view->ch.tx_hz :
		satellite_target_hz(satellite->nominal_tx_hz, satellite->tx_offset_hz);
	sb_init(&b, tx, sizeof(tx));
	sb_str(&b, "TX ");
	sb_freq(&b, tx_hz);
	sb_str(&b, " MHz");

	const font_t *font = ui_layout.tiny;
	gfx_text_fit(font, rx, gfx_width() - font->w, rx_fit, sizeof(rx_fit));
	gfx_text(font, 0, *y, rx_fit, ui_col.fg);
	*y += font->h;
	gfx_text_fit(font, tx, gfx_width() - font->w, tx_fit, sizeof(tx_fit));
	gfx_text(font, 0, *y, tx_fit, ui_col.fg);
	*y += font->h;
}

static void satellite_small_countdown_row(int *y, const radio_satellite_view_t *pass)
{
	char rise_value[24], set_value[24];
	if (!pass->valid) {
		str_copy(rise_value, sizeof(rise_value), "--");
		str_copy(set_value, sizeof(set_value), "--");
	} else {
		if (pass->visible_now) {
			str_copy(rise_value, sizeof(rise_value), TR(NOW, "Now"));
		} else {
			uint32_t minutes = satellite_eta_minutes_from_seconds(pass->aos_in_s);
			sbuf_t b;
			sb_init(&b, rise_value, sizeof(rise_value));
			satellite_eta_minutes(&b, minutes);
		}
		if (!pass->los_known || pass->horizon_clipped) {
			str_copy(set_value, sizeof(set_value), "--");
		} else {
			uint32_t minutes = satellite_eta_minutes_from_seconds(pass->los_in_s);
			sbuf_t b;
			sb_init(&b, set_value, sizeof(set_value));
			satellite_eta_minutes(&b, minutes);
		}
	}
	satellite_countdown_pair(y, rise_value, set_value, pass->valid && pass->visible_now, true);
}

static void path_line(int x0, int y0, int x1, int y1, color_t color)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int sx = x0 < x1 ? 1 : -1;
	int dy = y1 > y0 ? y0 - y1 : y1 - y0;
	int sy = y0 < y1 ? 1 : -1;
	int error = dx + dy;
	for (;;) {
		gfx_pixel(x0, y0, color);
		if (x0 == x1 && y0 == y1)
			break;
		int twice_error = 2 * error;
		if (twice_error >= dy) {
			error += dy;
			x0 += sx;
		}
		if (twice_error <= dx) {
			error += dx;
			y0 += sy;
		}
	}
}

static int path_spline_coordinate(int p0, int p1, int p2, int p3, int step)
{
	int linear = -p0 + p2;
	int quadratic = 2 * p0 - 5 * p1 + 4 * p2 - p3;
	int cubic = -p0 + 3 * p1 - 3 * p2 + p3;
	int numerator = 1024 * p1 + 64 * linear * step + 8 * quadratic * step * step +
		cubic * step * step * step;
	return numerator / 1024;
}

static int satellite_isin(int deg)
{
	static const uint16_t q[16] = { 0, 27, 53, 79, 104, 128, 150, 171, 190, 207, 222, 234, 243, 250, 255, 256 };
	deg %= 360;
	if (deg < 0)
		deg += 360;
	int sign = deg >= 180 ? -1 : 1;
	deg %= 180;
	if (deg > 90)
		deg = 180 - deg;
	int i = deg / 6, remainder = deg % 6;
	int value = q[i] + (i < 15 ? (q[i + 1] - q[i]) * remainder / 6 : 0);
	return sign * value;
}

static void path_circle(int cx, int cy, int radius, color_t color)
{
	int x = radius, y = 0, error = 1 - radius;
	while (x >= y) {
		gfx_pixel(cx + x, cy + y, color); gfx_pixel(cx - x, cy + y, color);
		gfx_pixel(cx + x, cy - y, color); gfx_pixel(cx - x, cy - y, color);
		gfx_pixel(cx + y, cy + x, color); gfx_pixel(cx - y, cy + x, color);
		gfx_pixel(cx + y, cy - x, color); gfx_pixel(cx - y, cy - x, color);
		y++;
		if (error < 0) {
			error += 2 * y + 1;
		} else {
			x--;
			error += 2 * (y - x) + 1;
		}
	}
}

static void path_sky_xy(uint16_t azimuth, uint8_t elevation, int cx, int cy, int radius_x, int radius_y,
			int *x, int *y)
{
	int distance_x = radius_x * (90 - elevation) / 90;
	int distance_y = radius_y * (90 - elevation) / 90;
	*x = cx + distance_x * satellite_isin(azimuth) / 256;
	*y = cy - distance_y * satellite_isin(azimuth + 90) / 256;
}

static void path_point_xy(const satellite_path_view_t *path, uint8_t index, int cx, int cy,
			  int radius_x, int radius_y, int *x, int *y)
{
	uint16_t point = path->points[index];
	path_sky_xy(satellite_path_azimuth(point), satellite_path_elevation(point), cx, cy,
		    radius_x, radius_y, x, y);
}

static void path_horizon_pixel(int x, int y, int mark_x, int mark_y, color_t color)
{
	int dx = x - mark_x, dy = y - mark_y;
	if (dx * dx + dy * dy <= 25)
		gfx_pixel(x, y, color);
}

static void path_horizon_arc_circle(int cx, int cy, int radius, int mark_x, int mark_y, color_t color)
{
	int x = radius, y = 0, error = 1 - radius;
	while (x >= y) {
		path_horizon_pixel(cx + x, cy + y, mark_x, mark_y, color);
		path_horizon_pixel(cx - x, cy + y, mark_x, mark_y, color);
		path_horizon_pixel(cx + x, cy - y, mark_x, mark_y, color);
		path_horizon_pixel(cx - x, cy - y, mark_x, mark_y, color);
		path_horizon_pixel(cx + y, cy + x, mark_x, mark_y, color);
		path_horizon_pixel(cx - y, cy + x, mark_x, mark_y, color);
		path_horizon_pixel(cx + y, cy - x, mark_x, mark_y, color);
		path_horizon_pixel(cx - y, cy - x, mark_x, mark_y, color);
		y++;
		if (error < 0) {
			error += 2 * y + 1;
		} else {
			x--;
			error += 2 * (y - x) + 1;
		}
	}
}

static void draw_path(int y, const radio_satellite_view_t *pass, const satellite_path_view_t *path)
{
	int width = gfx_width() - 16;
	int plot_y = y;
	if (!pass->valid) {
		gfx_text(ui_layout.small, 8, y, TR(PREDICTING_PASSES_2, "Predicting passes"), ui_col.dim);
		plot_y += ui_layout.small->h + 2;
	}
	int available = ui_layout.hint_y - plot_y - 2;
	int side = width < available ? width : available;
	int radius_y = side / 2 - 1;
	int radius_x = radius_y;
	if (radius_x < 5 || radius_y < 5)
		return;
	int cx = gfx_width() / 2;
	int cy = plot_y + radius_y;
	path_circle(cx, cy, radius_y, ui_col.line);
	path_circle(cx, cy, radius_y / 2, ui_col.line);
	gfx_hline(cx - radius_x, cy, 2 * radius_x + 1, ui_col.line);
	gfx_vline(cx, cy - radius_y, 2 * radius_y + 1, ui_col.line);
	if (path->count == 1u) {
		int point_x, point_y;
		path_point_xy(path, 0u, cx, cy, radius_x, radius_y, &point_x, &point_y);
		gfx_pixel(point_x, point_y, ui_col.rx);
	}
	for (uint8_t i = 0u; i + 1u < path->count; i++) {
		uint8_t before = i == 0u ? i : (uint8_t)(i - 1u);
		uint8_t after = i + 2u < path->count ? (uint8_t)(i + 2u) : (uint8_t)(i + 1u);
		int x0, y0, x1, y1, x2, y2, x3, y3;
		path_point_xy(path, before, cx, cy, radius_x, radius_y, &x0, &y0);
		path_point_xy(path, i, cx, cy, radius_x, radius_y, &x1, &y1);
		path_point_xy(path, (uint8_t)(i + 1u), cx, cy, radius_x, radius_y, &x2, &y2);
		path_point_xy(path, after, cx, cy, radius_x, radius_y, &x3, &y3);
		int previous_x = x1, previous_y = y1;
		for (int step = 1; step <= 8; step++) {
			int point_x = path_spline_coordinate(x0, x1, x2, x3, step);
			int point_y = path_spline_coordinate(y0, y1, y2, y3, step);
			path_line(previous_x, previous_y, point_x, point_y, ui_col.rx);
			previous_x = point_x;
			previous_y = point_y;
		}
	}
	if (path->count > 0u) {
		int start_x, start_y, end_x, end_y;
		path_sky_xy(satellite_path_azimuth(path->points[0]), 0u, cx, cy, radius_x, radius_y,
			    &start_x, &start_y);
		path_sky_xy(satellite_path_azimuth(path->points[path->count - 1u]), 0u, cx, cy,
			    radius_x, radius_y, &end_x, &end_y);
		path_horizon_arc_circle(cx, cy, radius_y, start_x, start_y, ui_col.rx);
		path_horizon_arc_circle(cx, cy, radius_y, end_x, end_y, ui_col.tx);
	}
	if (pass->visible_now) {
		int elevation = pass->elevation_tenths / 10;
		if (elevation < 0)
			elevation = 0;
		if (elevation > 90)
			elevation = 90;
		int current_x, current_y;
		path_sky_xy(path->azimuth_deg, (uint8_t)elevation, cx, cy, radius_x, radius_y,
			    &current_x, &current_y);
		gfx_fill(current_x - 1, current_y - 1, 3, 3, ui_col.warn);
	}
	gfx_text(ui_layout.tiny, cx + 2, cy - radius_y, "N", ui_col.dim);
}

static uint16_t satellite_pass_count(void)
{
	uint8_t status, completed, total, passes;
	ui_svc_satellite_scan_state(&status, &completed, &total, &passes);
	(void)completed;
	if (total == 0u)
		return 1u;
	if (status != RADIO_SAT_SCAN_COMPLETE)
		return 2u;
	return passes == 0u ? 2u : (uint16_t)passes + 1u;
}

void ui_satellite_passes_tick(void)
{
	ui_frame_t *frame = ui_frame();
	if (frame->scr != SCR_SATELLITE_PASSES)
		return;
	uint8_t status, total, passes;
	ui_svc_satellite_scan_state(&status, NULL, &total, &passes);
	if (status == RADIO_SAT_SCAN_COMPLETE && total != 0u && passes != 0u) {
		if (frame->focus == 0u) {
			frame->focus = 1u;
			ui_dirty();
		}
	} else if (frame->focus != 0u) {
		frame->focus = 0u;
		ui_dirty();
	}
}

static void satellite_update_pass_tle_age(void)
{
	uint32_t generation = ui_svc_satellite_generation();
	if (generation != pass_tle_age_generation) {
		pass_tle_age_generation = generation;
		pass_tle_age_available = false;
		pass_record.catalog_id = 0u;
	}
	uint32_t epoch;
	pass_tle_age_available = ui_svc_satellite_oldest_tle_epoch(&epoch) &&
		ui_svc_satellite_tle_age(epoch, &pass_tle_age_days);
	if (pass_tle_age_available) {
		if (pass_tle_age_days >= 14u)
			satellite_warn_stale_tle(0u);
	}
}

static void satellite_pass_item(uint16_t row_index, ui_item_t *item)
{
	uint8_t status, completed, total, passes;
	ui_svc_satellite_scan_state(NULL, NULL, &total, NULL);
	if (total == 0u) {
		item->flags = UI_ITEM_INFO;
		str_copy(item->label, sizeof(item->label),
			 TR(SATELLITE_NO_TLE_LOAD_SITE, "No TLEs: load from site"));
		return;
	}
	if (row_index == 0u) {
		item->flags = UI_ITEM_INFO;
		str_copy(item->label, sizeof(item->label), TR(SATELLITE_TLE_AGE, "TLE age"));
		satellite_update_pass_tle_age();
		if (pass_tle_age_available) {
			sbuf_t b;
			sb_init(&b, item->value, sizeof(item->value));
			sb_u32(&b, pass_tle_age_days, 1);
			sb_chr(&b, 'd');
		} else {
			str_copy(item->value, sizeof(item->value), "N/A");
		}
		return;
	}
	ui_svc_satellite_scan_state(&status, &completed, &total, &passes);
	if (status != RADIO_SAT_SCAN_COMPLETE) {
		item->flags = UI_ITEM_INFO;
		if (status == RADIO_SAT_SCAN_UNAVAILABLE) {
			str_copy(item->label, sizeof(item->label), TR(GPS_TIME_UNAVAILABLE, "GPS/time unavailable"));
		} else {
			str_copy(item->label, sizeof(item->label), TR(PREDICTING_PASSES_2, "Predicting passes"));
			sbuf_t b;
			sb_init(&b, item->value, sizeof(item->value));
			sb_u32(&b, completed, 1);
			sb_chr(&b, '/');
			sb_u32(&b, total, 1);
		}
		return;
	}
	if (passes == 0u) {
		item->flags = UI_ITEM_INFO;
		str_copy(item->label, sizeof(item->label), TR(NO_PASSES_IN_NEXT_24H, "No passes in next 24h"));
		return;
	}
	row_index--;
	radio_satellite_pass_t pass;
	satellite_record_brief_t summary;
	if (row_index >= passes || !ui_svc_satellite_pass((uint8_t)row_index, &pass) ||
	    !ui_svc_satellite_pass_summary(pass.record_index, &summary))
		return;
	satellite_cache_record(&summary);
	char name[SATELLITE_RECORD_BRIEF_NAME_MAX + 1u];
	satellite_copy_record_name(name);
	str_copy(item->label, sizeof(item->label), name);
	sbuf_t b;
	sb_init(&b, item->value, sizeof(item->value));
	if (pass.visible_now) {
		sb_str(&b, TR(NOW, "Now"));
	} else {
		satellite_eta_minutes(&b, pass.aos_minutes);
	}
	sb_chr(&b, ' ');
	sb_u32(&b, pass.maximum_elevation_deg, 1);
	sb_str(&b, GLYPH_DEGREE);
}

static void satellite_pass_select(uint16_t row_index, bool hold)
{
	(void)hold;
	uint8_t total;
	ui_svc_satellite_scan_state(NULL, NULL, &total, NULL);
	if (total == 0u || row_index == 0u)
		return;
	uint8_t status, passes;
	ui_svc_satellite_scan_state(&status, NULL, NULL, &passes);
	if (status != RADIO_SAT_SCAN_COMPLETE || passes == 0u || row_index > passes)
		return;
	radio_satellite_pass_t pass;
	if (!ui_svc_satellite_pass((uint8_t)(row_index - 1u), &pass))
		return;
	if (!ui_svc_satellite_track(true, pass.record_index)) {
		ui_toast(TR(SATELLITE_UNAVAILABLE_4, "Satellite unavailable"));
		return;
	}
	ui_push(SCR_SATELLITE, pass.record_index);
}

static void satellite_passes_enter(void)
{
	pass_tle_age_available = false;
	pass_tle_age_days = 0u;
	ui_svc_satellite_scan(true);
}

static bool satellite_passes_key(const input_event_t *ev)
{
	(void)ev;
	ui_satellite_passes_tick();
	return false;
}

const ui_scr_def_t ui_satellite_passes_screen = {
	.name = "satellite-passes", .flags = SF_LIST | SF_FIELD, .prefix = TRN(NEXT_PASSES, "Next passes"),
	.hint = TRN(UP_DOWN_PICK_BACK, "Up Down pick  Back"), .empty = TRN(NO_PASSES_IN_NEXT_24H, "No passes in next 24h"),
	.count = satellite_pass_count, .item = satellite_pass_item, .select = satellite_pass_select,
	.key = satellite_passes_key, .enter = satellite_passes_enter,
};

static void satellite_draw(void)
{
	char value[40];
	sbuf_t b;
	bool compact = gfx_height() < 150;
	uint32_t generation = ui_svc_satellite_generation();
	if (generation != pass_tle_age_generation) {
		pass_tle_age_generation = generation;
		pass_tle_age_available = false;
		pass_record.catalog_id = 0u;
	}
	uint8_t count = ui_svc_satellite_count();
	if (selected >= count && count != 0u)
		selected = 0u;
	if (count != 0u && pass_record.catalog_id == 0u) {
		satellite_record_brief_t summary;
		if (ui_svc_satellite_summary(selected, &summary))
			satellite_cache_record(&summary);
	}
	bool have_record = count != 0u && pass_record.catalog_id != 0u;
	char record_name[SATELLITE_RECORD_BRIEF_NAME_MAX + 1u];
	satellite_copy_record_name(record_name);
	const radio_view_t *view = ui_view();
	radio_satellite_view_t satellite = {0};
	ui_svc_satellite_view(&satellite);
	satellite_path_view_t path = {0};
	if (satellite.tracking.tracking && satellite.tracking.index == selected && gfx_height() > 64)
		ui_svc_satellite_path(&path);
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(view, &ui_st, TR(SATELLITE_2, "Satellite"), have_record ? record_name : "") + 2;
	if (!have_record) {
		gfx_text(ui_layout.small, 0, y, TR(SATELLITE_NO_TLE_LOAD_SITE, "No TLEs: load from site"), ui_col.warn);
	} else if (compact && gfx_height() <= 64) {
		bool selected_tracking = satellite.tracking.tracking && satellite.tracking.index == selected;
		if (!selected_tracking || satellite.tracking.status == SATELLITE_TRACKING_UNAVAILABLE)
			satellite_small_status_row(&y, &satellite.tracking, selected_tracking);
		if (selected_tracking) {
			satellite_small_frequency_row(&y, view, &satellite.tracking);
			satellite_small_countdown_row(&y, &satellite);
		}
	} else {
		if (!compact) {
			sb_init(&b, value, sizeof(value));
			sb_str(&b, "NORAD ");
			sb_u32(&b, pass_record.catalog_id, 1);
			row(&y, TR(CATALOG, "Catalog"), value, ui_col.fg);
		}
		if (satellite.tracking.tracking && satellite.tracking.index == selected) {
			if (satellite.tracking.status == SATELLITE_TRACKING_UNAVAILABLE)
				row(&y, TR(TRACKING_4, "Tracking"), TR(WAITING_FOR_POSITION_TIME_2, "Waiting for position/time"), ui_col.warn);
			satellite_frequency_rows(&y, value, sizeof(value), view, &satellite.tracking);
			if (satellite.valid) {
				if (!compact && gfx_width() < 220) {
					if (satellite.visible_now) {
						row(&y, TR(NEXT_PASS, "Next pass"), TR(VISIBLE_NOW, "Visible now"), ui_col.rx);
					} else {
						uint32_t minutes = satellite_eta_minutes_from_seconds(satellite.aos_in_s);
						sb_init(&b, value, sizeof(value));
						satellite_eta_minutes(&b, minutes);
						row(&y, TR(NEXT_PASS, "Next pass"), value, ui_col.fg);
					}
				}
				char rise_value[40], set_value[40];
				if (satellite.visible_now) {
					str_copy(rise_value, sizeof(rise_value), TR(NOW, "Now"));
				} else {
					uint32_t minutes = satellite_eta_minutes_from_seconds(satellite.aos_in_s);
					sb_init(&b, rise_value, sizeof(rise_value));
					satellite_eta_minutes(&b, minutes);
				}
				if (!satellite.los_known) {
					str_copy(set_value, sizeof(set_value), TR(CALCULATING, "Calculating"));
				} else if (satellite.horizon_clipped) {
					str_copy(set_value, sizeof(set_value),
						 TR(SATELLITE_STATE_BEYOND_FORECAST, "Beyond forecast"));
				} else {
					uint32_t minutes = satellite_eta_minutes_from_seconds(satellite.los_in_s);
					sb_init(&b, set_value, sizeof(set_value));
					satellite_eta_minutes(&b, minutes);
				}
				satellite_countdown_pair(&y, rise_value, set_value, satellite.visible_now, false);
			}
			draw_path(y, &satellite, &path);
		} else {
			row(&y, TR(TRACKING_4, "Tracking"), satellite.tracking.tracking ? TR(OTHER_SATELLITE_2, "Other satellite") : TR(VALUE_OFF, "Off"), ui_col.dim);
		}
	}
	bool arm_active = have_record && (pass_record.flags & SATELLITE_RECORD_NEEDS_ARM) && satellite.tracking.tracking &&
		satellite.tracking.index == selected;
	ui_draw_bottom(&ui_st, arm_active ? TR(UP_DOWN_SATELLITE_OK_TRACK, "Up Down satellite  OK track  Hold OK arm  # TLE  Back") :
		TR(UP_DOWN_SATELLITE_OK_TRACK_2, "Up Down satellite  OK track  # TLE  Back"));
	ui_draw_banner(&ui_st);
}

static bool satellite_key(const input_event_t *ev)
{
	int dir = 0;
	if (ev->action == ACT_ENC_STEP) {
		dir = ev->value > 0 ? 1 : ev->value < 0 ? -1 : 0;
	} else if (ev->gesture == GESTURE_LONG && ev->action == ACT_SELECT) {
		radio_satellite_view_t satellite = {0};
		ui_svc_satellite_view(&satellite);
		satellite_record_brief_t summary;
		if (satellite.tracking.tracking && satellite.tracking.index == selected &&
		    ui_svc_satellite_summary(selected, &summary) && (summary.flags & SATELLITE_RECORD_NEEDS_ARM)) {
			ui_svc_satellite_arm();
			ui_toast(TR(SO_50_ARM_REQUESTED, "SO-50 arm requested"));
			return true;
		}
	} else if (ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT) {
		if (ev->action == ACT_NAV_DOWN || ev->action == ACT_NAV_RIGHT)
			dir = 1;
		else if (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_LEFT)
			dir = -1;
		else if (ev->gesture == GESTURE_PRESS && ev->action == ACT_CONTEXT) {
			satellite_edit();
			return true;
		} else if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
			radio_satellite_view_t satellite = {0};
			ui_svc_satellite_view(&satellite);
			bool active = satellite.tracking.tracking && satellite.tracking.index == selected;
			if (!ui_svc_satellite_track(!active, selected))
				ui_toast(TR(SATELLITE_UNAVAILABLE_4, "Satellite unavailable"));
			ui_dirty();
			return true;
		}
	}
	if (dir == 0)
		return false;
	uint8_t count = ui_svc_satellite_count();
	if (count != 0u) {
		pass_record.catalog_id = 0u;
		selected = (uint8_t)((selected + count + dir) % count);
		ui_svc_satellite_select(selected);
		ui_dirty();
	}
	return true;
}

static void satellite_enter(void)
{
	selected = ui_svc_satellite_selected();
	if (selected == SETTINGS_UNSET8)
		selected = 0u;
	pass_record.catalog_id = 0u;
}

static void satellite_leave(void)
{
	// The toast only when a session was active: it explains the retune a Back mid-pass just caused
	radio_satellite_view_t satellite = {0};
	ui_svc_satellite_view(&satellite);
	if (!ui_svc_satellite_track(false, selected))
		ui_toast(TR(SATELLITE_TOAST_STOP_FAILED, "Tracking not stopped"));
	else if (satellite.tracking.tracking)
		ui_toast(TR(SATELLITE_TOAST_STOPPED, "Tracking stopped"));
}

const ui_scr_def_t ui_satellite_screen = {
	.name = "satellite", .flags = SF_FIELD, .draw = satellite_draw, .key = satellite_key, .enter = satellite_enter,
	.leave = satellite_leave,
};
#endif