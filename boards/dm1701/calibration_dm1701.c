// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 factory calibration reader (docs/spec/hardware-dm1701.md section 12).
// Read only: the working copy at 0x010000 is used when it carries the marker, otherwise the factory table in the
// SPI flash security registers 1-2 is converted in RAM the way a working copy would be. Nothing is ever written.
#include <string.h>
#include "dm1701.h"

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
// and the four gain rows are not erased (all 0xFF) and the gain rows are not all 0. The high row may legitimately be
// all 0xFF: the UHF conversion (x 1.7) saturates high factory values at 255
static bool band_sane(int band)
{
	uint8_t n = POINTS[band];
	const uint8_t *vlow = row(&ROW_VLOW, band), *low = row(&ROW_LOW, band);
	const uint8_t *mid = row(&ROW_MID, band), *high = row(&ROW_HIGH, band);
	if (all_same(low, n, 0xFF) || all_same(mid, n, 0xFF) || all_same(vlow, n, 0xFF)) {
		return false;
	}
	for (uint8_t i = 0; i < n; i++) {
		if (vlow[i] == 0 || low[i] == 0 || mid[i] == 0 || high[i] == 0) {
			return false;
		}
		if (vlow[i] > low[i] || low[i] >= high[i]) {
			return false;
		}
	}
	const row_t *gains[] = { &ROW_DMR_I, &ROW_DMR_Q, &ROW_FM_I, &ROW_FM_Q };
	for (unsigned g = 0; g < sizeof(gains) / sizeof(gains[0]); g++) {
		const uint8_t *p = row(gains[g], band);
		if (all_same(p, n, 0xFF) || all_same(p, n, 0x00)) {
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
		any = any || band_ok[band];
	}
	return any;
}

static bool load_working(void)
{
	return dm1701_flash.read(CAL_WORKING_ADDR, table, CAL_SIZE) &&
	       memcmp(&table[CAL_MARKER_OFFSET], MARKER, sizeof(MARKER)) == 0 && check_bands();
}

static bool load_factory(void)
{
	if (!dm1701_flash_read_security(1, 0, &table[0], 256) || !dm1701_flash_read_security(2, 0, &table[256], 256)) {
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

// Linear interpolation over a row of points (12.3): clamp below the first point, extrapolate past the last
static int32_t interpolate(const uint8_t *p, uint8_t n, uint32_t first, uint32_t step, uint32_t f10, int32_t mul)
{
	if (f10 <= first) {
		return p[0] * mul;
	}
	uint32_t i = (f10 - first) / step;
	if (i > (uint32_t)n - 2u) {
		i = (uint32_t)n - 2u;
	}
	int64_t a = (int64_t)p[i] * mul;
	int64_t b = (int64_t)p[i + 1] * mul;
	int64_t d = (int64_t)f10 - (int64_t)(first + i * step);
	return (int32_t)(a + (b - a) * d / (int64_t)step);
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
