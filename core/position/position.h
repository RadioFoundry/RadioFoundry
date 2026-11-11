// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Where the radio is (docs/spec/position.md): the board GPS, a host feeding NMEA, or a typed fixed position, chosen by
// the Position source setting. One service for every protocol that reports a position (DMR GPS Info, LRRP, APRS) and
// for the Position screen, so none of them needs another one built in. Built with CONFIG_POSITION (any of them, or the
// GPS). The board GPS half is there with CONFIG_GPS, the host half with CONFIG_POSITION_HOST (what feeds it).
//
// It owns its settings (position_cfg_t): loaded from the settings store at start-up, changed and saved with
// position_set_cfg.
//
// NMEA comes from the GPS driver's task, readers are the radio, UI and debug link tasks: every call takes the lock set
// with position_set_lock (short copies only).
#ifndef RFOUNDRY_POSITION_H
#define RFOUNDRY_POSITION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "settings/settings.h"
#if CONFIG_GPS_SKY
#include "position/nmea_sky.h"
#endif

#define POSITION_ALT_NONE	INT32_MIN
#define POSITION_STALE_MS	60000u		// a GPS / host fix older than this doesn't count

// The Position source setting, and the source a position came from
typedef enum { POSITION_AUTO, POSITION_GPS, POSITION_HOST, POSITION_FIXED, POSITION_PREF_COUNT } position_pref_t;
typedef enum { POSITION_SRC_NONE, POSITION_SRC_GPS, POSITION_SRC_HOST, POSITION_SRC_FIXED } position_src_t;

typedef struct {
	uint8_t pref;			// position_pref_t
	int32_t fix_lat, fix_lon;	// the fixed position, 1e-7 degree, both 0 = none
	int16_t fix_alt_m;		// INT16_MIN = none
	uint8_t ambiguity;		// 0-4: the blur DMR reports get (APRS101 ch. 8; APRS encodes its own)
	bool dmr_gps;			// our position in DMR voice calls (ETSI GPS Info LC, CONFIG_DMR_GPS)
} position_cfg_t;

typedef struct {
	int32_t lat, lon;	// 1e-7 degree
	int32_t alt_m;		// POSITION_ALT_NONE
	uint16_t speed_kmh;	// 0xFFFF unknown
	uint16_t course;	// 0..359, 0xFFFF unknown (or standing still)
	uint32_t utc_hms;	// hhmmss, 0xFFFFFFFF unknown (always for the fixed position)
	uint32_t date;		// ddmmyy, 0 unknown
	uint32_t ms;		// when it was last updated
} position_fix_t;

void position_set_lock(void (*lock)(void), void (*unlock)(void));
// Start-up: no fix, the settings as defaults until position_load
void position_init(void);
// The stored position settings (start-up, and when the settings were replaced)
void position_load(const settings_t *s);
void position_get_cfg(position_cfg_t *c);
// New settings: in use at once and saved (never call it with the lock held: it writes the settings store)
void position_set_cfg(const position_cfg_t *c, uint32_t now_ms);
// The settings in use into a settings record about to be saved, marked as stored. For a writer of the fields next to
// them (APRS), so its first save doesn't leave them unset
void position_cfg_store(settings_t *s);
// The ambiguity setting, 0-4 (APRS formats its own packets with it)
uint8_t position_ambiguity(void);

#if CONFIG_GPS || CONFIG_POSITION_HOST
// An NMEA sentence from the host, or (gps true) from a board GPS driver
void position_nmea(const char *s, size_t n, bool gps, uint32_t now_ms);
#endif

// The position in use now, exact (APRS applies its own ambiguity as it formats). POSITION_SRC_NONE: there is none
uint8_t position_fix(position_fix_t *out, uint32_t now_ms);
// The same for a DMR report (LRRP), with the ambiguity setting applied; err_m the error estimate in meters, the blur
// included
uint8_t position_report(position_fix_t *out, uint32_t *err_m, uint32_t now_ms);
// The ambiguity setting applied to a position from position_fix (src as it returned), for a DMR report built from it
// (APRS over DMR); err_m the error estimate in meters, the blur included
void position_blur(int32_t *lat, int32_t *lon, uint8_t src, uint32_t *err_m);
// Its position only (DMR GPS Info): false when there is none. dmr_gps the "send position in DMR calls" setting (set
// even when false)
bool position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps, uint32_t now_ms);
#if CONFIG_GPS
// Settings > Location > GPS: off drops the board GPS fix and ignores its sentences, so the position falls back at once
void position_gps_enable(bool on);
// The board GPS as it is now, for the Position screen: false when it is off or has sent nothing for a minute
typedef struct {
	bool fix;		// a position under a minute old
	uint8_t sats;		// satellites used (GGA)
	int32_t lat, lon;	// 1e-7 degree, with fix
	int32_t alt_m;		// POSITION_ALT_NONE
	uint16_t hdop10;	// x 10, 0xFFFF unknown
	uint16_t speed_kmh, course;	// 0xFFFF unknown
	uint32_t utc_hms;	// hhmmss of the last fix, 0xFFFFFFFF
	uint8_t mode;		// GSA (CONFIG_GPS_SKY): 1 no fix, 2 2D, 3 3D, 0 unknown
	uint8_t in_view;	// satellites in the table (position_gps_sat)
} position_gps_t;
bool position_gps(position_gps_t *out, uint32_t now_ms);
#if CONFIG_GPS_SKY
// Satellite i of the board GPS's table (0 .. in_view - 1) and whether the fix uses it; false past the end
bool position_gps_sat(uint8_t i, sky_sat_t *out, bool *used);
#endif
#endif

#endif
