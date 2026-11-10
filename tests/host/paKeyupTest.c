// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR PA key-up (core/radio/pa_keyup.c, docs/spec/pa-keyup.md): the boost and hold from the cal table's drives,
// their limits, and the checks on the constants the debug link may set
#include <stdio.h>
#include "radio/pa_keyup.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const pa_keyup_cfg_t dflt = {
	.blend_pct = 55, .top_pct = 111, .hold_pct = 80, .boost_us = 1500, .hold_us = 500, .pa_us = 150,
};

int main(void)
{
	// The values measured on the SDR bench: a UHF table of 955/1180/1400/2003/2615
	static const uint16_t d[5] = { 955, 1180, 1400, 2003, 2615 };
	static const uint16_t boost[5] = { 2025, 2127, 2226, 2497, 2772 };
	static const uint16_t hold[5] = { 955, 955, 1120, 1602, 2092 };
	for (int i = 0; i < 5; i++) {
		pa_keyup_t k = pa_keyup_drives(&dflt, d[i], 955, 2615, 4095);
		CHECK(k.boost == boost[i] && k.hold == hold[i] && k.drive == d[i]);
	}

	// The boost never under the step's drive (a calibration drive over the table's top), never over the limit; the
	// hold never over the step's drive, never under the lowest step's
	pa_keyup_t k = pa_keyup_drives(&dflt, 3500, 955, 2615, 4095);
	CHECK(k.boost == 3500 && k.hold == 2800);
	k = pa_keyup_drives(&dflt, 2615, 955, 2615, 2700);
	CHECK(k.boost == 2700);
	k = pa_keyup_drives(&dflt, 500, 955, 2615, 4095);
	CHECK(k.hold == 500 && k.boost > 500);

	// No boost: no table drive, no constants, or boost_us 0 (the PA keyed at pa_us at the drive, as before)
	pa_keyup_cfg_t off = dflt;
	off.boost_us = 0;
	k = pa_keyup_drives(&off, 955, 955, 2615, 4095);
	CHECK(k.boost == 0 && k.hold == 0 && k.drive == 955);
	k = pa_keyup_drives(NULL, 955, 955, 2615, 4095);
	CHECK(k.boost == 0 && k.drive == 955);
	k = pa_keyup_drives(&dflt, 0, 955, 2615, 4095);
	CHECK(k.boost == 0 && k.drive == 0);

	// No margin, no blend: the boost is the drive; the full blend reaches the target
	pa_keyup_cfg_t c = dflt;
	c.blend_pct = 0;
	CHECK(pa_keyup_drives(&c, 955, 955, 2615, 4095).boost == 955);
	c.blend_pct = 100;
	c.top_pct = 100;
	CHECK(pa_keyup_drives(&c, 955, 955, 2615, 4095).boost == 2615);

	// The checks on the constants
	CHECK(pa_keyup_valid(&dflt, 1580));
	CHECK(!pa_keyup_valid(&dflt, 1400));		// the boost earlier than the board allows
	CHECK(pa_keyup_valid(&off, 1400));		// no boost: only the PA's point counts
	c = dflt;
	c.top_pct = PA_KEYUP_TOP_MAX_PCT + 1u;
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.top_pct = 49;
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.blend_pct = 101;
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.hold_pct = 101;
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.hold_us = 1600;				// the hold before the boost
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.pa_us = 600;					// the PA before the hold
	CHECK(!pa_keyup_valid(&c, 1580));
	CHECK(!pa_keyup_valid(NULL, 1580));

	// The board's copy: nothing before init, the board's constants after, a set checked whole, NULL restores
	CHECK(pa_keyup_cfg() == NULL && !pa_keyup_set(&dflt));
	CHECK(pa_keyup_plan(955, 955, 2615).boost == 0);
	pa_keyup_init(&dflt, 1580, 4095);
	CHECK(pa_keyup_cfg() != NULL && pa_keyup_cfg()->blend_pct == 55);
	k = pa_keyup_plan(955, 955, 2615);
	CHECK(k.boost == 2025 && k.hold == 955);
	CHECK(pa_keyup_last().boost == 2025);
	c = dflt;
	c.blend_pct = 50;
	c.top_pct = 100;
	CHECK(pa_keyup_set(&c) && pa_keyup_cfg()->blend_pct == 50);
	CHECK(pa_keyup_plan(955, 955, 2615).boost == 955 + (2615 - 955) / 2);
	c.boost_us = 2000;
	CHECK(!pa_keyup_set(&c) && pa_keyup_cfg()->boost_us == 1500);
	CHECK(pa_keyup_set(NULL) && pa_keyup_cfg()->blend_pct == 55 && pa_keyup_cfg()->top_pct == 111);

	if (failures == 0) {
		printf("paKeyupTest: all passed\n");
	}
	return failures != 0;
}
