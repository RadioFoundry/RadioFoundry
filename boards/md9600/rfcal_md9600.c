// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 calibration as the RF code uses it (md9600.h; docs/spec/hardware-md9600.md 11, 19.5): the PA drive per power
// level and the squelch thresholds, on top of the row reader (cal_md9600.h). The layout is unverified, so a row group
// counts only when it looks plausible: PA drive present at every point and rising with the level, squelch thresholds
// present and level 1 apart from level 9. Anything else refuses transmit or leaves the noise reading unscaled.
#include "md9600.h"

// Board power level (5, 10, 25, 40 W) to its row
static const md9600_cal_row_t pa_row[MD9600_POWER_LEVELS] = {
	MD9600_CAL_POWER_LOW, MD9600_CAL_POWER_MID_LOW, MD9600_CAL_POWER_MID, MD9600_CAL_POWER_HIGH,
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
		for (uint8_t l = 0; l < MD9600_POWER_LEVELS; l++) {
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

bool md9600_cal_apc(uint32_t hz, uint8_t power, uint16_t *dac)
{
	int32_t v;
	if (power >= MD9600_POWER_LEVELS || !pa_ok(band_of_hz(hz)) || !md9600_cal_row(pa_row[power], hz, &v)) {
		return false;
	}
	*dac = x16(v);
	return *dac != 0;
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
