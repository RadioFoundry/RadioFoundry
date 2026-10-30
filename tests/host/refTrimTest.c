// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The reference trim setting (core/radio/ref_trim.c, docs/spec/radio-features.md 6.1): Hz clamped to +-10000 and
// rounded to 100 Hz steps, then handed to the board's hal_radio_t.ref_trim; without the hook only stored
#include <stdio.h>
#include "radio/ref_trim.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static hal_radio_t radio;
const board_t board = { .radio = &radio };

static int hook_calls;
static int16_t hook_hz;

static void hook(int16_t hz)
{
	hook_calls++;
	hook_hz = hz;
}

int main(void)
{
	CHECK(radio_ref_trim() == 0);
	CHECK(ref_trim_clamp(0) == 0 && ref_trim_clamp(-2100) == -2100 && ref_trim_clamp(3000) == 3000);
	CHECK(ref_trim_clamp(149) == 100 && ref_trim_clamp(150) == 200 && ref_trim_clamp(-149) == -100);
	CHECK(ref_trim_clamp(-150) == -200 && ref_trim_clamp(49) == 0 && ref_trim_clamp(-49) == 0);
	CHECK(ref_trim_clamp(10000) == 10000 && ref_trim_clamp(-9950) == -10000 && ref_trim_clamp(10001) == 10000);
	CHECK(ref_trim_clamp(-40000) == -10000 && ref_trim_clamp(32767) == 10000);

	// No board hook: no setting, the value is only kept
	CHECK(!ref_trim_available());
	radio_set_ref_trim(-2000);
	CHECK(radio_ref_trim() == -2000 && hook_calls == 0);

	// The board's trim gets the clamped, rounded value
	radio.ref_trim = hook;
	CHECK(ref_trim_available());
	radio_set_ref_trim(-2140);
	CHECK(hook_calls == 1 && hook_hz == -2100 && radio_ref_trim() == -2100);
	radio_set_ref_trim(15000);
	CHECK(hook_calls == 2 && hook_hz == 10000 && radio_ref_trim() == 10000);
	radio_set_ref_trim(0);
	CHECK(hook_calls == 3 && hook_hz == 0 && radio_ref_trim() == 0);

	if (failures) {
		printf("refTrimTest: %d failures\n", failures);
		return 1;
	}
	printf("refTrimTest: all passed\n");
	return 0;
}
