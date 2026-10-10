// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 factory calibration reader (docs/spec/hardware-dm1701.md section 12).
// Read only: the working copy at 0x010000 is used when it carries the marker, otherwise the factory table in the
// SPI flash security registers 1-2 is converted in RAM the way a working copy would be. Nothing is ever written.
#include <string.h>
#include "dm1701.h"
#include "radio/cal_curve.h"

#define CAL_SIZE		512u
#define CAL_WORKING_ADDR	0x010000u
#define CAL_MARKER_OFFSET	0x1B0u

// Used when there is no usable table for the band: a low APC reference (a guess, not measured; callers should limit
// TX when valid is false), I/Q gains at mid scale, no reference oscillator correction
#define SAFE_DAC		384u
#define SAFE_GAIN		0x80u

enum { BAND_VHF, BAND_UHF, BAND_COUNT };

// Row offsets in the 512-byte table (12.2): UHF rows have 9 points, VHF rows 5
typedef struct {
	uint16_t at[BAND_COUNT];
} row_t;

static const row_t ROW_HIGH = { { 0x019, 0x010 } };
static const row_t ROW_LOW = { { 0x029, 0x020 } };
static const row_t ROW_MID = { { 0x199, 0x190 } };
static const row_t ROW_VLOW = { { 0x1A9, 0x1A0 } };
static const row_t ROW_DMR_I = { { 0x139, 0x130 } };
static const row_t ROW_DMR_Q = { { 0x149, 0x140 } };
static const row_t ROW_FM_I = { { 0x179, 0x170 } };
static const row_t ROW_FM_Q = { { 0x189, 0x180 } };
static const uint16_t REF_TUNE[BAND_COUNT] = { 0x00C, 0x009 };
static const uint8_t POINTS[BAND_COUNT] = { 5, 9 };

// Interpolation grids (12.3), in 10 Hz units
static const uint32_t POWER_FIRST[BAND_COUNT] = { 13500000u, 40000000u };
static const uint32_t POWER_STEP[BAND_COUNT] = { 1000000u, 1000000u };
static const uint32_t GAIN_FIRST[BAND_COUNT] = { 13600000u, 40000000u };
static const uint32_t GAIN_STEP[BAND_COUNT] = { 950000u, 1000000u };

// Working-copy conversion of the factory power rows (12.1, DM-1701 rows), per mille: very low = low x k0,
// low x= k1, mid x= k2, high x= k3
static const uint16_t K_VLOW[BAND_COUNT] = { 610, 650 };
static const uint16_t K_LOW[BAND_COUNT] = { 1000, 1000 };
static const uint16_t K_MID[BAND_COUNT] = { 850, 860 };
static const uint16_t K_HIGH[BAND_COUNT] = { 1300, 1700 };

static const uint8_t MARKER[8] = { 0x00, 0x25, 0x00, 0x40, 0x00, 0x45, 0x01, 0x40 };

typedef enum { CAL_NONE, CAL_WORKING, CAL_FACTORY } cal_source_t;

static uint8_t table[CAL_SIZE];
static bool band_ok[BAND_COUNT];
static cal_source_t source;	// for the debugger
// Why each copy was rejected, for the diagnostics page: "<copy>:" then three characters per band (VHF, UHF):
// check and point, or "ok" ("w:mark", "w:VG2UO3", "f:VG2ok ")
static char why_working[12], why_factory[12];
static char *why;	// the one being filled
static uint8_t marker_seen[8];

static void reject(int band, char check, uint8_t point)
{
	if (why == NULL) {
		return;
	}
	char *w = &why[2 + band * 3];
	w[0] = band ? 'U' : 'V';
	w[1] = check;
	w[2] = (char)('0' + point % 10);
}

static const uint8_t *row(const row_t *r, int band)
{
	return &table[r->at[band]];
}

static bool all_same(const uint8_t *p, uint8_t n, uint8_t v)
{
	for (uint8_t i = 0; i < n; i++) {
		if (p[i] != v) {
			return false;
		}
	}
	return true;
}

// A band is usable when no power point is 0, very low <= low < high at every point, the very-low, low and mid rows
// and the four gain rows are not erased (all 0xFF); a gain row may be all 0. The high row may legitimately be
// all 0xFF: the UHF conversion (x 1.7) saturates high factory values at 255
static bool band_sane(int band)
{
	uint8_t n = POINTS[band];
	const uint8_t *vlow = row(&ROW_VLOW, band), *low = row(&ROW_LOW, band);
	const uint8_t *mid = row(&ROW_MID, band), *high = row(&ROW_HIGH, band);
	if (all_same(low, n, 0xFF) || all_same(mid, n, 0xFF) || all_same(vlow, n, 0xFF)) {
		reject(band, 'E', 0);
		return false;
	}
	for (uint8_t i = 0; i < n; i++) {
		if (vlow[i] == 0 || low[i] == 0 || mid[i] == 0 || high[i] == 0) {
			reject(band, 'Z', i);
			return false;
		}
		if (vlow[i] > low[i] || low[i] >= high[i]) {
			reject(band, 'O', i);
			return false;
		}
	}
	const row_t *gains[] = { &ROW_DMR_I, &ROW_DMR_Q, &ROW_FM_I, &ROW_FM_Q };
	for (unsigned g = 0; g < sizeof(gains) / sizeof(gains[0]); g++) {
		const uint8_t *p = row(gains[g], band);
		// All zero is real data: the bench DM-1701's factory table has FM I = 0 on both bands (2026-10-09)
		if (all_same(p, n, 0xFF)) {
			reject(band, 'G', (uint8_t)g);
			return false;
		}
	}
	return true;
}

static uint8_t scale(uint8_t v, uint16_t per_mille)
{
	uint32_t r = ((uint32_t)v * per_mille + 500u) / 1000u;
	return r > 255u ? 255u : (uint8_t)r;
}

// The factory table has no very-low row and different power units: derive both in RAM (12.1)
static void derive_working(void)
{
	for (int band = 0; band < BAND_COUNT; band++) {
		uint8_t *vlow = &table[ROW_VLOW.at[band]], *low = &table[ROW_LOW.at[band]];
		uint8_t *mid = &table[ROW_MID.at[band]], *high = &table[ROW_HIGH.at[band]];
		for (uint8_t i = 0; i < POINTS[band]; i++) {
			vlow[i] = scale(low[i], K_VLOW[band]);
			low[i] = scale(low[i], K_LOW[band]);
			mid[i] = scale(mid[i], K_MID[band]);
			high[i] = scale(high[i], K_HIGH[band]);
		}
	}
	memcpy(&table[CAL_MARKER_OFFSET], MARKER, sizeof(MARKER));
}

static bool check_bands(void)
{
	bool any = false;
	for (int band = 0; band < BAND_COUNT; band++) {
		band_ok[band] = band_sane(band);
		if (band_ok[band] && why != NULL) {
			char *w = &why[2 + band * 3];
			w[0] = band ? 'U' : 'V';
			w[1] = 'o';
			w[2] = 'k';
		}
		any = any || band_ok[band];
	}
	return any;
}

static void why_set(char *w, char copy, const char *s)
{
	w[0] = copy;
	w[1] = ':';
	strncpy(&w[2], s, 9);
	w[11] = '\0';
}

static bool load_working(void)
{
	why = why_working;
	why_set(why, 'w', "------");
	if (!dm1701_flash.read(CAL_WORKING_ADDR, table, CAL_SIZE)) {
		why_set(why, 'w', "read");
		return false;
	}
	memcpy(marker_seen, &table[CAL_MARKER_OFFSET], sizeof(marker_seen));
	if (memcmp(&table[CAL_MARKER_OFFSET], MARKER, sizeof(MARKER)) != 0) {
		why_set(why, 'w', "mark");
		return false;
	}
	return check_bands();
}

static bool load_factory(void)
{
	why = why_factory;
	why_set(why, 'f', "------");
	if (!dm1701_flash_read_security(1, 0, &table[0], 256) || !dm1701_flash_read_security(2, 0, &table[256], 256)) {
		why_set(why, 'f', "read");
		return false;
	}
	derive_working();
	return check_bands();
}

bool dm1701_cal_load(void)
{
	source = CAL_NONE;
	if (load_working()) {
		source = CAL_WORKING;
	} else if (load_factory()) {
		source = CAL_FACTORY;
	} else {
		band_ok[BAND_VHF] = band_ok[BAND_UHF] = false;
	}
	return source != CAL_NONE;
}

// Linear over an even grid of points (12.3, radio/cal_curve.h): clamped below the first point, extended past the
// last, truncated
static int32_t interpolate(const uint8_t *p, uint8_t n, uint32_t first, uint32_t step, uint32_t f10, int32_t mul)
{
	uint32_t grid[CAL_CURVE_MAX_POINTS];
	int32_t v[CAL_CURVE_MAX_POINTS];
	for (uint8_t i = 0; i < n; i++) {
		grid[i] = first + step * i;
		v[i] = p[i] * mul;
	}
	return cal_curve_at(grid, v, n, f10, CAL_CURVE_EXTEND);
}

static int32_t clamp(int32_t v, int32_t hi)
{
	return v < 0 ? 0 : (v > hi ? hi : v);
}

static uint16_t power_at(const row_t *r, int band, uint32_t f10)
{
	int32_t v = interpolate(row(r, band), POINTS[band], POWER_FIRST[band], POWER_STEP[band], f10, 16);
	return (uint16_t)clamp(v, 4095);
}

static uint8_t gain_at(const row_t *r, int band, uint32_t f10)
{
	int32_t v = interpolate(row(r, band), POINTS[band], GAIN_FIRST[band], GAIN_STEP[band], f10, 1);
	return (uint8_t)clamp(v, 255);
}

bool dm1701_cal_at(uint32_t hz, dm1701_cal_point_t *out)
{
	int band = hz > 300000000u ? BAND_UHF : BAND_VHF;
	if (!band_ok[band]) {
		*out = (dm1701_cal_point_t){
			.valid = false,
			.power_dac = { SAFE_DAC, SAFE_DAC, SAFE_DAC, SAFE_DAC },
			.fm_i = SAFE_GAIN, .fm_q = SAFE_GAIN, .dmr_i = SAFE_GAIN, .dmr_q = SAFE_GAIN,
			.ref_offset = 0,
		};
		return false;
	}
	uint32_t f10 = hz / 10u;
	out->valid = true;
	out->power_dac[0] = power_at(&ROW_VLOW, band, f10);
	out->power_dac[1] = power_at(&ROW_LOW, band, f10);
	out->power_dac[2] = power_at(&ROW_MID, band, f10);
	out->power_dac[3] = power_at(&ROW_HIGH, band, f10);
	out->fm_i = gain_at(&ROW_FM_I, band, f10);
	out->fm_q = gain_at(&ROW_FM_Q, band, f10);
	out->dmr_i = gain_at(&ROW_DMR_I, band, f10);
	out->dmr_q = gain_at(&ROW_DMR_Q, band, f10);
	out->ref_offset = (int8_t)((int)table[REF_TUNE[band]] - 128);
	return true;
}

const char *dm1701_cal_source(void)
{
	return source == CAL_WORKING ? "working" : source == CAL_FACTORY ? "factory" : "none";
}

// "w:mark f:UO3" plus the working copy's marker bytes, for the diagnostics page
void dm1701_cal_why(char *out, uint8_t len, uint8_t marker[8])
{
	uint8_t n = 0;
	for (const char *s = why_working; *s && n + 1 < len; s++)
		out[n++] = *s;
	if (n + 1 < len)
		out[n++] = ' ';
	for (const char *s = why_factory; *s && n + 1 < len; s++)
		out[n++] = *s;
	out[n] = '\0';
	memcpy(marker, marker_seen, 8);
}

// Raw byte of the table in RAM (the last copy read), for the diagnostics page
uint8_t dm1701_cal_byte(uint16_t offset)
{
	return offset < CAL_SIZE ? table[offset] : 0;
}
