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
	// An empty position can be selected and acknowledged like any other position.
	CHECK(switch_follow_step(&f, 15, 9000) == SWITCH_ASK);
	CHECK(switch_follow_step(&f, 15, 9000 + R) == SWITCH_CHECK);
	CHECK(switch_follow_check(&f, 1, 5, 8, true));
	CHECK(switch_follow_step(&f, 15, 9000 + 2 * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 1, 15, 8, true));
	// A codeplug write changes the zone size, so revalidate the slot.
	CHECK(switch_follow_step(&f, 15, 9000 + 3 * R) == SWITCH_CHECK);
	CHECK(switch_follow_check(&f, 1, 5, 16, true));
	CHECK(switch_follow_step(&f, 15, 9000 + 4 * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 1, 15, 16, true));
	// ... and back to a longer zone: the knob's position is taken there
	CHECK(switch_follow_step(&f, 15, 9000 + 5 * R) == SWITCH_CHECK);
	CHECK(switch_follow_check(&f, 0, 0, 19, true));
	// A position the radio keeps refusing while it could take it (a channel it can't read): asked a few times, then
	// given up, so a scan or the battery saver isn't stopped every SWITCH_FOLLOW_RETRY_MS
	CHECK(switch_follow_step(&f, 7, 12000) == SWITCH_ASK);
	for (unsigned i = 1; i <= SWITCH_FOLLOW_ASKS; i++) {
		CHECK(switch_follow_step(&f, 7, 12000 + i * R) == SWITCH_CHECK);
		CHECK(switch_follow_check(&f, 0, 0, 19, true));
	}
	CHECK(switch_follow_step(&f, 7, 12000 + (SWITCH_FOLLOW_ASKS + 1) * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 0, 0, 19, true));
	CHECK(switch_follow_step(&f, 7, 12000 + (SWITCH_FOLLOW_ASKS + 2) * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 0, 0, 19, true));
	// A host's choice holds: another zone and position (not the knob's), and the zone change it brings isn't one
	CHECK(switch_follow_step(&f, 3, 20000) == SWITCH_ASK);
	CHECK(switch_follow_step(&f, 3, 20000 + R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 0, 3, 19, true));
	switch_follow_hold(&f, 1, 20000 + R);
	CHECK(switch_follow_step(&f, 3, 20000 + 2 * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 0, 3, 19, true));	// not in the host's zone yet: wait
	CHECK(switch_follow_step(&f, 3, 20000 + 3 * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 1, 6, 8, true));	// there, at the host's position 6: left alone
	CHECK(switch_follow_step(&f, 3, 20000 + 4 * R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 1, 7, 8, true));	// a host step in it: still left alone
	// then the knob moves: it wins again
	CHECK(switch_follow_step(&f, 4, 22000) == SWITCH_ASK);
	CHECK(switch_follow_step(&f, 4, 22000 + R) == SWITCH_CHECK);
	CHECK(switch_follow_check(&f, 1, 7, 8, true));
	// A hold whose zone never comes (refused) gives up after SWITCH_HOLD_MS, and the knob is followed as before
	CHECK(!switch_follow_check(&f, 1, 4, 8, true));
	switch_follow_hold(&f, 5, 23000);
	CHECK(switch_follow_step(&f, 4, 23000 + R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 1, 4, 8, true));
	CHECK(switch_follow_step(&f, 4, 23000 + SWITCH_HOLD_MS + R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 1, 4, 8, true));
	// A pending request and knob turns in VFO or dual watch must not move the restored memory channel.
	CHECK(switch_follow_step(&f, 6, 26000) == SWITCH_ASK);
	switch_follow_mode(&f, true, 6, 1, 8);
	CHECK(switch_follow_step(&f, 6, 26010) == SWITCH_IDLE);
	switch_follow_mode(&f, true, 7, 0, 19);
	CHECK(switch_follow_step(&f, 7, 26020) == SWITCH_IDLE);
	switch_follow_mode(&f, false, 7, 2, 11);
	CHECK(switch_follow_step(&f, 7, 26000 + R) == SWITCH_CHECK);
	CHECK(!switch_follow_check(&f, 2, 1, 11, true));
	CHECK(switch_follow_step(&f, 8, 26000 + R + 10) == SWITCH_ASK);
	// Across the clock's wrap
	CHECK(switch_follow_step(&f, 2, 0xFFFFFF00u) == SWITCH_ASK);
	CHECK(switch_follow_step(&f, 2, 0xFFFFFF00u + R) == SWITCH_CHECK);
	printf("switchFollowTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
