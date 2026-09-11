// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// NMEA 0183 RMC / GGA (aprs_nmea.h)
#include <string.h>
#include "aprs/aprs_nmea.h"

#define FIELDS_MAX	16

static int hexval(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

// Decimal "123.45" -> value x 10^scale (rounded down beyond); false when empty or malformed
static bool fixed(const char *s, size_t n, uint8_t scale, uint32_t *out)
{
	uint32_t v = 0;
	uint8_t frac = 0;
	bool dot = false, any = false;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (c == '.') {
			if (dot) {
				return false;
			}
			dot = true;
		} else if (c >= '0' && c <= '9') {
			if (dot) {
				if (frac >= scale) {
					continue;
				}
				frac++;
			}
			if (v > (UINT32_MAX - 9u) / 10u) {
				return false;
			}
			v = v * 10u + (uint32_t)(c - '0');
			any = true;
		} else {
			return false;
		}
	}
	while (frac < scale) {
		v *= 10u;
		frac++;
	}
	*out = v;
	return any;
}

// "ddmm.mmmm" (lon "dddmm.mmmm") and the hemisphere
static bool coord(const char *s, size_t n, char hemi, bool lon, int32_t *out)
{
	uint32_t v;	// degrees x 100 + minutes, in 1e-5 minute
	if (!fixed(s, n, 5, &v)) {
		return false;
	}
	uint32_t deg = v / 10000000u;		// ddmm.mmmmm x 1e5 / 1e7 = dd
	uint32_t min_e5 = v % 10000000u;	// mm.mmmmm x 1e5
	if (min_e5 >= 6000000u || deg > (lon ? 180u : 90u)) {
		return false;
	}
	int64_t e7 = (int64_t)deg * 10000000 + ((int64_t)min_e5 * 10000000) / 6000000;
	bool neg;
	if (lon ? (hemi == 'W') : (hemi == 'S')) {
		neg = true;
	} else if (lon ? (hemi == 'E') : (hemi == 'N')) {
		neg = false;
	} else {
		return false;
	}
	*out = (int32_t)(neg ? -e7 : e7);
	return true;
}

bool nmea_parse(const char *s, size_t n, nmea_fix_t *out)
{
	memset(out, 0, sizeof(*out));
	out->speed_kn10 = 0xFFFFu;
	out->course10 = 0xFFFFu;
	out->alt_dm = INT32_MIN;
	while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) {
		n--;
	}
	if (n < 10 || s[0] != '$' || n > NMEA_LINE_MAX) {
		return false;
	}
	const char *star = memchr(s, '*', n);
	if (star == NULL || (size_t)(star - s) + 3u != n) {
		return false;
	}
	uint8_t sum = 0;
	for (const char *c = s + 1; c < star; c++) {
		sum ^= (uint8_t)*c;
	}
	int h = hexval(star[1]), l = hexval(star[2]);
	if (h < 0 || l < 0 || ((h << 4) | l) != sum) {
		return false;
	}
	// split the fields after "$ttSSS"
	const char *f[FIELDS_MAX];
	size_t fl[FIELDS_MAX];
	uint8_t nf = 0;
	const char *p = s + 1;
	while (p <= star && nf < FIELDS_MAX) {
		const char *e = p;
		while (e < star && *e != ',') {
			e++;
		}
		f[nf] = p;
		fl[nf] = (size_t)(e - p);
		nf++;
		p = e + 1;
	}
	if (fl[0] != 5) {
		return false;
	}
	uint32_t v;
	if (memcmp(f[0] + 2, "RMC", 3) == 0) {
		if (nf < 10) {
			return false;
		}
		out->kind = NMEA_RMC;
		if (fixed(f[1], fl[1], 0, &v)) {
			out->utc_hms = v;
		}
		out->fix = fl[2] == 1 && f[2][0] == 'A';
		if (out->fix && (fl[4] != 1 || fl[6] != 1 || !coord(f[3], fl[3], f[4][0], false, &out->lat) ||
				 !coord(f[5], fl[5], f[6][0], true, &out->lon))) {
			return false;
		}
		if (fixed(f[7], fl[7], 1, &v) && v < 0xFFFFu) {
			out->speed_kn10 = (uint16_t)v;
		}
		if (fixed(f[8], fl[8], 1, &v) && v < 3600u) {
			out->course10 = (uint16_t)v;
		}
		if (fixed(f[9], fl[9], 0, &v)) {
			out->date = v;
		}
		return true;
	}
	if (memcmp(f[0] + 2, "GGA", 3) == 0) {
		if (nf < 10) {
			return false;
		}
		out->kind = NMEA_GGA;
		if (fixed(f[1], fl[1], 0, &v)) {
			out->utc_hms = v;
		}
		out->fix = fl[6] == 1 && f[6][0] >= '1' && f[6][0] <= '8';
		if (out->fix && (fl[3] != 1 || fl[5] != 1 || !coord(f[2], fl[2], f[3][0], false, &out->lat) ||
				 !coord(f[4], fl[4], f[5][0], true, &out->lon))) {
			return false;
		}
		if (fixed(f[7], fl[7], 0, &v) && v < 100u) {
			out->sats = (uint8_t)v;
		}
		if (fl[9] > 0) {
			bool neg = f[9][0] == '-';
			if (fixed(f[9] + (neg ? 1 : 0), fl[9] - (neg ? 1u : 0u), 1, &v)) {
				out->alt_dm = neg ? -(int32_t)v : (int32_t)v;
			}
		}
		return true;
	}
	return false;
}
