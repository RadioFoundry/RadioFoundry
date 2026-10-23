// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// PTT on a pin shared with the programming cable's serial line (drivers/input/sharedptt, docs/spec/hardware-dm32.md
// 6.2): host bytes must never read as a press, a held press must
#include <stdio.h>
#include "input/sharedptt/shared_ptt.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define NEVER	0xFFFFFFFFu

// A press held from t on, scanned every step ms with no host byte since rx (or none at all): ms from t to the first
// scan that reports it pressed, NEVER if none did within limit ms
static uint32_t press_after(shared_ptt_t *s, bool rx_seen, uint32_t rx, uint32_t t, uint32_t step, uint32_t limit)
{
	for (uint32_t d = 0; d <= limit; d += step) {
		if (shared_ptt_pressed(s, true, rx_seen, rx, t + d)) {
			return d;
		}
	}
	return NEVER;
}

static uint32_t rng = 0x2545F491u;

static uint32_t rnd(uint32_t n)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng % n;
}

// Host traffic like PPP with the debug link polled over it: bursts after quiet gaps on both sides of the 50 ms gate.
// A key scan that runs late catches up with scans in the same millisecond. A burst's first byte pulls the pin low
// before the UART reports it (the receive time moves on a millisecond later). Returns the scans reported pressed.
static uint32_t host_traffic(shared_ptt_t *s, uint32_t t0, uint32_t ms)
{
	uint32_t pressed = 0, rx = t0, burst = t0 + 70u, burst_end = burst + 3u, next_scan = t0;
	bool rx_seen = true;
	for (uint32_t t = t0; t < t0 + ms; t++) {
		if (t >= burst_end) {
			burst = t + SHARED_PTT_QUIET_MS - 10u + rnd(80);
			burst_end = burst + 1u + rnd(8);
		}
		if (t > burst && t <= burst_end) {
			rx = t;		// bytes received: the UART has reported the burst so far
		}
		if (t < next_scan) {
			continue;
		}
		// 1 to 4 scans this millisecond: a late task catching up runs them back to back
		uint32_t scans = rnd(10) == 0 ? 2u + rnd(3) : 1u;
		for (uint32_t i = 0; i < scans; i++) {
			bool low = t >= burst && t <= burst_end && rnd(2) == 0;	// the 0 bits of the bytes on the line
			if (shared_ptt_pressed(s, low, rx_seen, rx, t)) {
				pressed++;
			}
		}
		next_scan = t + 10u + (rnd(4) == 0 ? rnd(10) : 0u);	// on time, or late
	}
	return pressed;
}

int main(void)
{
	shared_ptt_t s = { 0 };

	// Released: never pressed, whatever the serial line does
	CHECK(!shared_ptt_pressed(&s, false, false, 0, 1000));
	CHECK(!shared_ptt_pressed(&s, false, true, 995, 1000));

	// No byte ever received (no host): a low pin held SHARED_PTT_HOLD_MS is a press, not before
	CHECK(!shared_ptt_pressed(&s, true, false, 0, 1000));
	CHECK(!shared_ptt_pressed(&s, true, false, 0, 1000 + SHARED_PTT_HOLD_MS - 1u));
	CHECK(shared_ptt_pressed(&s, true, false, 0, 1000 + SHARED_PTT_HOLD_MS));
	// ... and stays pressed while held, for as long as it's held
	CHECK(shared_ptt_pressed(&s, true, false, 0, 1000 + SHARED_PTT_HOLD_MS + 10u));
	CHECK(shared_ptt_pressed(&s, true, false, 0, 1000 + 60000u));
	CHECK(!shared_ptt_pressed(&s, false, false, 0, 61100));	// released
	CHECK(s.ignored == 0u);	// the hold isn't the gate: only gated scans count

	// The hold is time, not scans: any number of scans in one millisecond never press; scans 1 ms apart press after
	// the same time as scans 10 ms apart
	for (int i = 0; i < 100; i++) {
		CHECK(!shared_ptt_pressed(&s, true, false, 0, 62000));
	}
	CHECK(!shared_ptt_pressed(&s, false, false, 0, 62010));
	CHECK(press_after(&s, false, 0, 62020, 1, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!shared_ptt_pressed(&s, false, false, 0, 62200));
	CHECK(press_after(&s, false, 0, 62210, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!shared_ptt_pressed(&s, false, false, 0, 62400));

	// The bench failure: a host polls, its 0 bits pull the pin low on scan after scan. Bytes 1 ms apart, a scan every
	// 10 ms that always sees the pin low: never a press
	uint32_t rx = 0;
	for (uint32_t now = 2000; now < 4000; now += 10) {
		rx = now - 1u;
		CHECK(!shared_ptt_pressed(&s, true, true, rx, now));
	}
	CHECK(s.ignored == 200u);

	// During PPP (#126): a late key scan catches up with two scans in the same millisecond, both on the low
	// bits of the first byte of a burst, 65 ms after the last byte. Not a press; the next scan on time finds the byte
	// arrived and the gate shut
	CHECK(!shared_ptt_pressed(&s, true, true, 4935, 5000));
	CHECK(!shared_ptt_pressed(&s, true, true, 4935, 5000));
	CHECK(!shared_ptt_pressed(&s, true, true, 5001, 5010));
	CHECK(s.ignored == 201u);
	// The gate shut a hold that had started: once the quiet time has passed again, the hold counts from the first
	// low after it, not from before the byte
	CHECK(press_after(&s, true, 5001, 5001 + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!shared_ptt_pressed(&s, false, true, 5001, 5200));

	// A burst start that the next scan sees high again: the hold starts over from the next low
	CHECK(!shared_ptt_pressed(&s, true, true, 4935, 6000));
	CHECK(!shared_ptt_pressed(&s, false, true, 4935, 6010));
	CHECK(press_after(&s, true, 4935, 6020, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!shared_ptt_pressed(&s, false, true, 4935, 6200));

	// The last byte just inside the quiet time: still ignored; past it, held: a press (the line held low, a real key)
	CHECK(!shared_ptt_pressed(&s, true, true, 7000, 7000 + SHARED_PTT_QUIET_MS - 1u));
	CHECK(press_after(&s, true, 7000, 7000 + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!shared_ptt_pressed(&s, false, true, 7000, 7200));

	// The millisecond clock wraps between the byte and the scan, and during the hold
	CHECK(!shared_ptt_pressed(&s, true, true, 0xFFFFFFF0u, 0x10u));
	CHECK(!shared_ptt_pressed(&s, false, true, 0xFFFFFFF0u, 0x20u));
	CHECK(press_after(&s, true, 0xFFFFFFC0u, 0xFFFFFFF8u, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!shared_ptt_pressed(&s, false, true, 0xFFFFFFC0u, 0x100u));

	// Ten minutes of host traffic with late scans: never a press. A real press afterwards still is one
	shared_ptt_t h = { 0 };
	CHECK(host_traffic(&h, 100000, 600000) == 0u);
	CHECK(h.ignored > 0u);
	CHECK(press_after(&h, true, 700000, 700000 + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);

	if (failures == 0) {
		printf("sharedPttTest: all passed\n");
	}
	return failures != 0;
}
