// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GSV / GSA into the satellite table (nmea_sky.h)
#include "config.h"
#if CONFIG_GPS_SKY
#include <string.h>
#include "aprs/nmea_sky.h"
#include "aprs/aprs_nmea.h"

#define FIELDS_MAX	22	// GSV: 4 + 4 satellites x 4 + the NMEA 4.1 signal ID
#define SYS_ALL		0xFFu	// a GN talker: the satellites' numbers say their systems

static uint8_t talker_sys(const char *t)
{
	static const struct { char t[3]; uint8_t sys; } map[] = {
		{ "GP", SKY_GPS }, { "GL", SKY_GLONASS }, { "GA", SKY_GALILEO }, { "BD", SKY_BEIDOU },
		{ "GB", SKY_BEIDOU }, { "GQ", SKY_QZSS }, { "GN", SYS_ALL },
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (t[0] == map[i].t[0] && t[1] == map[i].t[1]) {
			return map[i].sys;
		}
	}
	return SKY_SYS_COUNT;
}

// A PRN as its system and the number within it: the ranges receivers use for a mixed talker (NMEA 4.0 and the
// u-blox extended numbering), else the talker's own system. False when it is neither (SBAS, out of range)
static bool sat_id(uint8_t talker, uint32_t prn, uint8_t *sys, uint8_t *num)
{
	static const struct { uint16_t lo, hi; uint8_t sys; } ranges[] = {
		{ 65, 96, SKY_GLONASS }, { 193, 202, SKY_QZSS }, { 201, 263, SKY_BEIDOU }, { 301, 336, SKY_GALILEO },
		{ 401, 463, SKY_BEIDOU },
	};
	if (prn > 64) {
		for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
			if (prn >= ranges[i].lo && prn <= ranges[i].hi) {
				*sys = ranges[i].sys;
				*num = (uint8_t)(prn - ranges[i].lo + 1u);
				return true;
			}
		}
		return false;
	}
	if (prn == 0 || (talker == SYS_ALL && prn > 32) || (talker == SKY_GPS && prn > 32)) {
		return false;
	}
	*sys = talker == SYS_ALL ? SKY_GPS : talker;
	*num = (uint8_t)prn;
	return true;
}

static uint32_t num_or(const char *s, uint8_t n, uint32_t none)
{
	uint32_t v;
	return nmea_number(s, n, 0, &v) ? v : none;
}

static void gsv(nmea_sky_t *k, uint8_t talker, const char **f, const uint8_t *fl, uint8_t nf)
{
	uint32_t total = num_or(f[1], fl[1], 0), msg = num_or(f[2], fl[2], 0);
	if (msg == 0 || msg > total) {
		return;
	}
	if (msg == 1) {
		for (uint8_t i = 0; i < k->n; i++) {
			if (talker == SYS_ALL || k->sat[i].sys == talker) {
				k->sat[i].seen = 0;
			}
		}
	}
	for (uint8_t g = 4; g + 4u <= nf; g += 4) {
		uint8_t sys, num;
		if (!sat_id(talker, num_or(f[g], fl[g], 0), &sys, &num)) {
			continue;
		}
		uint8_t i = 0;
		while (i < k->n && !(k->sat[i].sys == sys && k->sat[i].num == num)) {
			i++;
		}
		if (i == k->n) {
			if (k->n == NMEA_SKY_MAX) {
				continue;
			}
			k->n++;
		}
		uint32_t el = num_or(f[g + 1], fl[g + 1], SKY_ELEV_NONE);
		uint32_t az = num_or(f[g + 2], fl[g + 2], 0xFFFFu);
		uint32_t snr = num_or(f[g + 3], fl[g + 3], 0);
		sky_sat_t *s = &k->sat[i];
		s->sys = sys;
		s->num = num;
		s->seen = 1;
		s->elev = el > 90u ? SKY_ELEV_NONE : el;
		s->az2 = az > 359u ? SKY_AZ_NONE : az / 2u;
		s->snr = snr > 63u ? 63u : snr;
	}
	if (msg == total) {	// the round is over: drop what it didn't mention
		uint8_t j = 0;
		for (uint8_t i = 0; i < k->n; i++) {
			if (k->sat[i].seen || (talker != SYS_ALL && k->sat[i].sys != talker)) {
				k->sat[j++] = k->sat[i];
			}
		}
		k->n = j;
	}
}

static void gsa(nmea_sky_t *k, uint8_t talker, const char **f, const uint8_t *fl, uint8_t nf)
{
	if (nf < 18) {
		return;
	}
	uint32_t mode = num_or(f[2], fl[2], 0);
	if (!k->gsa_run) {	// a new round: one GSA per system follows (GP, BD, ...) or several GN
		memset(k->used, 0, sizeof(k->used));
		k->mode = 0;
		uint32_t v;
		k->pdop10 = nmea_number(f[15], fl[15], 1, &v) && v < 0xFFFFu ? (uint16_t)v : 0xFFFFu;
		k->vdop10 = nmea_number(f[17], fl[17], 1, &v) && v < 0xFFFFu ? (uint16_t)v : 0xFFFFu;
	}
	k->gsa_run = true;
	if (mode >= 1 && mode <= 3 && mode > k->mode) {
		k->mode = (uint8_t)mode;
	}
	if (talker == SYS_ALL && nf > 18) {	// NMEA 4.1: the system ID
		uint32_t id = num_or(f[18], fl[18], 0);
		if (id >= 1 && id <= SKY_SYS_COUNT) {
			talker = (uint8_t)(id - 1u);
		}
	}
	for (uint8_t i = 3; i <= 14; i++) {
		uint8_t sys, num;
		if (sat_id(talker, num_or(f[i], fl[i], 0), &sys, &num)) {
			k->used[sys] |= (uint64_t)1 << num;
		}
	}
}

void nmea_sky_init(nmea_sky_t *k)
{
	memset(k, 0, sizeof(*k));
	k->pdop10 = k->vdop10 = 0xFFFFu;
}

bool nmea_sky_sentence(nmea_sky_t *k, const char *s, size_t n)
{
	const char *f[FIELDS_MAX];
	uint8_t fl[FIELDS_MAX];
	uint8_t nf = nmea_fields(s, n, f, fl, FIELDS_MAX);
	bool is_gsa = nf >= 4 && memcmp(f[0] + 2, "GSA", 3) == 0;
	bool is_gsv = nf >= 4 && memcmp(f[0] + 2, "GSV", 3) == 0;
	if (!is_gsa) {
		k->gsa_run = false;
	}
	uint8_t talker = nf > 0 ? talker_sys(f[0]) : SKY_SYS_COUNT;
	if ((!is_gsa && !is_gsv) || talker == SKY_SYS_COUNT) {
		return false;
	}
	if (is_gsv) {
		gsv(k, talker, f, fl, nf);
	} else {
		gsa(k, talker, f, fl, nf);
	}
	return true;
}
#endif
