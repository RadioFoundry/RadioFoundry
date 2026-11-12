// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UI's Position screen over core/position (ui_svc.h): the board GPS, whatever else is built. Without CONFIG_GPS
// the weak stand-ins answer "no GPS" (ui_svc_stub.c)
#include "config.h"
#if CONFIG_GPS
#include "ui_svc.h"
#include "position/position.h"
#include "board.h"
#include "hal_time.h"

// The Position screen reads the board GPS from the position service, the one place its NMEA goes
bool ui_svc_position(ui_position_t *out)
{
	position_gps_t g;
	if (!board_has_gps() || !position_gps(&g, hal_millis())) {
		return false;
	}
	out->fix = g.fix;
	out->sats = g.sats;
	out->lat_e7 = g.lat;
	out->lon_e7 = g.lon;
	out->alt_ok = g.alt_m != POSITION_ALT_NONE;
	out->alt_m = out->alt_ok ? (int16_t)g.alt_m : 0;
	out->mode = g.mode;
	out->in_view = g.in_view;
	out->hdop10 = g.hdop10;
	out->speed_kmh = g.speed_kmh;
	out->course = g.course;
	out->utc_hms = g.utc_hms;
	return true;
}

#if CONFIG_GPS_SKY
bool ui_svc_gps_sat(uint8_t i, ui_gps_sat_t *out)
{
	sky_sat_t k;
	bool used;
	if (!position_gps_sat(i, &k, &used)) {
		return false;
	}
	out->sys = (uint8_t)k.sys;	// SKY_* and UI_GNSS_* share their order
	out->num = (uint8_t)k.num;
	out->elev = k.elev == SKY_ELEV_NONE ? 0xFFu : (uint8_t)k.elev;
	out->snr = (uint8_t)k.snr;
	out->az = k.az2 == SKY_AZ_NONE ? 0xFFFFu : (uint16_t)(k.az2 * 2u);
	out->used = used;
	return true;
}
#endif
#endif
