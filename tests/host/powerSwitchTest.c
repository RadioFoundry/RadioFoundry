// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The volume knob power switch read from the battery sense line (core/radio/power_switch.c), with the MD-380's
// measured readings (docs/spec/hardware-md380.md 10.1: 8.3 V with the knob on, about 7 mV with it off)
#include <stdio.h>
#include <string.h>
#include "radio/power_switch.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define ON_MV	8300
#define OFF_MV	7	// raw 3 at 40.3 counts per 0.1 V

// Readings every step_ms from t, all at mv; returns the last answer
static bool feed(pwrsw_t *s, uint32_t *t, uint32_t mv, int n, uint32_t step_ms)
{
	bool on = true;

	for (int i = 0; i < n; i++, *t += step_ms)
		on = pwrsw_on(s, mv, *t);
	return on;
}

static void test_knob_off(void)
{
	pwrsw_t s;
	uint32_t t = 2000;

	memset(&s, 0, sizeof(s));
	CHECK(feed(&s, &t, ON_MV, 100, 10));
	// The knob turns off: on for the first two low readings, off at the third
	CHECK(pwrsw_on(&s, OFF_MV, t));
	t += 10;
	CHECK(pwrsw_on(&s, OFF_MV, t));
	t += 10;
	CHECK(!pwrsw_on(&s, OFF_MV, t));
	// and stays off
	CHECK(!feed(&s, &t, OFF_MV, 50, 10));
	// On again: at the first good reading
	CHECK(pwrsw_on(&s, ON_MV, t));
}

static void test_glitches(void)
{
	pwrsw_t s;
	uint32_t t = 2000;

	memset(&s, 0, sizeof(s));
	// One or two low readings between good ones never switch off (a TX load dip, an ADC glitch)
	for (int i = 0; i < 200; i++) {
		CHECK(pwrsw_on(&s, i % 3 == 2 ? ON_MV : OFF_MV, t));
		t += 10;
	}
	// Low readings faster than PWRSW_SAMPLE_MS count once per PWRSW_SAMPLE_MS: a burst of polls isn't three readings
	memset(&s, 0, sizeof(s));
	CHECK(feed(&s, &t, OFF_MV, 20, 0));		// 20 polls at one instant: one reading
	t += PWRSW_SAMPLE_MS - 1;
	CHECK(pwrsw_on(&s, OFF_MV, t));			// too soon: still one
	t += 1;
	CHECK(pwrsw_on(&s, OFF_MV, t));			// two
	t += PWRSW_SAMPLE_MS;
	CHECK(!pwrsw_on(&s, OFF_MV, t));		// three
	// The threshold: a flat battery (critical 6.0 V) is on, anything under 5.5 V is the switch
	memset(&s, 0, sizeof(s));
	CHECK(feed(&s, &t, 6000, 10, 10));
	CHECK(feed(&s, &t, PWRSW_OFF_MV, 10, 10));
	CHECK(!feed(&s, &t, PWRSW_OFF_MV - 1, PWRSW_SAMPLES, 10));
	// The millisecond clock wrapping between readings
	memset(&s, 0, sizeof(s));
	t = 0xFFFFFFF0u;
	CHECK(!feed(&s, &t, OFF_MV, PWRSW_SAMPLES, 10));
}

int main(void)
{
	test_knob_off();
	test_glitches();
	printf("# powerSwitchTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
