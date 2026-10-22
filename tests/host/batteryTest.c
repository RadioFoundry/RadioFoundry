// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The battery average and charge estimate (drivers/power/battery): the first reading fills the average, readings come
// at most every BATTERY_AVG_MS, a step moves the average by 1/8 per reading, and the percent's ends and middle
#include <stdio.h>
#include "power/battery/battery.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t reading = 8000, reads;

static uint32_t read_mv(void)
{
	reads++;
	return reading;
}

int main(void)
{
	battery_avg_t a = { 0 };
	CHECK(battery_avg_mv(&a, 0, read_mv) == 8000 && reads == 1);
	reading = 7200;
	CHECK(battery_avg_mv(&a, BATTERY_AVG_MS - 1, read_mv) == 8000 && reads == 1);	// too soon: no new reading
	CHECK(battery_avg_mv(&a, BATTERY_AVG_MS, read_mv) == 7900 && reads == 2);	// 8000 - 800 / 8
	uint32_t t = BATTERY_AVG_MS;
	for (int i = 0; i < 60; i++) {
		t += BATTERY_AVG_MS;
		battery_avg_mv(&a, t, read_mv);
	}
	CHECK(battery_avg_mv(&a, t, read_mv) >= 7200 && battery_avg_mv(&a, t, read_mv) <= 7208);
	// the millisecond counter wraps between two readings
	battery_avg_t w = { 0 };
	reading = 7000;
	battery_avg_mv(&w, 0xFFFFFF00u, read_mv);
	reading = 7800;
	CHECK(battery_avg_mv(&w, 0xFFFFFF00u + BATTERY_AVG_MS, read_mv) == 7100);

	CHECK(battery_linear_pct(6000, 6400, 8200) == 0 && battery_linear_pct(6400, 6400, 8200) == 0);
	CHECK(battery_linear_pct(7300, 6400, 8200) == 50 && battery_linear_pct(8200, 6400, 8200) == 100);
	CHECK(battery_linear_pct(9000, 6400, 8200) == 100);
	if (failures) {
		printf("batteryTest: %d failures\n", failures);
		return 1;
	}
	printf("batteryTest: all passed\n");
	return 0;
}
