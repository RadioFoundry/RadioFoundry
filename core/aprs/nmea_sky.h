// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The satellites a GPS receiver reports (NMEA 0183 GSV and GSA, docs/spec/aprs.md 7.1): where each one is in the sky,
// its signal and whether the fix uses it, and the fix mode. For the GPS status screen (CONFIG_GPS_SKY).
// Pure: no RTOS; the caller locks. Small on purpose: one satellite is 4 bytes.
#ifndef RFOUNDRY_NMEA_SKY_H
#define RFOUNDRY_NMEA_SKY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NMEA_SKY_MAX	28	// satellites kept; a GPS + BeiDou receiver sees about 20 at once

// The systems, by GSV talker (GP GL GA BD/GB GQ) or NMEA 4.1 system ID
enum { SKY_GPS, SKY_GLONASS, SKY_GALILEO, SKY_BEIDOU, SKY_QZSS, SKY_SYS_COUNT };

#define SKY_ELEV_NONE	127u	// elevation not reported (the satellite isn't drawn on the sky plot)
#define SKY_AZ_NONE	255u

typedef struct {
	uint32_t num : 7;	// the number within its system (GLONASS 1-32 for PRN 65-96; GPS 33-64 are SBAS)
	uint32_t seen : 1;	// in this round of GSV sentences
	uint32_t elev : 7;	// degrees 0-90, SKY_ELEV_NONE
	uint32_t sys : 3;	// SKY_*
	uint32_t snr : 6;	// dB-Hz 0-63, 0 not tracked
	uint32_t az2 : 8;	// azimuth / 2 (0-179), SKY_AZ_NONE
} sky_sat_t;

typedef struct {
	sky_sat_t sat[NMEA_SKY_MAX];
	uint8_t n;
	uint8_t mode;		// GSA: 1 no fix, 2 2D, 3 3D, 0 not heard
	uint64_t used[SKY_SYS_COUNT];	// bit num: the fix uses that satellite (GSA)
	uint8_t sig[SKY_SYS_COUNT];	// NMEA 4.1 signal ID whose GSV rounds are kept, 0 none heard
	bool gsa_run;		// the last sentence was a GSA: the next one adds to this round's used set
} nmea_sky_t;

void nmea_sky_init(nmea_sky_t *k);
// Any NMEA sentence; GSV and GSA update the table, the rest end a round of GSA. False when it wasn't one of them
bool nmea_sky_sentence(nmea_sky_t *k, const char *s, size_t n);
static inline bool nmea_sky_used(const nmea_sky_t *k, const sky_sat_t *s)
{
	return (k->used[s->sys] >> s->num) & 1u;
}

#endif
