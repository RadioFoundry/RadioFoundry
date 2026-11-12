// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The volume knob curve (drivers/power/knob): the ends, each measured point, linear between points, and a two-segment
// table (a break point at half the turn) giving what the plain two-segment formula gives, and the levels' hysteresis
// and ends
#include <stdio.h>
#include "power/knob/knob.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	// An audio taper knob measured at its quarters
	static const uint16_t q[5] = { 0, 59, 129, 671, 1010 };
	CHECK(knob_curve_pct(0, q, 5) == 0);
	CHECK(knob_curve_pct(-3, q, 5) == 0);
	CHECK(knob_curve_pct(59, q, 5) == 25);
	CHECK(knob_curve_pct(129, q, 5) == 50);
	CHECK(knob_curve_pct(671, q, 5) == 75);
	CHECK(knob_curve_pct(1010, q, 5) == 100);
	CHECK(knob_curve_pct(1023, q, 5) == 100);
	CHECK(knob_curve_pct(400, q, 5) == 50 + (400 - 129) * 25 / (671 - 129));
	CHECK(knob_curve_pct(30, q, 5) == 30 * 25 / 59);
	int32_t last = 0;
	for (int32_t raw = 0; raw <= 1023; raw++) {
		int32_t p = knob_curve_pct(raw, q, 5);
		CHECK(p >= last && p <= 100);	// never goes down as the knob turns up
		last = p;
	}

	// min, break, max: 0..50 % below the break, 50..100 % above it
	static const uint16_t b[3] = { 120, 1500, 3900 };
	for (int32_t raw = 121; raw <= 3900; raw += 7) {
		int32_t want = raw < 1500 ? (raw - 120) * 50 / (1500 - 120) : 50 + (raw - 1500) * 50 / (3900 - 1500);
		CHECK(knob_curve_pct(raw, b, 3) == want);
	}

	// Levels of 5 % with 1 % of hysteresis: the stops reach the end levels from the next one, a boundary holds
	CHECK(knob_level(100, 19, 21, 5, 1) == 20);
	CHECK(knob_level(99, 19, 21, 5, 1) == 19);
	CHECK(knob_level(0, 1, 21, 5, 1) == 0);
	CHECK(knob_level(4, 1, 21, 5, 1) == 1);	// within the hysteresis below 5
	CHECK(knob_level(3, 1, 21, 5, 1) == 0);
	CHECK(knob_level(10, 1, 21, 5, 1) == 1);	// within the hysteresis above 10
	CHECK(knob_level(11, 1, 21, 5, 1) == 2);
	CHECK(knob_level(52, 0, 21, 5, 1) == 10);
	uint8_t lv = 0;
	for (int32_t raw = 0; raw <= 1010; raw++) {
		lv = knob_level(knob_curve_pct(raw, q, 5), lv, 21, 5, 1);
	}
	CHECK(lv == 20);	// turned all the way up: the top level
	for (int32_t raw = 1010; raw >= 0; raw--) {
		lv = knob_level(knob_curve_pct(raw, q, 5), lv, 21, 5, 1);
	}
	CHECK(lv == 0);

	if (failures == 0) {
		printf("knobTest: passed (0 failures)\n");
	}
	return failures != 0;
}
