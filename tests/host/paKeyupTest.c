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
	.boost_us = 1500, .pa_us = 1500, .drive_us = 500, .late_pct = 100,
};

int main(void)
{
	// A UHF table with a top drive of 2615: the boost is 3138 at every step, and lasts
	// (1450000 + 600 * (d - 680)) / (3138 - 680) us, so a higher step's boost is longer. Every boost ends where the top
	// step's does (here at the drive point), so a lower step's starts later by what it is shorter
	static const uint16_t d[5] = { 955, 1249, 1613, 2101, 2615 };
	static const uint16_t len[5] = { 657, 728, 817, 936, 1000 };
	static const uint16_t hold[5] = { 764, 999, 1290, 1680, 2092 };
	for (int i = 0; i < 5; i++) {
		pa_keyup_t k = pa_keyup_drives(&dflt, d[i], 2615, 0, 4095);
		CHECK(k.boost == 3138 && k.hold == hold[i] && k.drive == d[i] && k.hold_us == 500 && k.late_us == 1000 - len[i]);
	}

	// late_pct: how much of that goes to a later start. 0 starts every step at boost_us
	pa_keyup_cfg_t lp = dflt;
	lp.late_pct = 0;
	pa_keyup_t k0 = pa_keyup_drives(&lp, 955, 2615, 0, 4095);
	CHECK(k0.late_us == 0 && k0.hold_us == 1500 - 657);
	lp.late_pct = 50;
	k0 = pa_keyup_drives(&lp, 955, 2615, 0, 4095);
	CHECK(k0.late_us == 171 && k0.hold_us == 1500 - 171 - 657);
	lp.late_pct = 101;
	CHECK(!pa_keyup_valid(&lp, 1580));

	// The boost never over the limit, and it ends by the point the drive goes to the step's: a short lead cuts it
	pa_keyup_t k = pa_keyup_drives(&dflt, 2615, 2615, 0, 2700);
	CHECK(k.boost == 2700 && k.hold_us == 500);
	pa_keyup_cfg_t c = dflt;
	c.boost_us = 900;
	c.pa_us = 900;
	k = pa_keyup_drives(&c, 955, 2615, 0, 4095);
	CHECK(k.boost == 3138 && k.hold_us == 500);
	// A drive under the dead zone still gets the knee's boost, and holds at its own drive
	k = pa_keyup_drives(&dflt, 500, 2615, 0, 4095);
	CHECK(k.boost == 3138 && k.hold_us == 500 && k.late_us == 1000 - 589 && k.hold == 500);
	// The hold never under the dead zone: 80 % of 800 is 640, held at 681
	k = pa_keyup_drives(&dflt, 800, 2615, 0, 4095);
	CHECK(k.boost == 3138 && k.hold == 681 && k.drive == 800);

	// No boost: no table drive, no constants, boost_us 0, a boost inside the dead zone or under the step's drive (a
	// calibration drive over the table's top): the PA keyed at pa_us at the drive, as before
	pa_keyup_cfg_t off = dflt;
	off.boost_us = 0;
	k = pa_keyup_drives(&off, 955, 2615, 0, 4095);
	CHECK(k.boost == 0 && k.hold == 0 && k.drive == 955);
	k = pa_keyup_drives(NULL, 955, 2615, 0, 4095);
	CHECK(k.boost == 0 && k.drive == 955);
	k = pa_keyup_drives(&dflt, 0, 2615, 0, 4095);
	CHECK(k.boost == 0 && k.drive == 0);
	k = pa_keyup_drives(&dflt, 300, 500, 0, 4095);
	CHECK(k.boost == 0 && k.drive == 300);
	k = pa_keyup_drives(&dflt, 3500, 2615, 0, 4095);
	CHECK(k.boost == 0 && k.drive == 3500);

	// A calibration floor (the drive where the output reaches zero): dead counts from it, the hold stays above it,
	// and a drive under it gets no boost
	pa_keyup_cfg_t fl = { .dead = 50, .knee = 100, .slope_us = 0, .boost_pct = 100, .hold_pct = 50,
			      .boost_us = 1500, .pa_us = 1500, .drive_us = 500 };
	k = pa_keyup_drives(&fl, 600, 2000, 440, 2300);
	CHECK(k.boost == 2000 && k.hold == 520 && k.hold_us == 1500 - 66 && k.late_us == 0);
	fl.hold_pct = 0;	// held just above the floor's dead zone
	CHECK(pa_keyup_drives(&fl, 600, 2000, 440, 2300).hold == 491);
	CHECK(pa_keyup_drives(&fl, 400, 2000, 440, 2300).boost == 0);

	// The points in time order, the enum's order on a tie; one point without a boost
	pa_keyup_point_t pt[PA_KEYUP_POINTS];
	k = pa_keyup_drives(&dflt, 955, 2615, 0, 4095);
	CHECK(pa_keyup_points(&dflt, &k, pt) == 4);
	// The boost and the PA with it start 343 us late: the step's boost is that much shorter than the top step's
	CHECK(pt[0].kind == PA_KEYUP_PT_BOOST && pt[0].us == 1157 && pt[1].kind == PA_KEYUP_PT_PA && pt[1].us == 1157);
	CHECK(pt[2].kind == PA_KEYUP_PT_HOLD && pt[2].us == 500 && pt[3].kind == PA_KEYUP_PT_DRIVE && pt[3].us == 500);
	c = dflt;
	c.pa_us = 1200;		// a PA point of its own stays, until the boost starts after it
	CHECK(pa_keyup_points(&c, &k, pt) == 4 && pt[0].kind == PA_KEYUP_PT_BOOST && pt[1].kind == PA_KEYUP_PT_PA && pt[1].us == 1157);
	pa_keyup_t top = pa_keyup_drives(&dflt, 2615, 2615, 0, 4095);
	CHECK(top.late_us == 0 && pa_keyup_points(&c, &top, pt) == 4 && pt[0].us == 1500 && pt[1].us == 1200);
	c = dflt;
	c.pa_us = 300;
	CHECK(pa_keyup_points(&c, &k, pt) == 4 && pt[3].kind == PA_KEYUP_PT_PA && pt[3].us == 300);
	CHECK(pt[0].kind == PA_KEYUP_PT_BOOST && pt[1].kind == PA_KEYUP_PT_HOLD && pt[2].kind == PA_KEYUP_PT_DRIVE);
	k = pa_keyup_drives(&off, 955, 2615, 0, 4095);
	CHECK(pa_keyup_points(&off, &k, pt) == 1 && pt[0].kind == PA_KEYUP_PT_PA && pt[0].us == off.pa_us);

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
	CHECK(pa_keyup_plan(955, 2615, 0, &used).boost == 0 && used.knee == 7);
	pa_keyup_init(&dflt, 1580, 4095);
	CHECK(pa_keyup_cfg() != NULL && pa_keyup_cfg()->knee == 1450);
	k = pa_keyup_plan(955, 2615, 0, &used);
	CHECK(k.boost == 3138 && k.hold == 764 && k.hold_us == 500 && k.late_us == 343 && used.knee == 1450 && used.boost_us == 1500);
	// A band's constants replace the board's, but not ones the debug link set, until it gives them back
	static const pa_keyup_cfg_t band2 = { .dead = 0, .knee = 500, .slope_us = 780, .boost_pct = 120, .hold_pct = 100,
					      .boost_us = 1500, .pa_us = 1500, .drive_us = 300 };
	static const pa_keyup_cfg_t late = { .boost_pct = 100, .hold_pct = 100, .boost_us = 1600, .pa_us = 1600 };
	pa_keyup_select(&late);		// past the limit: ignored
	CHECK(pa_keyup_cfg()->knee == 1450);
	pa_keyup_select(&band2);
	CHECK(pa_keyup_cfg()->knee == 500);
	pa_keyup_cfg_t dbg = band2;
	dbg.knee = 700;
	CHECK(pa_keyup_set(&dbg));
	pa_keyup_select(&dflt);
	CHECK(pa_keyup_cfg()->knee == 700);
	CHECK(pa_keyup_set(NULL) && pa_keyup_cfg()->knee == 1450);
	pa_keyup_select(&band2);
	CHECK(pa_keyup_cfg()->knee == 500);
	pa_keyup_select(&dflt);
	CHECK(pa_keyup_last().hold_us == 500);
	c = dflt;
	c.knee = 1229;
	CHECK(pa_keyup_set(&c) && pa_keyup_cfg()->knee == 1229);
	CHECK(pa_keyup_plan(680, 2615, 0, NULL).hold_us == 528);	// boosts of 500 and (the top step's) 972 us
	CHECK(pa_keyup_plan(955, 2615, 0, &used).boost == 3138 && used.knee == 1229);
	c.boost_us = 2000;
	CHECK(!pa_keyup_set(&c) && pa_keyup_cfg()->boost_us == 1500);
	CHECK(pa_keyup_set(NULL) && pa_keyup_cfg()->knee == 1450 && pa_keyup_cfg()->boost_pct == 120);

	if (failures == 0) {
		printf("paKeyupTest: all passed\n");
	}
	return failures != 0;
}
