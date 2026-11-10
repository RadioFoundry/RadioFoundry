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

// A table of four levels at one frequency, none elsewhere
static uint16_t level_drive(uint32_t hz, uint8_t level)
{
	static const uint16_t d[4] = { 955, 1249, 2101, 2615 };
	return hz == 435000000u && level < 4 ? d[level] : 0;
}

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
	// A drive under the dead zone still gets the knee's boost, and holds at its own drive
	k = pa_keyup_drives(&dflt, 500, 2615, 4095);
	CHECK(k.boost == 3138 && k.hold_us == 1500 - 589 && k.hold == 500);
	// The hold never under the dead zone: 80 % of 800 is 640, held at 681
	k = pa_keyup_drives(&dflt, 800, 2615, 4095);
	CHECK(k.boost == 3138 && k.hold == 681 && k.drive == 800);

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
	pa_keyup_cfg_t used = { .knee = 7 };
	CHECK(pa_keyup_plan(955, 2615, &used).boost == 0 && used.knee == 7);
	pa_keyup_init(&dflt, 1580, 4095);
	CHECK(pa_keyup_cfg() != NULL && pa_keyup_cfg()->knee == 1450);
	k = pa_keyup_plan(955, 2615, &used);
	CHECK(k.boost == 3138 && k.hold == 764 && k.hold_us == 843 && used.knee == 1450 && used.boost_us == 1500);
	CHECK(pa_keyup_last().hold_us == 843);
	c = dflt;
	c.knee = 1229;
	CHECK(pa_keyup_set(&c) && pa_keyup_cfg()->knee == 1229);
	CHECK(pa_keyup_plan(680, 2615, NULL).hold_us == 1000);
	CHECK(pa_keyup_plan(955, 2615, &used).boost == 3138 && used.knee == 1229);
	c.boost_us = 2000;
	CHECK(!pa_keyup_set(&c) && pa_keyup_cfg()->boost_us == 1500);
	CHECK(pa_keyup_set(NULL) && pa_keyup_cfg()->knee == 1450 && pa_keyup_cfg()->boost_pct == 120);

	// The top drive: the largest level's at the frequency, 0 with none
	CHECK(pa_keyup_top(level_drive, 435000000u, 4) == 2615 && pa_keyup_top(level_drive, 435000000u, 2) == 1249);
	CHECK(pa_keyup_top(level_drive, 0u, 4) == 0 && pa_keyup_top(NULL, 435000000u, 4) == 0);

	// The points earliest first; a tie in the enum's order (the board's switch, the boost, the PA, the hold, the drive)
	pa_keyup_point_t pt[PA_KEYUP_POINTS];
	k = pa_keyup_drives(&dflt, 955, 2615, 4095);
	uint8_t n = pa_keyup_points(&dflt, &k, true, 1800, pt);
	CHECK(n == 5);
	CHECK(pt[0].at == PA_KEYUP_AT_BOARD && pt[0].ahead_us == 1800);
	CHECK(pt[1].at == PA_KEYUP_AT_BOOST && pt[2].at == PA_KEYUP_AT_PA && pt[1].ahead_us == 1500 && pt[2].ahead_us == 1500);
	CHECK(pt[3].at == PA_KEYUP_AT_HOLD && pt[3].ahead_us == 843 && pt[4].at == PA_KEYUP_AT_DRIVE && pt[4].ahead_us == 500);
	n = pa_keyup_points(&dflt, &k, true, 1500, pt);		// a tie with the boost: the switch first
	CHECK(n == 5 && pt[0].at == PA_KEYUP_AT_BOARD && pt[1].at == PA_KEYUP_AT_BOOST);
	c = dflt;
	c.pa_us = 600;						// the PA later than the hold
	n = pa_keyup_points(&c, &k, true, 0, pt);
	CHECK(n == 4 && pt[0].at == PA_KEYUP_AT_BOOST && pt[1].at == PA_KEYUP_AT_HOLD && pt[2].at == PA_KEYUP_AT_PA &&
	      pt[3].at == PA_KEYUP_AT_DRIVE);
	// No boost (disarmed, or none planned): the switch and the PA alone
	n = pa_keyup_points(&dflt, &k, false, 1800, pt);
	CHECK(n == 2 && pt[0].at == PA_KEYUP_AT_BOARD && pt[1].at == PA_KEYUP_AT_PA);
	pa_keyup_t none = pa_keyup_drives(&off, 955, 2615, 4095);
	n = pa_keyup_points(&off, &none, true, 0, pt);
	CHECK(n == 1 && pt[0].at == PA_KEYUP_AT_PA && pt[0].ahead_us == 300);

	// Steps from an edge 7 ms ahead: points at one time in one step, the PA's own drive only without a boost
	pa_keyup_steps_t st;
	pa_keyup_steps(&dflt, &k, true, 7000, &st);
	CHECK(st.n == 3);
	CHECK(st.s[0].at_us == 5500 && st.s[0].set_drive && st.s[0].drive == 3138 && st.s[0].pa_on);
	CHECK(st.s[1].at_us == 6157 && st.s[1].drive == 764 && !st.s[1].pa_on);
	CHECK(st.s[2].at_us == 6500 && st.s[2].drive == 955 && !st.s[2].pa_on);
	pa_keyup_steps(&dflt, &k, false, 7000, &st);
	CHECK(st.n == 1 && st.s[0].at_us == 5500 && st.s[0].drive == 955 && st.s[0].pa_on);
	c = dflt;
	c.drive_us = 843;					// the hold's point and the drive's: the drive wins
	k = pa_keyup_drives(&c, 955, 2615, 4095);
	pa_keyup_steps(&c, &k, true, 7000, &st);
	CHECK(st.n == 2 && st.s[1].drive == 955);
	pa_keyup_steps(&off, &none, true, 1000, &st);		// a point before the edge: as soon as it can
	CHECK(st.n == 1 && st.s[0].at_us == 700);
	pa_keyup_steps(&off, &none, true, 200, &st);
	CHECK(st.n == 1 && st.s[0].at_us == 1);

	if (failures == 0) {
		printf("paKeyupTest: all passed\n");
	}
	return failures != 0;
}
