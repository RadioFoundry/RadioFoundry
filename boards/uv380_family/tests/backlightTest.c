// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family backlight PWM (backlight_uvf.h): a low rate the backlight follows while idle and a rate far above
// the FM audio band while transmitting or playing receive audio (#330). The backlight stage stays lit about 38 us after the pin falls, at any
// rate, so the compare is shortened by that tail: pulse plus tail is the brightness asked for, where the period allows.
#include <stdio.h>
#include <stdlib.h>
#include "backlight_uvf.h"
#include "fuzz.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	CHECK(uvf_bl_period(false, false) == UVF_BL_IDLE_PERIOD);
	CHECK(uvf_bl_period(false, true) == UVF_BL_PERIOD);
	CHECK(uvf_bl_period(true, false) == UVF_BL_PERIOD);
	CHECK(uvf_bl_period(true, true) == UVF_BL_PERIOD);
	// Active rate: the period divides the timer clock exactly, fits the 16-bit counter, and the cycle is far above
	// the voice band so the ripple's FM sidebands fall well outside the adjacent channel
	CHECK(UVF_BL_TIMER_HZ % UVF_BL_PWM_HZ == 0);
	CHECK(UVF_BL_PERIOD == 1800u);
	CHECK(UVF_BL_PERIOD - 1 <= 0xFFFFu);
	CHECK(UVF_BL_PWM_HZ >= 25000u);
	// Idle rate: one the backlight driver follows (40 kHz barely dimmed the panel), still above visible flicker,
	// exact in timer counts and within the 16-bit counter
	CHECK(UVF_BL_TIMER_HZ % UVF_BL_IDLE_HZ == 0);
	CHECK(UVF_BL_IDLE_PERIOD == 36000u);
	CHECK(UVF_BL_IDLE_PERIOD - 1 <= 0xFFFFu);
	CHECK(UVF_BL_IDLE_HZ >= 1000u && UVF_BL_IDLE_HZ <= 5000u);
	// The tail in timer counts: 38 us at 72 MHz, longer than the transmit period (so transmit can't dim) and a small
	// share of the idle period
	CHECK(UVF_BL_TAIL == 2736u);
	CHECK(UVF_BL_TAIL > UVF_BL_PERIOD);
	CHECK(UVF_BL_TAIL * 10u < UVF_BL_IDLE_PERIOD);
	// Equivalence partitions at the idle rate: off, too low for the tail (one count), a middle level, fully on,
	// values above 100
	CHECK(uvf_bl_compare(0, UVF_BL_IDLE_PERIOD) == 0);
	CHECK(uvf_bl_compare(5, UVF_BL_IDLE_PERIOD) == 1);	// the idle dim floor: 1800 counts lit is under the tail
	CHECK(uvf_bl_compare(10, UVF_BL_IDLE_PERIOD) == 3600u - 2736u);
	CHECK(uvf_bl_compare(50, UVF_BL_IDLE_PERIOD) == 18000u - 2736u);
	CHECK(uvf_bl_compare(100, UVF_BL_IDLE_PERIOD) == UVF_BL_IDLE_PERIOD);
	CHECK(uvf_bl_compare(255, UVF_BL_IDLE_PERIOD) == UVF_BL_IDLE_PERIOD);
	// Boundary values around the tail: 7 % (2520 counts) is under it (one count), 8 % (2880) the first level above it,
	// 99 % still PWM
	CHECK(uvf_bl_compare(7, UVF_BL_IDLE_PERIOD) == 1);
	CHECK(uvf_bl_compare(8, UVF_BL_IDLE_PERIOD) == 2880u - 2736u);
	CHECK(uvf_bl_compare(99, UVF_BL_IDLE_PERIOD) == 35640u - 2736u);
	CHECK(uvf_bl_compare(99, UVF_BL_IDLE_PERIOD) < UVF_BL_IDLE_PERIOD);
	// Every Brightness setting and idle dim level from 10 % up: pulse plus tail is its exact share of the period
	for (unsigned b = 10; b <= 95; b += 5)
		CHECK((uvf_bl_compare((uint8_t)b, UVF_BL_IDLE_PERIOD) + UVF_BL_TAIL) * 100u == b * UVF_BL_IDLE_PERIOD);
	// Transmit rate: every PWM level is the one-count minimum (the tail fills the period, as it always did there)
	for (unsigned p = 1; p < 100; p++)
		CHECK(uvf_bl_compare((uint8_t)p, UVF_BL_PERIOD) == 1);
	CHECK(uvf_bl_compare(0, UVF_BL_PERIOD) == 0);
	CHECK(uvf_bl_compare(100, UVF_BL_PERIOD) == UVF_BL_PERIOD);
	// Exhaustive over the input type at both rates: monotonic, within the period, lit stays lit, steady only at 100+,
	// and wherever the tail fits, pulse plus tail is the exact share
	const uint16_t periods[] = { UVF_BL_IDLE_PERIOD, UVF_BL_PERIOD };
	for (unsigned k = 0; k < 2; k++) {
		unsigned prev = 0, per = periods[k];
		for (unsigned p = 0; p <= 255; p++) {
			unsigned c = uvf_bl_compare((uint8_t)p, (uint16_t)per);
			CHECK(c >= prev);
			CHECK(c <= per);
			CHECK((p == 0) == (c == 0));
			CHECK((p >= 100) == (c == per));
			if (p > 0 && p < 100 && p * per / 100u > UVF_BL_TAIL)
				CHECK(c + UVF_BL_TAIL == p * per / 100u);
			prev = c;
		}
	}
	// Seeded property: for any two levels at either rate, the brighter never gets a shorter pulse
	fuzz_t f;
	fuzz_init(&f, 0xB1);
	for (long i = 0; i < fuzz_iters(2000); i++) {
		uint8_t a = (uint8_t)fuzz_below(&f, 256), b = (uint8_t)fuzz_below(&f, 256);
		uint16_t per = periods[fuzz_below(&f, 2)];
		if (a <= b)
			CHECK(uvf_bl_compare(a, per) <= uvf_bl_compare(b, per));
		else
			CHECK(uvf_bl_compare(a, per) >= uvf_bl_compare(b, per));
	}
	printf("backlightTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
