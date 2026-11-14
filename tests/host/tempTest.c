// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The temperature framework (core/app/temp.c): curves rising and falling, the ends, rounding, and a board's sensors
// read through temp_read
#include <stdio.h>
#include <stddef.h>
#include "app/temp.h"

static int checks, failures;
#define CHECK(c, ...)                                                                                                  \
	do {                                                                                                           \
		checks++;                                                                                              \
		if (!(c)) {                                                                                            \
			failures++;                                                                                    \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                    \
			printf(__VA_ARGS__);                                                                           \
			printf("\n");                                                                                  \
		}                                                                                                      \
	} while (0)

static uint16_t mv_now;
static uint16_t read_mv(void) { return mv_now; }
static bool read_dc(int16_t *dc) { *dc = 423; return true; }
static bool no_dc(int16_t *dc) { (void)dc; return false; }
static bool fan(void) { return true; }

int main(void)
{
	int16_t dc = 0;
	// A rising sensor (higher mV = hotter), three points
	static const temp_point_t up[] = { { 500, 200 }, { 700, 500 }, { 800, 700 } };
	CHECK(temp_curve_dc(up, 3, 500, &dc) && dc == 200, "the first point (%d)", dc);
	CHECK(temp_curve_dc(up, 3, 600, &dc) && dc == 350, "halfway along a segment (%d)", dc);
	CHECK(temp_curve_dc(up, 3, 750, &dc) && dc == 600, "the second segment (%d)", dc);
	CHECK(temp_curve_dc(up, 3, 100, &dc) && dc == 200, "below the curve: the first point (%d)", dc);
	CHECK(temp_curve_dc(up, 3, 3300, &dc) && dc == 700, "above the curve: the last point (%d)", dc);
	CHECK(temp_curve_dc(up, 3, 501, &dc) && dc == 202, "rounded to the nearest 0.1 degree (%d)", dc);
	// A falling one (an NTC on the low side of a divider), with temperatures below zero
	static const temp_point_t down[] = { { 2800, -200 }, { 1650, 250 }, { 400, 1000 } };
	CHECK(temp_curve_dc(down, 3, 1650, &dc) && dc == 250, "a falling curve's middle point (%d)", dc);
	CHECK(temp_curve_dc(down, 3, 2225, &dc) && dc == 25, "between the first two (%d)", dc);
	CHECK(temp_curve_dc(down, 3, 3000, &dc) && dc == -200, "past the cold end (%d)", dc);
	CHECK(temp_curve_dc(down, 3, 100, &dc) && dc == 1000, "past the hot end (%d)", dc);
	CHECK(temp_curve_dc(down, 3, 2799, &dc) && dc == -200, "rounding near a negative point (%d)", dc);
	// One point, none, a flat segment
	static const temp_point_t one[] = { { 900, 300 } };
	CHECK(temp_curve_dc(one, 1, 10, &dc) && dc == 300 && temp_curve_dc(one, 1, 4000, &dc) && dc == 300, "one point");
	CHECK(!temp_curve_dc(NULL, 0, 600, &dc) && !temp_curve_dc(up, 0, 600, &dc), "no curve, no temperature");
	static const temp_point_t flat[] = { { 500, 100 }, { 500, 300 }, { 600, 500 } };
	CHECK(temp_curve_dc(flat, 3, 550, &dc) && dc == 400, "past a flat segment (%d)", dc);

	// A board's sensors: mV with a curve, mV without one, degrees only, a digital sensor with no reading
	static const hal_temp_sensor_t s[] = {
		{ .kind = TEMP_KIND_PA, .read_mv = read_mv, .curve = up, .curve_len = 3 },
		{ .kind = TEMP_KIND_BOARD, .read_mv = read_mv },
		{ .kind = TEMP_KIND_MCU, .read_dc = read_dc },
		{ .kind = TEMP_KIND_BATTERY, .read_dc = no_dc },
	};
	const hal_temp_t t = { .sensors = s, .count = 4, .fan_on = fan };
	temp_reading_t r;
	mv_now = 700;
	CHECK(temp_read(&t, 0, &r) && r.kind == TEMP_KIND_PA && r.has_mv && r.mv == 700 && r.has_dc && r.dc == 500,
	      "a PA sensor with a curve");
	CHECK(temp_read(&t, 1, &r) && r.has_mv && r.mv == 700 && !r.has_dc, "a sensor whose curve isn't known: mV only");
	CHECK(temp_read(&t, 2, &r) && !r.has_mv && r.has_dc && r.dc == 423, "a sensor that gives degrees");
	CHECK(temp_read(&t, 3, &r) && !r.has_mv && !r.has_dc, "no reading now");
	CHECK(!temp_read(&t, 4, &r) && !temp_read(NULL, 0, &r), "past the last sensor, or no sensors");
	CHECK(t.fan_on(), "the fan hook");

	printf("tempTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
