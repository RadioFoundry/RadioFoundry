// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PTT on a connector that may be empty (drivers/input/plugptt): a line that rests low with nothing plugged in never
// keys the radio, a plugged-in mic's press does
#include <stdio.h>
#include "input/plugptt/plug_ptt.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Scan the line every 10 ms from t for ms: true if any scan reported a press
static bool scan(plug_ptt_t *p, bool low, uint32_t t, uint32_t ms)
{
	bool any = false;
	for (uint32_t d = 0; d < ms; d += 10) {
		any |= plug_ptt_pressed(p, low, t + d);
	}
	return any;
}

int main(void)
{
	// Nothing plugged in: the line reads low from power-on (measured on a radio) and never keys
	plug_ptt_t p = { 0 };
	CHECK(!scan(&p, true, 0, 60000));
	CHECK(!p.armed);

	// Glitches high shorter than the arm time don't arm it
	p = (plug_ptt_t){ 0 };
	for (uint32_t t = 0; t < 10000; t += 200) {
		CHECK(!scan(&p, false, t, 50));
		CHECK(!scan(&p, true, t + 50, 150));
	}
	CHECK(!p.armed);

	// A mic plugged in at power-on: idle high arms it, then a press keys and a release unkeys
	p = (plug_ptt_t){ 0 };
	CHECK(!scan(&p, false, 0, 500));
	CHECK(p.armed);
	CHECK(plug_ptt_pressed(&p, true, 500));
	CHECK(plug_ptt_pressed(&p, true, 4500));
	CHECK(!plug_ptt_pressed(&p, false, 4510));

	// A press right after power-on, before the arm time, doesn't key; once released and idle, the next one does
	p = (plug_ptt_t){ 0 };
	CHECK(!scan(&p, false, 0, 50));
	CHECK(!scan(&p, true, 50, 1000));
	CHECK(!scan(&p, false, 1050, 200));
	CHECK(plug_ptt_pressed(&p, true, 1250));

	// A mic plugged in later: the line goes from resting low to idle high, then presses work
	p = (plug_ptt_t){ 0 };
	CHECK(!scan(&p, true, 0, 5000));
	CHECK(!scan(&p, false, 5000, 200));
	CHECK(plug_ptt_pressed(&p, true, 5200));

	// The clock wrapping while the line is high still arms it
	p = (plug_ptt_t){ 0 };
	CHECK(!scan(&p, false, 0xFFFFFF00u, 500));
	CHECK(p.armed);

	// A line that reads high while it settles at start-up, then falls to rest low (a cable or mic holding it): never
	// keys, however long it reads high inside the settle time
	p = (plug_ptt_t){ 0 };
	CHECK(!scan(&p, false, 0, PLUG_PTT_SETTLE_MS));
	CHECK(!scan(&p, true, PLUG_PTT_SETTLE_MS, 60000));
	CHECK(!p.armed);

	// The settle time counts from the first scan, not from the clock's zero: a scan that starts late settles late
	p = (plug_ptt_t){ 0 };
	CHECK(!scan(&p, false, 3000, PLUG_PTT_SETTLE_MS));
	CHECK(!scan(&p, true, 3000 + PLUG_PTT_SETTLE_MS, 5000));
	CHECK(!p.armed);

	// High through the settle time and after: arms PLUG_PTT_ARM_MS after settling, not before
	p = (plug_ptt_t){ 0 };
	CHECK(!scan(&p, false, 0, PLUG_PTT_SETTLE_MS + PLUG_PTT_ARM_MS - 10));
	CHECK(!p.armed);
	CHECK(!scan(&p, false, PLUG_PTT_SETTLE_MS + PLUG_PTT_ARM_MS - 10, 20));
	CHECK(p.armed);

	// The clock wrapping inside the settle time: settles, then arms, on time
	p = (plug_ptt_t){ 0 };
	CHECK(!scan(&p, false, 0xFFFFFF80u, PLUG_PTT_SETTLE_MS + PLUG_PTT_ARM_MS - 10));
	CHECK(!p.armed);
	CHECK(!scan(&p, false, 0xFFFFFF80u + PLUG_PTT_SETTLE_MS + PLUG_PTT_ARM_MS - 10, 20));
	CHECK(p.armed);

	if (failures) {
		printf("plugPttTest: %d failures\n", failures);
		return 1;
	}
	printf("plugPttTest: ok\n");
	return 0;
}
