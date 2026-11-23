// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When a GPS module is powered (drivers/gps/gps_rx/gps_power.c), stepped the way the platform GPS tasks
// (platform/stm32f4/gps_uart, platform/hrc7000/gps_hrc.c) step it: the start-up probe whatever the setting, then
// Settings > Location > GPS once a module was heard, and never again once none was
#include <stdio.h>
#include "gps/gps_rx/gps_power.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	gps_power_t p = { 0 };

	// Setting on (the default): powered through the probe and after it, no switch
	gps_power_init(&p);
	CHECK(p.on && !gps_power_step(&p));
	gps_power_found(&p);
	CHECK(!gps_power_step(&p) && p.on);

	// Off, then on again: one switch each, at the next step
	gps_power_set(&p, false);
	CHECK(gps_power_step(&p) && !p.on);
	CHECK(!gps_power_step(&p) && !p.on);
	gps_power_set(&p, true);
	CHECK(gps_power_step(&p) && p.on);
	CHECK(!gps_power_step(&p));

	// Off before the module is known (the UI applies the stored setting at start-up): the probe still runs, then the
	// module goes off as soon as it has been heard
	gps_power_t q = { 0 };
	gps_power_set(&q, false);
	gps_power_init(&q);		// init keeps the setting
	CHECK(q.on && !gps_power_step(&q));
	gps_power_found(&q);
	CHECK(gps_power_step(&q) && !q.on);

	// A setting toggled back before the next step: nothing to switch
	gps_power_set(&q, true);
	gps_power_set(&q, false);
	CHECK(!gps_power_step(&q) && !q.on);

	// Nothing heard: off for good, whatever the setting does
	gps_power_t r = { 0 };
	gps_power_init(&r);
	gps_power_absent(&r);
	CHECK(!r.on && !gps_power_step(&r));
	gps_power_set(&r, false);
	gps_power_set(&r, true);
	CHECK(!gps_power_step(&r) && !r.on);

	printf("gpsPowerTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
