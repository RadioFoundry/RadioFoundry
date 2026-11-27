// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DB25-D's squelch reading (boards/db25d/squelch_db25d.c): the core's one squelch opens at each level exactly at
// the calibration page's threshold, and the FD6818's own noise and glitch limits hold it shut
#include <stdio.h>
#include "squelch_db25d.h"
#include "radio/radio_sm.h"

_Static_assert(DB25D_SQL_OPEN_L1 == RADIO_SQL_OPEN_L1, "the board's copy of the core's level 1 line");
_Static_assert(DB25D_SQL_STEP == RADIO_SQL_DB_PER_STEP, "the board's copy of the core's step");
_Static_assert(DB25D_SQL_LEVELS == RADIO_SQUELCH_MAX, "one threshold per core level");

static int failures;

// The core's line for a level (radio_sm.c radio_sm_squelch_open_below), from its constants
static int open_below(int level)
{
	return RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP;
}

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void levels_open_at_thresholds(const int16_t open[DB25D_SQL_LEVELS])
{
	for (int16_t rssi = -160; rssi <= 0; rssi++) {
		uint8_t n = db25d_sql_noise(open, rssi);
		for (uint8_t lvl = 1; lvl <= DB25D_SQL_LEVELS; lvl++) {
			bool core_open = n < open_below(lvl);
			CHECK(core_open == (rssi >= open[lvl - 1]));
		}
		// Weaker never reads less noise
		if (rssi > -160) {
			CHECK(db25d_sql_noise(open, (int16_t)(rssi - 1)) >= n);
		}
	}
}

int main(void)
{
	// Thresholds a few dB apart, as a page holds them
	const int16_t spread[DB25D_SQL_LEVELS] = { -124, -122, -120, -117, -114, -111, -108, -104, -100 };
	levels_open_at_thresholds(spread);
	// Equal neighbors and wide gaps
	const int16_t uneven[DB25D_SQL_LEVELS] = { -130, -130, -128, -110, -110, -109, -90, -89, -60 };
	levels_open_at_thresholds(uneven);

	// An empty channel well under level 1 reads far over every line; a strong carrier far under level 9's
	CHECK(db25d_sql_noise(spread, -150) >= open_below(1) + RADIO_SQL_FAST_CLOSE_DB);
	CHECK(db25d_sql_noise(spread, -40) == 0);
	CHECK(db25d_sql_noise(spread, -160) == 64 + 36);

	// FD6818 limits: glitch over 140 narrow / 120 wide is no carrier
	CHECK(db25d_sql_fd6818_carrier(30, 140, -110, false));
	CHECK(!db25d_sql_fd6818_carrier(30, 141, -110, false));
	CHECK(!db25d_sql_fd6818_carrier(30, 121, -110, true));
	// Noise limit: 45 on a weak signal (82.5 - 0.38 x 100 = 44.5 at raw 100, -110 dBm, so 44), 41 narrow / 39 wide on a
	// strong one
	CHECK(db25d_sql_fd6818_carrier(44, 0, -110, false));
	CHECK(!db25d_sql_fd6818_carrier(45, 0, -110, false));
	CHECK(db25d_sql_fd6818_carrier(45, 0, -140, false));
	CHECK(!db25d_sql_fd6818_carrier(46, 0, -140, false));
	CHECK(db25d_sql_fd6818_carrier(41, 0, -50, false));
	CHECK(!db25d_sql_fd6818_carrier(42, 0, -50, false));
	CHECK(db25d_sql_fd6818_carrier(39, 0, -50, true));
	CHECK(!db25d_sql_fd6818_carrier(40, 0, -50, true));

	if (failures == 0) {
		printf("db25dSqlTest: all passed\n");
	}
	return failures != 0;
}
