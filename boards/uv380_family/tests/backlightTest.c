// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family backlight PWM (backlight_uvf.h): a low rate the backlight follows while receiving, so every brightness
// the core asks for (Brightness 10-100 %, the idle dim level of a fifth with a 5 % floor) is its duty on the panel, and
// a rate far above the FM audio band while transmitting (#330). Each rate maps every brightness to its exact duty.
#include <stdio.h>
#include <stdlib.h>
#include "backlight_uvf.h"
#include "fuzz.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	// Transmit rate: the period divides the timer clock exactly, fits the 16-bit counter, and the cycle is far above
	// the voice band so the ripple's FM sidebands fall well outside the adjacent channel
	CHECK(UVF_BL_TIMER_HZ % UVF_BL_PWM_HZ == 0);
	CHECK(UVF_BL_PERIOD == 1800u);
	CHECK(UVF_BL_PERIOD - 1 <= 0xFFFFu);
	CHECK(UVF_BL_PWM_HZ >= 25000u);
	// Receive rate: one the backlight driver follows (40 kHz barely dimmed the panel), still above visible flicker,
	// exact in timer counts and within the 16-bit counter
	CHECK(UVF_BL_TIMER_HZ % UVF_BL_IDLE_HZ == 0);
	CHECK(UVF_BL_IDLE_PERIOD == 36000u);
	CHECK(UVF_BL_IDLE_PERIOD - 1 <= 0xFFFFu);
	CHECK(UVF_BL_IDLE_HZ >= 1000u && UVF_BL_IDLE_HZ <= 5000u);
	// The receive rate's duty at a brightness is the transmit rate's: switching rates keeps the level
	for (unsigned p = 0; p <= 100; p++)
		CHECK(uvf_bl_compare((uint8_t)p, UVF_BL_IDLE_PERIOD) * UVF_BL_PERIOD ==
		      uvf_bl_compare((uint8_t)p, UVF_BL_PERIOD) * UVF_BL_IDLE_PERIOD);

	// Equivalence partitions: off, lit below one percent of a count, a middle level, fully on, values above 100
	CHECK(uvf_bl_compare(0, UVF_BL_PERIOD) == 0);
	CHECK(uvf_bl_compare(1, UVF_BL_PERIOD) == 18);
	CHECK(uvf_bl_compare(50, UVF_BL_PERIOD) == 900);
	CHECK(uvf_bl_compare(100, UVF_BL_PERIOD) == UVF_BL_PERIOD);
	CHECK(uvf_bl_compare(255, UVF_BL_PERIOD) == UVF_BL_PERIOD);

	// Boundary values: the lowest lit level, the last PWM level, the edge into fully on
	CHECK(uvf_bl_compare(2, UVF_BL_PERIOD) == 36);
	CHECK(uvf_bl_compare(99, UVF_BL_PERIOD) == 1782);
	CHECK(uvf_bl_compare(99, UVF_BL_PERIOD) < UVF_BL_PERIOD);	// still PWM, not steady

	// Every Brightness setting and idle dim level is its exact duty
	for (unsigned b = 5; b <= 100; b += 5)
		CHECK(uvf_bl_compare((uint8_t)b, UVF_BL_PERIOD) * 100u == b * UVF_BL_PERIOD);

	// Exhaustive over the input type: exact percent, monotonic, lit stays lit, PWM only strictly between off and on
	unsigned prev = 0;
	for (unsigned p = 0; p <= 255; p++) {
		unsigned c = uvf_bl_compare((uint8_t)p, UVF_BL_PERIOD);
		CHECK(c >= prev);
		CHECK(c <= UVF_BL_PERIOD);
		CHECK((p == 0) == (c == 0));
		CHECK((p >= 100) == (c == UVF_BL_PERIOD));
		if (p > 0 && p < 100)
			CHECK(c * 100u == p * UVF_BL_PERIOD);
		prev = c;
	}

	// Seeded property: for any two levels, the brighter never gets a shorter on time
	fuzz_t f;
	fuzz_init(&f, 0xB1);
	for (long i = 0; i < fuzz_iters(2000); i++) {
		uint8_t a = (uint8_t)fuzz_below(&f, 256), b = (uint8_t)fuzz_below(&f, 256);
		if (a <= b)
			CHECK(uvf_bl_compare(a, UVF_BL_PERIOD) <= uvf_bl_compare(b, UVF_BL_PERIOD));
		else
			CHECK(uvf_bl_compare(a, UVF_BL_PERIOD) >= uvf_bl_compare(b, UVF_BL_PERIOD));
	}

	printf("backlightTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
