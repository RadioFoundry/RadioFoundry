// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The power key held to switch off (core/radio/power_key.c)
#include <stdio.h>
#include "radio/power_key.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// n polls at one level; returns the last answer
static bool feed(pwrkey_t *k, bool down, unsigned n)
{
	bool on = true;

	for (unsigned i = 0; i < n; i++)
		on = pwrkey_on(k, down);
	return on;
}

// The press that switched the radio on, held long past 2 s, never switches it off
static void test_switch_on_press(void)
{
	pwrkey_t k;

	pwrkey_init(&k);
	CHECK(feed(&k, true, 100));
	CHECK(feed(&k, false, 1));
	CHECK(feed(&k, true, PWRKEY_OFF_POLLS - 1));
}

// After a release, 2 s held is off; a shorter press isn't, and it doesn't add up with the next one
static void test_hold_off(void)
{
	pwrkey_t k;

	pwrkey_init(&k);
	CHECK(feed(&k, false, 3));
	CHECK(feed(&k, true, PWRKEY_OFF_POLLS - 1));
	CHECK(feed(&k, false, 1));
	CHECK(feed(&k, true, PWRKEY_OFF_POLLS - 1));
	CHECK(!pwrkey_on(&k, true));
	CHECK(!pwrkey_on(&k, true));	// stays off while still held
}

int main(void)
{
	test_switch_on_press();
	test_hold_off();
	printf("powerKeyTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
