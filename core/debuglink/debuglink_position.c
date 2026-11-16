// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The position service on the debug link (docs/spec/debuglink.md 3.16, docs/spec/position.md): the "pos" keys of the
// aprs view sections and the position part of the APRS command (the position settings' fields, NMEA as from the
// host). There with or without APRS; built without CONFIG_POSITION: "pos.built=0".
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

#if CONFIG_POSITION
#include "hal_time.h"
#include "position/position.h"

static void deg(dl_text_t *t, const char *key, int32_t e7)
{
	uint32_t m = e7 < 0 ? 0u - (uint32_t)e7 : (uint32_t)e7;
	dl_text(t, "%s=%s%u.%07u\n", key, e7 < 0 ? "-" : "", (unsigned)(m / 10000000u), (unsigned)(m % 10000000u));
}

void debuglink_view_position(uint8_t part, dl_text_t *t)
{
	static const char *const srcs[] = { "none", "gps", "host", "fixed" };
	position_cfg_t c;

	position_get_cfg(&c);
	if (part == 1) {
		// The first two keep the names they had as APRS settings
		dl_text(t, "set.aprs_pos_src=%u\nset.aprs_dmr_gps=%u\nset.pos_ambiguity=%u\n", c.pref, c.dmr_gps, c.ambiguity);
		deg(t, "set.pos_lat", c.fix_lat);
		deg(t, "set.pos_lon", c.fix_lon);
		return;
	}
	position_fix_t f;
	uint32_t err_m = 0, now_ms = hal_millis();
	uint8_t src = position_report(&f, &err_m, now_ms);
	dl_text(t, "pos.built=1\npos.host_built=%u\npos.src=%s\n", CONFIG_POSITION_HOST, srcs[src < 4 ? src : 0]);
	if (src != POSITION_SRC_NONE) {
		// As a DMR report gives it: the ambiguity applied
		deg(t, "pos.lat", f.lat);
		deg(t, "pos.lon", f.lon);
		dl_text(t, "pos.err_m=%u\npos.age_s=%u\n", (unsigned)err_m, (unsigned)((now_ms - f.ms) / 1000u));
	}
#if CONFIG_GPS
	position_gps_t g;
	bool heard = position_gps(&g, now_ms);
	dl_text(t, "pos.gps_heard=%u\npos.gps_fix=%u\npos.gps_sats=%u\n", heard, heard && g.fix, heard ? g.sats : 0u);
#endif
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool debuglink_cmd_position(const uint8_t *req, uint16_t len, uint8_t *st)
{
	if (len < 1) {
		return false;
	}
#if CONFIG_POSITION_HOST
	if (req[0] == DL_APRS_OP_NMEA) {
		position_nmea((const char *)req + 1, (size_t)len - 1u, false, hal_millis());
		*st = DL_ST_OK;
		return true;
	}
#endif
	if (req[0] != DL_APRS_OP_SET || len != 6) {
		return false;
	}
	uint32_t val = get32(req + 2);
	position_cfg_t c;
	position_get_cfg(&c);
	*st = DL_ST_OK;
	switch (req[1]) {
	case DL_APRS_F_LAT: c.fix_lat = (int32_t)val; break;
	case DL_APRS_F_LON: c.fix_lon = (int32_t)val; break;
	case DL_APRS_F_DMR_GPS: c.dmr_gps = val != 0; break;
	case DL_APRS_F_POS_SRC:
		if (val >= POSITION_PREF_COUNT) {
			*st = DL_ST_BAD_ARG;
			return true;
		}
		c.pref = (uint8_t)val;
		break;
	case DL_APRS_F_AMBIGUITY:
		if (val > 4) {
			*st = DL_ST_BAD_ARG;
			return true;
		}
		c.ambiguity = (uint8_t)val;
		break;
	default:
		return false;	// an APRS field
	}
	position_set_cfg(&c, hal_millis());
	return true;
}

#else

void debuglink_view_position(uint8_t part, dl_text_t *t)
{
	if (part == 0) {
		dl_text(t, "pos.built=0\n");
	}
}

bool debuglink_cmd_position(const uint8_t *req, uint16_t len, uint8_t *st)
{
	(void)req;
	(void)len;
	(void)st;
	return false;
}

#endif
