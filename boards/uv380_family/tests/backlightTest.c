// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family backlight PWM (backlight_uvf.h): a rate the backlight follows, with the pin held steady while
// transmitting or playing receive audio (#330, #759). The backlight stage stays lit about 38 us after the pin falls, so
// the compare is shortened by that tail: pulse plus tail is the brightness asked for, where the period allows.
#include <stdio.h>
#include <stdlib.h>
#include "backlight_uvf.h"
#include "fuzz.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	// The rate: exact in timer counts, within the 16-bit counter, one the backlight stage follows (40 kHz barely
	// dimmed the panel) and still above visible flicker
	CHECK(UVF_BL_TIMER_HZ % UVF_BL_PWM_HZ == 0);
	CHECK(UVF_BL_PERIOD == 36000u);
	CHECK(UVF_BL_PERIOD - 1 <= 0xFFFFu);
	CHECK(UVF_BL_PWM_HZ >= 1000u && UVF_BL_PWM_HZ <= 5000u);
	// The tail in timer counts: 38 us at 72 MHz, a small share of the period
	CHECK(UVF_BL_TAIL == 2736u);
	CHECK(UVF_BL_TAIL * 10u < UVF_BL_PERIOD);
	// Equivalence partitions: off, too low for the tail (one count), a middle level, fully on, values above 100
	CHECK(uvf_bl_compare(0, false) == 0);
	CHECK(uvf_bl_compare(5, false) == 1);	// the idle dim floor: 1800 counts lit is under the tail
	CHECK(uvf_bl_compare(10, false) == 3600u - 2736u);
	CHECK(uvf_bl_compare(50, false) == 18000u - 2736u);
	CHECK(uvf_bl_compare(100, false) == UVF_BL_PERIOD);
	CHECK(uvf_bl_compare(255, false) == UVF_BL_PERIOD);
	// Boundary values around the tail: 7 % (2520 counts) is under it (one count), 8 % (2880) the first level above it,
	// 99 % still PWM
	CHECK(uvf_bl_compare(7, false) == 1);
	CHECK(uvf_bl_compare(8, false) == 2880u - 2736u);
	CHECK(uvf_bl_compare(99, false) == 35640u - 2736u);
	CHECK(uvf_bl_compare(99, false) < UVF_BL_PERIOD);
	// Every Brightness setting and idle dim level from 10 % up: pulse plus tail is its exact share of the period
	for (unsigned b = 10; b <= 95; b += 5)
		CHECK((uvf_bl_compare((uint8_t)b, false) + UVF_BL_TAIL) * 100u == b * UVF_BL_PERIOD);
	// Transmit or receive audio: every lit level is the steady pin, never a pulse (#759: one count left some panels
	// dark), and off stays off
	for (unsigned p = 1; p <= 255; p++)
		CHECK(uvf_bl_compare((uint8_t)p, true) == UVF_BL_PERIOD);
	CHECK(uvf_bl_compare(0, true) == 0);
	// Exhaustive over the input type: monotonic, within the period, lit stays lit, steady only at 100+, and wherever
	// the tail fits, pulse plus tail is the exact share
	unsigned prev = 0;
	for (unsigned p = 0; p <= 255; p++) {
		unsigned c = uvf_bl_compare((uint8_t)p, false);
		CHECK(c >= prev);
		CHECK(c <= UVF_BL_PERIOD);
		CHECK((p == 0) == (c == 0));
		CHECK((p >= 100) == (c == UVF_BL_PERIOD));
		if (p > 0 && p < 100 && p * UVF_BL_PERIOD / 100u > UVF_BL_TAIL)
			CHECK(c + UVF_BL_TAIL == p * UVF_BL_PERIOD / 100u);
		prev = c;
	}
	// Seeded property: for any two levels in either state, the brighter never gets a shorter pulse, and the quiet
	// state is never dimmer than idle
	fuzz_t f;
	fuzz_init(&f, 0xB1);
	for (long i = 0; i < fuzz_iters(2000); i++) {
		uint8_t a = (uint8_t)fuzz_below(&f, 256), b = (uint8_t)fuzz_below(&f, 256);
		bool quiet = fuzz_below(&f, 2) != 0;
		if (a <= b)
			CHECK(uvf_bl_compare(a, quiet) <= uvf_bl_compare(b, quiet));
		else
			CHECK(uvf_bl_compare(a, quiet) >= uvf_bl_compare(b, quiet));
		CHECK(uvf_bl_compare(a, true) >= uvf_bl_compare(a, false));
	}
	printf("backlightTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
