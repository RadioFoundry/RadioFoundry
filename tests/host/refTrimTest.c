// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The reference trim setting (core/radio/ref_trim.c, docs/spec/radio-features.md 6.1): ppb clamped to +-25000 and
// rounded to 100 ppb (0.1 ppm) steps, then handed to the board's hal_radio_t.ref_trim; without the hook only stored
#include <stdio.h>
#include "radio/ref_trim.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static hal_radio_t radio;
const board_t board = { .radio = &radio };

static int hook_calls;
static int16_t hook_ppb;

static void hook(int16_t ppb)
{
	hook_calls++;
	hook_ppb = ppb;
}

int main(void)
{
	CHECK(radio_ref_trim() == 0);
	CHECK(ref_trim_clamp(0) == 0 && ref_trim_clamp(-2100) == -2100 && ref_trim_clamp(25000) == 25000);
	CHECK(ref_trim_clamp(149) == 100 && ref_trim_clamp(150) == 200 && ref_trim_clamp(-149) == -100);
	CHECK(ref_trim_clamp(-150) == -200 && ref_trim_clamp(49) == 0 && ref_trim_clamp(-49) == 0);
	CHECK(ref_trim_clamp(25001) == 25000 && ref_trim_clamp(-24950) == -25000 && ref_trim_clamp(-40000) == -25000);
	CHECK(ref_trim_clamp(32767) == 25000);

	// What the page shows: -4.5 ppm is -1980 Hz at 440 MHz and -648 Hz at 144 MHz; 0.1 ppm is 14.4 Hz at 144 MHz
	CHECK(ref_trim_hz_at(-4500, 440) == -1980 && ref_trim_hz_at(-4500, 144) == -648);
	CHECK(ref_trim_hz_at(100, 144) == 14 && ref_trim_hz_at(-100, 145) == -15 && ref_trim_hz_at(0, 440) == 0);
	CHECK(ref_trim_hz_at(25000, 480) == 12000 && ref_trim_hz_at(-25000, 480) == -12000);

	// No board hook: no setting, the value is only kept
	CHECK(!ref_trim_available());
	radio_set_ref_trim(-2000);
	CHECK(radio_ref_trim() == -2000 && hook_calls == 0);

	// The board's trim gets the clamped, rounded value
	radio.ref_trim = hook;
	CHECK(ref_trim_available());
	radio_set_ref_trim(-2140);
	CHECK(hook_calls == 1 && hook_ppb == -2100 && radio_ref_trim() == -2100);
	radio_set_ref_trim(30000);
	CHECK(hook_calls == 2 && hook_ppb == 25000 && radio_ref_trim() == 25000);
	radio_set_ref_trim(0);
	CHECK(hook_calls == 3 && hook_ppb == 0 && radio_ref_trim() == 0);

	if (failures) {
		printf("refTrimTest: %d failures\n", failures);
		return 1;
	}
	printf("refTrimTest: all passed\n");
	return 0;
}
