// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A squelch reading from signal strength (sql_rssi.h)
#include "radio/sql_rssi.h"

static uint8_t clamp_u8(int32_t v)
{
	return v < 0 ? 0u : v > 254 ? 254u : (uint8_t)v;
}

bool radio_sql_rssi_levels(int16_t open_dbm[RADIO_SQUELCH_MAX])
{
	int16_t read = open_dbm[0];
	for (int n = 1; n < RADIO_SQUELCH_MAX; n++) {
		if (open_dbm[n] < read) {
			return false;
		}
		read = open_dbm[n];
		if (open_dbm[n] <= open_dbm[n - 1]) {
			open_dbm[n] = (int16_t)(open_dbm[n - 1] + 1);
		}
	}
	return true;
}

uint8_t radio_sql_rssi_noise(const int16_t open_dbm[RADIO_SQUELCH_MAX], int16_t rssi_dbm)
{
	if (rssi_dbm < open_dbm[0]) {
		return clamp_u8(RADIO_SQL_OPEN_L1 + (open_dbm[0] - rssi_dbm));
	}
	for (int n = 1; n < RADIO_SQUELCH_MAX; n++) {
		if (rssi_dbm < open_dbm[n]) {
			// Between level n's threshold and level n + 1's: under level n's line, at or over level n + 1's
			int32_t line = RADIO_SQL_OPEN_L1 - (n - 1) * RADIO_SQL_DB_PER_STEP;
			int32_t span = open_dbm[n] - open_dbm[n - 1];
			int32_t frac = span > 0 ? (rssi_dbm - open_dbm[n - 1]) * (RADIO_SQL_DB_PER_STEP - 1) / span : 0;
			return clamp_u8(line - 1 - frac);
		}
	}
	int32_t line9 = RADIO_SQL_OPEN_L1 - (RADIO_SQUELCH_MAX - 1) * RADIO_SQL_DB_PER_STEP;
	return clamp_u8(line9 - 1 - (rssi_dbm - open_dbm[RADIO_SQUELCH_MAX - 1]));
}
