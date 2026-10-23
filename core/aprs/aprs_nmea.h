// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NMEA 0183 position sentences (RMC, GGA, any talker: GP, GN, GL, GA, BD) for the position sources (docs/spec/aprs.md
// 6): a board GPS, or a phone / PC feeding its GPS over the USB link. The checksum is required.
#ifndef RFOUNDRY_APRS_NMEA_H
#define RFOUNDRY_APRS_NMEA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NMEA_LINE_MAX		88	// 82 by the standard, a little slack

typedef enum { NMEA_NONE, NMEA_RMC, NMEA_GGA } nmea_kind_t;

typedef struct {
	uint8_t kind;		// nmea_kind_t
	bool fix;		// RMC status A / GGA quality > 0
	int32_t lat, lon;	// 1e-7 degree
	uint16_t speed_kn10;	// RMC: knots x 10, 0xFFFF unknown
	uint16_t course10;	// RMC: degrees x 10 (0..3599), 0xFFFF unknown
	int32_t alt_dm;		// GGA: decimeters above mean sea level, INT32_MIN unknown
	uint8_t sats;		// GGA: satellites used
	uint32_t utc_hms;	// hhmmss
	uint32_t date;		// RMC: ddmmyy, 0 unknown
} nmea_fix_t;

// One sentence ("$GPRMC,...*hh", trailing CR / LF allowed). False for a bad checksum, another sentence or a malformed
// field
bool nmea_parse(const char *s, size_t n, nmea_fix_t *out);

#endif
