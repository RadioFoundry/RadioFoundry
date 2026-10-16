// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// PTT on a pin shared with the programming cable's serial line (drivers/input/sharedptt, docs/spec/hardware-dm32.md
// 6.2): host bytes must never read as a press, a held press must
#include <stdio.h>
#include "input/sharedptt/shared_ptt.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	shared_ptt_t s = { 0 };

	// Released: never pressed, whatever the serial line does
	CHECK(!shared_ptt_pressed(&s, false, false, 0, 1000));
	CHECK(!shared_ptt_pressed(&s, false, true, 995, 1000));

	// No byte ever received (no host): a low pin is a press
	CHECK(shared_ptt_pressed(&s, true, false, 0, 1000));

	// The bench failure: a host polls, its 0 bits pull the pin low on scan after scan. Bytes 1 ms apart, a scan every
	// 10 ms that always sees the pin low: never a press
	uint32_t rx = 0;
	for (uint32_t now = 2000; now < 4000; now += 10) {
		rx = now - 1u;
		CHECK(!shared_ptt_pressed(&s, true, true, rx, now));
	}
	CHECK(s.ignored == 200u);

	// The last byte just inside the quiet time: still ignored; just past it: a press (the line held low, a real key)
	CHECK(!shared_ptt_pressed(&s, true, true, 5000, 5000 + SHARED_PTT_QUIET_MS - 1u));
	CHECK(shared_ptt_pressed(&s, true, true, 5000, 5000 + SHARED_PTT_QUIET_MS));

	// The millisecond clock wraps between the byte and the scan
	CHECK(!shared_ptt_pressed(&s, true, true, 0xFFFFFFF0u, 0x10u));
	CHECK(shared_ptt_pressed(&s, true, true, 0xFFFFFFF0u, 0x10u + SHARED_PTT_QUIET_MS));

	if (failures == 0) {
		printf("sharedPttTest: all passed\n");
	}
	return failures != 0;
}
