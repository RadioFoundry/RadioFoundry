// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The squelch's reading for the radio core (squelch_db25d.h)
#include "squelch_db25d.h"

static uint8_t clamp_u8(int32_t v)
{
	return v < 0 ? 0u : v > 254 ? 254u : (uint8_t)v;
}

uint8_t db25d_sql_noise(const int16_t open_dbm[DB25D_SQL_LEVELS], int16_t rssi_dbm)
{
	if (rssi_dbm < open_dbm[0]) {
		return clamp_u8(DB25D_SQL_OPEN_L1 + (open_dbm[0] - rssi_dbm));
	}
	for (int n = 1; n < DB25D_SQL_LEVELS; n++) {
		if (rssi_dbm < open_dbm[n]) {
			// Between level n's threshold and level n + 1's: under level n's line, at or over level n + 1's
			int32_t line = DB25D_SQL_OPEN_L1 - (n - 1) * DB25D_SQL_STEP;
			int32_t span = open_dbm[n] - open_dbm[n - 1];
			int32_t frac = span > 0 ? (rssi_dbm - open_dbm[n - 1]) * (DB25D_SQL_STEP - 1) / span : 0;
			return clamp_u8(line - 1 - frac);
		}
	}
	int32_t line9 = DB25D_SQL_OPEN_L1 - (DB25D_SQL_LEVELS - 1) * DB25D_SQL_STEP;
	return clamp_u8(line9 - 1 - (rssi_dbm - open_dbm[DB25D_SQL_LEVELS - 1]));
}

bool db25d_sql_fd6818_carrier(uint8_t noise, uint16_t glitch, int16_t rssi_dbm, bool wide)
{
	if (glitch > (wide ? 120u : 140u)) {
		return false;
	}
	// The noise limit: 82.5 - 0.38 x RSSI (in the chip's half-dB units, dBm x 2 + 320), at most 45, at least 41 narrow
	// or 39 wide
	int32_t raw = (int32_t)rssi_dbm * 2 + 320;
	int32_t limit = (825 - 38 * raw / 10) / 10;
	if (limit > 45) {
		limit = 45;
	}
	int32_t floor = wide ? 39 : 41;
	if (limit < floor) {
		limit = floor;
	}
	return noise <= limit;
}
