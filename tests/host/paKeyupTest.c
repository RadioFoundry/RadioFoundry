// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR PA key-up (core/radio/pa_keyup.c, docs/spec/pa-keyup.md): the boost, its length and the hold from the cal
// table's drives, their limits, and the checks on the constants the debug link may set
#include <stdio.h>
#include "radio/pa_keyup.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const pa_keyup_cfg_t dflt = {
	.dead = 680, .knee = 1450, .slope_us = 600, .boost_pct = 120, .hold_pct = 80,
	.boost_us = 1500, .pa_us = 1500, .drive_us = 500,
};

int main(void)
{
	// A UHF table with a top drive of 2615: the boost is 3138 at every step, and lasts
	// (1450000 + 600 * (d - 680)) / (3138 - 680) us, so a higher step's boost is longer
	static const uint16_t d[5] = { 955, 1249, 1613, 2101, 2615 };
	static const uint16_t len[5] = { 657, 728, 817, 936, 1000 };
	static const uint16_t hold[5] = { 764, 999, 1290, 1680, 2092 };
	for (int i = 0; i < 5; i++) {
		pa_keyup_t k = pa_keyup_drives(&dflt, d[i], 2615, 4095);
		CHECK(k.boost == 3138 && k.hold == hold[i] && k.drive == d[i] && k.hold_us == 1500 - len[i]);
	}

	// The boost never over the limit, and it ends by the point the drive goes to the step's: a short lead cuts it
	pa_keyup_t k = pa_keyup_drives(&dflt, 2615, 2615, 2700);
	CHECK(k.boost == 2700 && k.hold_us == 500);
	pa_keyup_cfg_t c = dflt;
	c.boost_us = 900;
	c.pa_us = 900;
	k = pa_keyup_drives(&c, 955, 2615, 4095);
	CHECK(k.boost == 3138 && k.hold_us == 500);
	// A drive under the dead zone still gets the knee's boost
	k = pa_keyup_drives(&dflt, 500, 2615, 4095);
	CHECK(k.boost == 3138 && k.hold_us == 1500 - 589 && k.hold == 400);

	// No boost: no table drive, no constants, boost_us 0, a boost inside the dead zone or under the step's drive (a
	// calibration drive over the table's top): the PA keyed at pa_us at the drive, as before
	pa_keyup_cfg_t off = dflt;
	off.boost_us = 0;
	k = pa_keyup_drives(&off, 955, 2615, 4095);
	CHECK(k.boost == 0 && k.hold == 0 && k.drive == 955);
	k = pa_keyup_drives(NULL, 955, 2615, 4095);
	CHECK(k.boost == 0 && k.drive == 955);
	k = pa_keyup_drives(&dflt, 0, 2615, 4095);
	CHECK(k.boost == 0 && k.drive == 0);
	k = pa_keyup_drives(&dflt, 300, 500, 4095);
	CHECK(k.boost == 0 && k.drive == 300);
	k = pa_keyup_drives(&dflt, 3500, 2615, 4095);
	CHECK(k.boost == 0 && k.drive == 3500);

	// The checks on the constants
	CHECK(pa_keyup_valid(&dflt, 1580));
	CHECK(!pa_keyup_valid(&dflt, 1400));		// the boost earlier than the board allows
	off.pa_us = 300;
	CHECK(pa_keyup_valid(&off, 1400));		// no boost: only the PA's point counts
	c = dflt;
	c.boost_pct = PA_KEYUP_BOOST_MAX_PCT + 1u;
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.boost_pct = 49;
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.hold_pct = 101;
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.drive_us = 1600;				// the drive's point before the boost
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.pa_us = 1550;					// the PA before the boost
	CHECK(!pa_keyup_valid(&c, 1580));
	CHECK(!pa_keyup_valid(NULL, 1580));

	// The board's copy: nothing before init, the board's constants after, a set checked whole, NULL restores
	CHECK(pa_keyup_cfg() == NULL && !pa_keyup_set(&dflt));
	CHECK(pa_keyup_plan(955, 2615).boost == 0);
	pa_keyup_init(&dflt, 1580, 4095);
	CHECK(pa_keyup_cfg() != NULL && pa_keyup_cfg()->knee == 1450);
	k = pa_keyup_plan(955, 2615);
	CHECK(k.boost == 3138 && k.hold == 764 && k.hold_us == 843);
	CHECK(pa_keyup_last().hold_us == 843);
	c = dflt;
	c.knee = 1229;
	CHECK(pa_keyup_set(&c) && pa_keyup_cfg()->knee == 1229);
	CHECK(pa_keyup_plan(680, 2615).hold_us == 1000);
	c.boost_us = 2000;
	CHECK(!pa_keyup_set(&c) && pa_keyup_cfg()->boost_us == 1500);
	CHECK(pa_keyup_set(NULL) && pa_keyup_cfg()->knee == 1450 && pa_keyup_cfg()->boost_pct == 120);

	if (failures == 0) {
		printf("paKeyupTest: all passed\n");
	}
	return failures != 0;
}
