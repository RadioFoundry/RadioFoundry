// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A squelch reading from signal strength (core/radio/sql_rssi.c): the core's one squelch opens at each level exactly
// at the calibration's threshold and closes where the level below opens
#include <stdio.h>
#include <string.h>
#include "radio/sql_rssi.h"

static int failures;

// The core's line for a level (radio_sm.c radio_sm_squelch_open_below), from its constants
static uint8_t open_below(uint8_t level)
{
	return (uint8_t)(RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP);
}

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void levels_open_at_thresholds(const int16_t open[RADIO_SQUELCH_MAX])
{
	for (int16_t rssi = -160; rssi <= 0; rssi++) {
		uint8_t n = radio_sql_rssi_noise(open, rssi);
		for (uint8_t lvl = 1; lvl <= RADIO_SQUELCH_MAX; lvl++) {
			uint8_t line = open_below(lvl);
			CHECK((n < line) == (rssi >= open[lvl - 1]));
			// The core holds a level open while the reading is under its line plus RADIO_SQL_HYST_DB: down to the
			// level below's threshold; level 1 closes 3 dB under its own
			int16_t close_below = lvl == 1 ? (int16_t)(open[0] - (RADIO_SQL_HYST_DB - 1)) : open[lvl - 2];
			CHECK((n < line + RADIO_SQL_HYST_DB) == (rssi >= close_below));
			// No carrier closes every level
			CHECK(RADIO_SQL_NO_CARRIER >= line + RADIO_SQL_HYST_DB);
		}
		// Weaker never reads less noise
		if (rssi > -160) {
			CHECK(radio_sql_rssi_noise(open, (int16_t)(rssi - 1)) >= n);
		}
	}
}

int main(void)
{
	// 1 dB a level, as calibration pages hold them
	int16_t page[RADIO_SQUELCH_MAX] = { -98, -97, -96, -95, -94, -93, -92, -91, -90 };
	CHECK(radio_sql_rssi_levels(page));
	CHECK(page[0] == -98 && page[8] == -90);
	levels_open_at_thresholds(page);
	// A few dB apart, and uneven gaps
	int16_t spread[RADIO_SQUELCH_MAX] = { -124, -122, -120, -117, -114, -111, -108, -104, -100 };
	CHECK(radio_sql_rssi_levels(spread));
	levels_open_at_thresholds(spread);
	int16_t uneven[RADIO_SQUELCH_MAX] = { -130, -129, -128, -110, -109, -108, -90, -89, -60 };
	CHECK(radio_sql_rssi_levels(uneven));
	levels_open_at_thresholds(uneven);

	// Equal entries: moved 1 dB apart, so each level keeps a close point under its open point
	int16_t flat[RADIO_SQUELCH_MAX] = { -98, -98, -98, -95, -95, -94, -94, -94, -90 };
	const int16_t flat_want[RADIO_SQUELCH_MAX] = { -98, -97, -96, -95, -94, -93, -92, -91, -90 };
	CHECK(radio_sql_rssi_levels(flat));
	CHECK(memcmp(flat, flat_want, sizeof(flat)) == 0);
	levels_open_at_thresholds(flat);
	// A run of equal entries is judged on the values read, not the moved ones
	int16_t run[RADIO_SQUELCH_MAX] = { -33, -33, -33, -33, -33, -33, -33, -33, -33 };
	CHECK(radio_sql_rssi_levels(run));
	CHECK(run[0] == -33 && run[8] == -25);
	levels_open_at_thresholds(run);
	// A tighter level that opens on less signal: not thresholds
	int16_t bad[RADIO_SQUELCH_MAX] = { -98, -97, -96, -95, -99, -93, -92, -91, -90 };
	CHECK(!radio_sql_rssi_levels(bad));

	// An empty channel well under level 1 reads far over every line; a strong carrier far under level 9's
	CHECK(radio_sql_rssi_noise(spread, -150) >= open_below(1) + RADIO_SQL_FAST_CLOSE_DB);
	CHECK(radio_sql_rssi_noise(spread, -40) == 0);
	CHECK(radio_sql_rssi_noise(spread, -160) == RADIO_SQL_OPEN_L1 + 36);

	if (failures == 0) {
		printf("sqlRssiTest: all passed\n");
	}
	return failures != 0;
}
