// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The input task's following of a channel switch (core/input/switch_follow.h): a new position, or a new zone, is
// asked for at once and again until the radio is on it, then not again until the knob moves or the zone changes
#include <stdio.h>

#include "input/switch_follow.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define R	SWITCH_FOLLOW_RETRY_MS

int main(void)
{
	switch_follow_t f;
	switch_follow_init(&f);
	// Not settled yet: nothing
	CHECK(switch_follow_step(&f, -1, 0) == SWITCH_IDLE);
	// Power-on: the first position is asked for at once; the codeplug isn't loaded (size 0): no new request
	CHECK(switch_follow_step(&f, 4, 100) == SWITCH_ASK);
	CHECK(switch_follow_step(&f, 4, 100 + R - 10) == SWITCH_IDLE);
	CHECK(switch_follow_step(&f, 4, 100 + R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 0, 0, 0, true));
	CHECK(switch_follow_step(&f, 4, 100 + R + 10) == SWITCH_IDLE);
	// Loaded, on its saved position 0 (the first request came too early): asked again
	CHECK(switch_follow_step(&f, 4, 100 + 2 * R) == SWITCH_CHECK);
	CHECK(switch_follow_check(&f, 0, 0, 19, true));
	// Now on it: done; later checks find nothing to do
	CHECK(switch_follow_step(&f, 4, 100 + 3 * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 0, 4, 19, true));
	// A scan moves the radio on in the same zone: left alone
	CHECK(switch_follow_step(&f, 4, 100 + 4 * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 0, 9, 19, true));
	// A turn during a transmission: asked and refused; not asked while it lasts, then again until it takes
	CHECK(switch_follow_step(&f, 5, 5000) == SWITCH_ASK);
	CHECK(switch_follow_step(&f, 5, 5000 + R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 0, 4, 19, false));
	CHECK(switch_follow_step(&f, 5, 5000 + R + 10) == SWITCH_IDLE);
	CHECK(switch_follow_step(&f, 5, 5000 + 2 * R) == SWITCH_CHECK);
	CHECK(switch_follow_check(&f, 0, 4, 19, true));
	CHECK(switch_follow_step(&f, 5, 5000 + 3 * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 0, 5, 19, true));
	// A zone key: the new zone starts on its first channel, and the knob's position is asked for
	CHECK(switch_follow_step(&f, 5, 6000) == SWITCH_CHECK);
	CHECK(switch_follow_check(&f, 1, 0, 8, true));
	CHECK(switch_follow_step(&f, 5, 6000 + R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 1, 5, 8, true));
	// A position past the end of the zone: asked once, then given up
	CHECK(switch_follow_step(&f, 15, 9000) == SWITCH_ASK);
	CHECK(switch_follow_step(&f, 15, 9000 + R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 1, 5, 8, true));
	CHECK(switch_follow_step(&f, 15, 9000 + 2 * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 1, 5, 8, true));
	// ... and back to a longer zone: the knob's position is taken there
	CHECK(switch_follow_step(&f, 15, 9000 + 3 * R) == SWITCH_CHECK);
	CHECK(switch_follow_check(&f, 0, 0, 19, true));
	// Across the clock's wrap
	CHECK(switch_follow_step(&f, 2, 0xFFFFFF00u) == SWITCH_ASK);
	CHECK(switch_follow_step(&f, 2, 0xFFFFFF00u + R) == SWITCH_CHECK);
	printf("switchFollowTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
