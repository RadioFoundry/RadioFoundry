// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 calibration as the RF code uses it (md9600.h; docs/spec/hardware-md9600.md 11, 19.8): the PA drive per power
// level and the squelch thresholds, on top of the row reader (cal_md9600.h). The layout is unverified, so a row group
// counts only when it looks plausible: PA drive present at every point and rising with the level, squelch thresholds
// present and level 1 apart from level 9. Anything else refuses transmit or leaves the noise reading unscaled.
#include "md9600.h"

// The factory PA rows, lowest first
static const md9600_cal_row_t pa_row[MD9600_FACTORY_PA_ROWS] = {
	MD9600_CAL_POWER_LOW, MD9600_CAL_POWER_MID_LOW, MD9600_CAL_POWER_MID, MD9600_CAL_POWER_HIGH,
};

// The power levels from a wattmeter on one V1-V3 radio, two frequencies per band, cold and warm (hardware-md9600.md
// 5.6). Below about 10 W the output follows the absolute drive, the same at every frequency of a band, so the three
// low levels are fixed drives, each at least 20 counts over the drive where the power loop loses control (the PA
// jumps to about 10 W there). Above, the output moves with frequency as the factory rows do: 25 W lies between the
// rows by the watts they gave, the top level a fixed step over the top row
typedef struct {
	uint16_t low[3];			// 100 mW (UHF) or 250 mW (VHF), 1 W, 5 W
	uint16_t row_dw[MD9600_FACTORY_PA_ROWS];	// what the factory rows gave, tenths of a watt
	uint16_t mid_dw;			// level 3: 25 W
	uint16_t top_step;			// level 4 over the top row
	uint16_t floor;				// the drive where the output reaches zero
} pa_band_t;

static const pa_band_t pa_band[2] = {
	[MD9600_UHF] = { { 456, 509, 680 }, { 92, 131, 242, 394 }, 250, 337, 440 },
	[MD9600_VHF] = { { 880, 892, 947 }, { 101, 183, 266, 493 }, 250, 45, 870 },
};

// The calibration points of a band: UHF 9 from 400 MHz in 10 MHz steps, VHF 5 from 136 MHz in 9.5 MHz steps
static uint32_t point_hz(uint8_t band, uint8_t i)
{
	return band == MD9600_VHF ? 136000000u + i * 9500000u : 400000000u + i * 10000000u;
}

static uint8_t points(uint8_t band)
{
	return band == MD9600_VHF ? 5u : 9u;
}

static bool pa_ok(uint8_t band)
{
	for (uint8_t i = 0; i < points(band); i++) {
		int32_t prev = 0;
		for (uint8_t l = 0; l < MD9600_FACTORY_PA_ROWS; l++) {
			int32_t v;
			if (!md9600_cal_row(pa_row[l], point_hz(band, i), &v) || v <= prev) {
				return false;
			}
			prev = v;
		}
	}
	return true;
}

static bool sql_ok(uint8_t band)
{
	for (uint8_t i = 0; i < points(band); i++) {
		int32_t l1, l9;
		if (!md9600_cal_row(MD9600_CAL_SQ1_OPEN, point_hz(band, i), &l1) ||
		    !md9600_cal_row(MD9600_CAL_SQ9_OPEN, point_hz(band, i), &l9) || l1 == l9) {
			return false;
		}
	}
	return true;
}

uint8_t md9600_cal_rf_rows(uint8_t band)
{
	int32_t v;
	uint8_t r = 0;
	if (md9600_cal_row(MD9600_CAL_RX_TUNE, point_hz(band, 0), &v)) {
		r |= MD9600_CALOK_RX;
	}
	if (pa_ok(band)) {
		r |= MD9600_CALOK_PA;
	}
	if (sql_ok(band)) {
		r |= MD9600_CALOK_SQL;
	}
	return r;
}

static uint8_t band_of_hz(uint32_t hz)
{
	return hz < MD9600_BAND_SPLIT_HZ ? MD9600_VHF : MD9600_UHF;
}

static uint16_t x16(int32_t v)
{
	v *= 16;	// DAC values and 12-bit readings are the stored byte x 16
	return (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : v);
}

bool md9600_cal_pa_row(uint32_t hz, uint8_t row, uint16_t *dac)
{
	int32_t v;
	if (row >= MD9600_FACTORY_PA_ROWS || !pa_ok(band_of_hz(hz)) || !md9600_cal_row(pa_row[row], hz, &v)) {
		return false;
	}
	*dac = x16(v);
	return *dac != 0;
}

bool md9600_cal_apc(uint32_t hz, uint8_t power, uint16_t *dac)
{
	uint8_t band = band_of_hz(hz);
	const pa_band_t *b = &pa_band[band];
	uint16_t row[MD9600_FACTORY_PA_ROWS], lv[MD9600_POWER_LEVELS];
	if (power >= MD9600_POWER_LEVELS) {
		return false;
	}
	for (uint8_t r = 0; r < MD9600_FACTORY_PA_ROWS; r++) {
		if (!md9600_cal_pa_row(hz, r, &row[r])) {
			return false;
		}
	}
	for (uint8_t l = 0; l < 3u; l++) {
		lv[l] = b->low[l];
	}
	uint8_t r = 0;	// 25 W between the two rows around it (row 0 below it on every radio measured)
	while (r + 2u < MD9600_FACTORY_PA_ROWS && b->row_dw[r + 1u] < b->mid_dw) {
		r++;
	}
	int32_t span = (int32_t)b->row_dw[r + 1u] - (int32_t)b->row_dw[r];
	lv[3] = (uint16_t)(row[r] + ((int32_t)row[r + 1u] - row[r]) * ((int32_t)b->mid_dw - b->row_dw[r]) / span);
	lv[4] = (uint16_t)(row[MD9600_FACTORY_PA_ROWS - 1u] + b->top_step);
	// A radio whose rows don't fit the measured shape (a level not over the one below, or the top past the limit)
	// transmits on no level rather than on a wrong one
	for (uint8_t l = 1; l < MD9600_POWER_LEVELS; l++) {
		if (lv[l] <= lv[l - 1u]) {
			return false;
		}
	}
	if (lv[4] > MD9600_APC_MAX) {
		return false;
	}
	*dac = lv[power];
	return true;
}

uint16_t md9600_cal_apc_floor(uint8_t band)
{
	return band < 2u ? pa_band[band].floor : 0u;
}

bool md9600_cal_squelch(uint32_t hz, uint16_t *l1_open, uint16_t *l9_open)
{
	int32_t l1, l9;
	if (!sql_ok(band_of_hz(hz)) || !md9600_cal_row(MD9600_CAL_SQ1_OPEN, hz, &l1) ||
	    !md9600_cal_row(MD9600_CAL_SQ9_OPEN, hz, &l9)) {
		return false;
	}
	*l1_open = x16(l1);	// the thresholds are 8-bit readings: the 12-bit average >> 4 (unverified)
	*l9_open = x16(l9);
	return *l1_open != *l9_open;
}
