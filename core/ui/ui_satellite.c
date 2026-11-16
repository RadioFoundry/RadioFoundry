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
static char edit_line1[SATELLITE_TLE_LINE_LENGTH + 1u];
static char edit_line2[SATELLITE_TLE_LINE_LENGTH + 1u];
static satellite_record_summary_t pass_record;

static void satellite_line2_kept(void)
{
	if (strlen(edit_line2) != SATELLITE_TLE_LINE_LENGTH) {
		ui_toast(TR(TLE_UPDATE_FAILED_2, "TLE update failed"));
		return;
	}
	if (ui_svc_satellite_replace(selected, edit_line1, edit_line2))
		ui_toast(TR(TLE_UPDATE_QUEUED, "TLE update queued"));
	else
		ui_toast(TR(SATELLITE_UNAVAILABLE_4, "Satellite unavailable"));
}

static void satellite_line1_kept(void)
{
	if (strlen(edit_line1) != SATELLITE_TLE_LINE_LENGTH) {
		ui_toast(TR(TLE_UPDATE_FAILED_2, "TLE update failed"));
		return;
	}
	edit_line2[0] = '\0';
	ui_menus_satellite_text(edit_line2, 2u, satellite_line2_kept);
}

static void satellite_edit(void)
{
	if (ui_svc_satellite_update_busy() || selected >= ui_svc_satellite_count()) {
		ui_toast(TR(SATELLITE_UNAVAILABLE_4, "Satellite unavailable"));
		return;
	}
	edit_line1[0] = '\0';
	edit_line2[0] = '\0';
	ui_menus_satellite_text(edit_line1, 1u, satellite_line1_kept);
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

static uint32_t satellite_target_hz(uint32_t nominal_hz, int64_t offset_hz)
{
	int64_t target_hz = (int64_t)nominal_hz + offset_hz;
	if (target_hz < 0)
		return 0u;
	if ((uint64_t)target_hz > UINT32_MAX)
		return UINT32_MAX;
	return (uint32_t)target_hz;
}

static void satellite_frequency_rows(int *y, char *value, size_t value_len, const radio_view_t *view)
{
	sbuf_t b;
	sb_init(&b, value, value_len);
	sb_freq(&b, satellite_target_hz(view->satellite_nominal_rx_hz, view->satellite_rx_offset_hz));
	sb_str(&b, " MHz");
	row(y, "RX", value, ui_col.fg);
	sb_init(&b, value, value_len);
	uint32_t tx_hz = view->state == RS_TX ? view->ch.tx_hz :
		satellite_target_hz(view->satellite_nominal_tx_hz, view->satellite_tx_offset_hz);
	sb_freq(&b, tx_hz);
	sb_str(&b, " MHz");
	row(y, "TX", value, ui_col.fg);
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

static void path_circle(int cx, int cy, int r, color_t color)
{
	int x = r, y = 0, error = 1 - r;
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

static void path_sky_xy(uint16_t azimuth, uint8_t elevation, int cx, int cy, int radius, int *x, int *y)
{
	int distance = radius * (90 - elevation) / 90;
	*x = cx + distance * satellite_isin(azimuth) / 256;
	*y = cy - distance * satellite_isin(azimuth + 90) / 256;
}

static void path_point_xy(const satellite_path_view_t *path, uint8_t index, int cx, int cy,
			  int radius, int *x, int *y)
{
	uint16_t point = path->points[index];
	path_sky_xy(satellite_path_azimuth(point), satellite_path_elevation(point), cx, cy, radius, x, y);
}

static void draw_path(int y, const radio_view_t *view, const satellite_path_view_t *path, bool compact)
{
	int width = gfx_width() - 16;
	int plot_y = y;
	if (!view->satellite_pass_valid) {
		gfx_text(ui_layout.small, 8, y, TR(PREDICTING_PASSES_2, "Predicting passes"), ui_col.dim);
		plot_y += ui_layout.small->h + 2;
	}
	int available = ui_layout.hint_y - plot_y - 2;
	int side = width < available ? width : available;
	int radius = side / 2 - 1;
	if (radius < 5)
		return;
	int cx = gfx_width() / 2;
	int cy = plot_y + radius;
	path_circle(cx, cy, radius, ui_col.line);
	path_circle(cx, cy, radius / 2, ui_col.line);
	gfx_hline(cx - radius, cy, 2 * radius + 1, ui_col.line);
	gfx_vline(cx, cy - radius, 2 * radius + 1, ui_col.line);
	if (path->count == 1u) {
		int point_x, point_y;
		path_point_xy(path, 0u, cx, cy, radius, &point_x, &point_y);
		gfx_pixel(point_x, point_y, ui_col.rx);
	}
	for (uint8_t i = 0u; i + 1u < path->count; i++) {
		uint8_t before = i == 0u ? i : (uint8_t)(i - 1u);
		uint8_t after = i + 2u < path->count ? (uint8_t)(i + 2u) : (uint8_t)(i + 1u);
		int x0, y0, x1, y1, x2, y2, x3, y3;
		path_point_xy(path, before, cx, cy, radius, &x0, &y0);
		path_point_xy(path, i, cx, cy, radius, &x1, &y1);
		path_point_xy(path, (uint8_t)(i + 1u), cx, cy, radius, &x2, &y2);
		path_point_xy(path, after, cx, cy, radius, &x3, &y3);
		int previous_x = x1, previous_y = y1;
		for (int step = 1; step <= 8; step++) {
			int point_x = path_spline_coordinate(x0, x1, x2, x3, step);
			int point_y = path_spline_coordinate(y0, y1, y2, y3, step);
			path_line(previous_x, previous_y, point_x, point_y, ui_col.rx);
			previous_x = point_x;
			previous_y = point_y;
		}
	}
	if (view->satellite_pass_visible_now) {
		int elevation = view->satellite_elevation_tenths / 10;
		if (elevation < 0)
			elevation = 0;
		if (elevation > 90)
			elevation = 90;
		int current_x, current_y;
		path_sky_xy(path->azimuth_deg, (uint8_t)elevation, cx, cy, radius, &current_x, &current_y);
		gfx_fill(current_x - 1, current_y - 1, 3, 3, ui_col.warn);
	}
	gfx_text(ui_layout.tiny, cx + 2, cy - radius, "N", ui_col.dim);
	(void)compact;
}

static uint16_t satellite_pass_count(void)
{
	uint8_t status, completed, total, passes;
	ui_svc_satellite_scan_state(&status, &completed, &total, &passes);
	(void)completed;
	(void)total;
	return status == RADIO_SAT_SCAN_COMPLETE ? passes : 1u;
}

static void satellite_pass_item(uint16_t row_index, ui_item_t *item)
{
	uint8_t status, completed, total, passes;
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
	radio_satellite_pass_t pass;
	if (row_index >= passes || !ui_svc_satellite_pass((uint8_t)row_index, &pass) ||
	    !ui_svc_satellite_summary(pass.record_index, &pass_record))
		return;
	str_copy(item->label, sizeof(item->label), pass_record.name);
	sbuf_t b;
	sb_init(&b, item->value, sizeof(item->value));
	if (pass.visible_now) {
		sb_str(&b, TR(NOW, "Now"));
	} else {
		satellite_eta_minutes(&b, pass.aos_minutes);
	}
	sb_chr(&b, ' ');
	sb_u32(&b, pass.maximum_elevation_deg, 1);
	sb_str(&b, "deg");
}

static void satellite_pass_select(uint16_t row_index, bool hold)
{
	(void)hold;
	radio_satellite_pass_t pass;
	if (!ui_svc_satellite_pass((uint8_t)row_index, &pass))
		return;
	if (!ui_svc_satellite_track(true, pass.record_index)) {
		ui_toast(TR(SATELLITE_UNAVAILABLE_4, "Satellite unavailable"));
		return;
	}
	ui_push(SCR_SATELLITE, pass.record_index);
}

static void satellite_passes_enter(void)
{
	ui_svc_satellite_scan(true);
}

const ui_scr_def_t ui_satellite_passes_screen = {
	.name = "satellite-passes", .flags = SF_LIST | SF_FIELD, .prefix = TRN(NEXT_PASSES, "Next passes"),
	.hint = TRN(UP_DOWN_PICK_BACK, "Up Down pick  Back"), .empty = TRN(NO_PASSES_IN_NEXT_24H, "No passes in next 24h"),
	.count = satellite_pass_count, .item = satellite_pass_item, .select = satellite_pass_select,
	.enter = satellite_passes_enter,
};

static void satellite_draw(void)
{
	char value[40];
	sbuf_t b;
	bool compact = gfx_height() < 150;
	uint8_t count = ui_svc_satellite_count();
	if (selected >= count && count != 0u)
		selected = 0u;
	satellite_record_summary_t record;
	bool have_record = count != 0u && ui_svc_satellite_summary(selected, &record);
	const radio_view_t *view = ui_view();
	satellite_path_view_t path = {0};
	if (view->satellite_tracking && view->satellite_index == selected)
		ui_svc_satellite_path(&path);
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(view, &ui_st, TR(SATELLITE_2, "Satellite"), have_record ? record.name : "") + 2;
	if (!have_record) {
		gfx_text(ui_layout.small, 0, y, TR(NO_TLE_RECORDS_INSTALLED, "No TLE records installed"), ui_col.warn);
	} else if (compact) {
		if (view->satellite_tracking && view->satellite_index == selected) {
			const char *status = view->satellite_status == SATELLITE_TRACKING_VISIBLE ? TR(VISIBLE_2, "Visible") :
				view->satellite_status == SATELLITE_TRACKING_BELOW_HORIZON ? TR(BELOW_HORIZON_2, "Below horizon") :
				TR(WAITING_FOR_POSITION_TIME_2, "Waiting for position/time");
			row(&y, TR(TRACKING_4, "Tracking"), status, view->satellite_status == SATELLITE_TRACKING_VISIBLE ?
			     ui_col.rx : ui_col.warn);
			satellite_frequency_rows(&y, value, sizeof(value), view);
			if (view->satellite_pass_valid && !view->satellite_pass_visible_now) {
				uint32_t minutes = view->satellite_aos_in_s / 60u;
				sb_init(&b, value, sizeof(value));
				satellite_eta_minutes(&b, minutes);
				row(&y, TR(AOS_IN_2, "AOS in"), value, ui_col.fg);
			}
			draw_path(y, view, &path, compact);
		} else {
			row(&y, TR(TRACKING_4, "Tracking"), view->satellite_tracking ? TR(OTHER_SATELLITE_2, "Other satellite") : TR(VALUE_OFF, "Off"), ui_col.dim);
		}
	} else {
		sb_init(&b, value, sizeof(value));
		sb_str(&b, "NORAD ");
		sb_u32(&b, record.catalog_id, 1);
		row(&y, TR(CATALOG, "Catalog"), value, ui_col.fg);
		if (view->satellite_tracking && view->satellite_index == selected) {
			const char *status = view->satellite_status == SATELLITE_TRACKING_VISIBLE ? TR(VISIBLE_2, "Visible") :
				view->satellite_status == SATELLITE_TRACKING_BELOW_HORIZON ? TR(BELOW_HORIZON_2, "Below horizon") :
				TR(WAITING_FOR_POSITION_TIME_2, "Waiting for position/time");
			row(&y, TR(TRACKING_4, "Tracking"), status, view->satellite_status == SATELLITE_TRACKING_VISIBLE ?
			     ui_col.rx : ui_col.warn);
			satellite_frequency_rows(&y, value, sizeof(value), view);
			if (view->satellite_pass_valid) {
				if (view->satellite_pass_visible_now) {
					row(&y, TR(NEXT_PASS, "Next pass"), TR(VISIBLE_NOW, "Visible now"), ui_col.rx);
				} else {
					uint32_t minutes = view->satellite_aos_in_s / 60u;
					sb_init(&b, value, sizeof(value));
					satellite_eta_minutes(&b, minutes);
					row(&y, TR(AOS_IN_2, "AOS in"), value, ui_col.fg);
				}
				if (!view->satellite_los_known && !view->satellite_pass_visible_now) {
					row(&y, TR(LOS, "LOS"), TR(CALCULATING, "Calculating"), ui_col.dim);
				} else if (view->satellite_pass_horizon_clipped) {
					row(&y, "", TR(LOS_BEYOND_FORECAST, "LOS beyond forecast"), ui_col.dim);
				} else {
					uint32_t minutes = view->satellite_los_in_s / 60u;
					sb_init(&b, value, sizeof(value));
					satellite_eta_minutes(&b, minutes);
					row(&y, TR(LOS_IN, "LOS in"), value, ui_col.fg);
				}
			}
			draw_path(y, view, &path, compact);
		} else {
			row(&y, TR(TRACKING_4, "Tracking"), view->satellite_tracking ? TR(OTHER_SATELLITE_2, "Other satellite") : TR(VALUE_OFF, "Off"), ui_col.dim);
		}
	}
	bool so50_active = have_record && record.catalog_id == 27607u && view->satellite_tracking &&
		view->satellite_index == selected;
	ui_draw_bottom(&ui_st, so50_active ? TR(UP_DOWN_SATELLITE_OK_TRACK, "Up Down satellite  OK track  Hold OK arm  # TLE  Back") :
		TR(UP_DOWN_SATELLITE_OK_TRACK_2, "Up Down satellite  OK track  # TLE  Back"));
	ui_draw_banner(&ui_st);
}

static bool satellite_key(const input_event_t *ev)
{
	int dir = 0;
	if (ev->action == ACT_ENC_STEP) {
		dir = ev->value > 0 ? 1 : ev->value < 0 ? -1 : 0;
	} else if (ev->gesture == GESTURE_LONG && ev->action == ACT_SELECT) {
		const radio_view_t *view = ui_view();
		if (view->satellite_tracking && view->satellite_index == selected &&
		    ui_svc_satellite_summary(selected, &pass_record) && pass_record.catalog_id == 27607u) {
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
			const radio_view_t *view = ui_view();
			bool active = view->satellite_tracking && view->satellite_index == selected;
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
}

const ui_scr_def_t ui_satellite_screen = {
	.name = "satellite", .flags = SF_FIELD, .draw = satellite_draw, .key = satellite_key, .enter = satellite_enter,
};
#endif