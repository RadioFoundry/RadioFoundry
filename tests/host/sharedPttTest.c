// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PTT on a pin shared with the programming cable's serial line (drivers/input/sharedptt, docs/spec/hardware-dm32.md
// 6.2): host bytes must never read as a press, a held press must, and the radio's own transmission picked up on the
// line must never key it or keep it keyed
#include <stdio.h>
#include "input/sharedptt/shared_ptt.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define NEVER	0xFFFFFFFFu

static uint32_t flagged;	// the UART's running count of flagged characters
static bool keyed;		// the radio transmits

// The gate as a board calls it, with the line's flagged count and TX state from above
static bool gate(shared_ptt_t *s, bool pin_low, bool rx_seen, uint32_t rx_ms, uint32_t now_ms)
{
	shared_ptt_line_t line = { .rx_seen = rx_seen, .rx_ms = rx_ms, .flagged = flagged, .keyed = keyed };
	return shared_ptt_pressed(s, pin_low, &line, now_ms);
}

// A press held from t on, scanned every step ms with no host byte since rx (or none at all): ms from t to the first
// scan that reports it pressed, NEVER if none did within limit ms
static uint32_t press_after(shared_ptt_t *s, bool rx_seen, uint32_t rx, uint32_t t, uint32_t step, uint32_t limit)
{
	for (uint32_t d = 0; d <= limit; d += step) {
		if (gate(s, true, rx_seen, rx, t + d)) {
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
			if (gate(s, low, rx_seen, rx, t)) {
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
	CHECK(!gate(&s, false, false, 0, 1000));
	CHECK(!gate(&s, false, true, 995, 1000));

	// No byte ever received (no host): a low pin held SHARED_PTT_HOLD_MS is a press, not before
	CHECK(!gate(&s, true, false, 0, 1000));
	CHECK(!gate(&s, true, false, 0, 1000 + SHARED_PTT_HOLD_MS - 1u));
	CHECK(gate(&s, true, false, 0, 1000 + SHARED_PTT_HOLD_MS));
	// ... and stays pressed while held, for as long as it's held
	CHECK(gate(&s, true, false, 0, 1000 + SHARED_PTT_HOLD_MS + 10u));
	CHECK(gate(&s, true, false, 0, 1000 + 60000u));
	CHECK(!gate(&s, false, false, 0, 61100));	// released
	CHECK(s.ignored == 0u);	// the hold isn't the gate: only gated scans count

	// The hold is time, not scans: any number of scans in one millisecond never press; scans 1 ms apart press after
	// the same time as scans 10 ms apart
	for (int i = 0; i < 100; i++) {
		CHECK(!gate(&s, true, false, 0, 62000));
	}
	CHECK(!gate(&s, false, false, 0, 62010));
	CHECK(press_after(&s, false, 0, 62020, 1, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, false, 0, 62200));
	CHECK(press_after(&s, false, 0, 62210, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, false, 0, 62400));

	// The bench failure: a host polls, its 0 bits pull the pin low on scan after scan. Bytes 1 ms apart, a scan every
	// 10 ms that always sees the pin low: never a press
	uint32_t rx = 0;
	for (uint32_t now = 2000; now < 4000; now += 10) {
		rx = now - 1u;
		CHECK(!gate(&s, true, true, rx, now));
	}
	CHECK(s.ignored == 200u);

	// During PPP (#126): a late key scan catches up with two scans in the same millisecond, both on the low
	// bits of the first byte of a burst, 65 ms after the last byte. Not a press; the next scan on time finds the byte
	// arrived and the gate shut
	CHECK(!gate(&s, true, true, 4935, 5000));
	CHECK(!gate(&s, true, true, 4935, 5000));
	CHECK(!gate(&s, true, true, 5001, 5010));
	CHECK(s.ignored == 201u);
	// The gate shut a hold that had started: once the quiet time has passed again, the hold counts from the first
	// low after it, not from before the byte
	CHECK(press_after(&s, true, 5001, 5001 + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 5001, 5200));

	// A burst start that the next scan sees high again: the hold starts over from the next low
	CHECK(!gate(&s, true, true, 4935, 6000));
	CHECK(!gate(&s, false, true, 4935, 6010));
	CHECK(press_after(&s, true, 4935, 6020, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 4935, 6200));

	// The last byte just inside the quiet time: still ignored; past it, held: a press (the line held low, a real key)
	CHECK(!gate(&s, true, true, 7000, 7000 + SHARED_PTT_QUIET_MS - 1u));
	CHECK(press_after(&s, true, 7000, 7000 + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 7000, 7200));

	// The millisecond clock wraps between the byte and the scan, and during the hold
	CHECK(!gate(&s, true, true, 0xFFFFFFF0u, 0x10u));
	CHECK(!gate(&s, false, true, 0xFFFFFFF0u, 0x20u));
	CHECK(press_after(&s, true, 0xFFFFFFC0u, 0xFFFFFFF8u, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 0xFFFFFFC0u, 0x100u));

	// Ten minutes of host traffic with late scans: never a press. A real press afterwards still is one
	shared_ptt_t h = { 0 };
	CHECK(host_traffic(&h, 100000, 600000) == 0u);
	CHECK(h.ignored > 0u);
	CHECK(press_after(&h, true, 700000, 700000 + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);

	// The radio's own RF on the cable (a minute keyed, seen on the bench): a burst keyed the radio by the link, RF pulled
	// the line low with garbage flagged characters and no good byte, the gate took it for a press, and the press kept
	// the radio keyed. While the radio transmits, a new low never presses, however long
	shared_ptt_t r = { 0 };
	keyed = true;
	for (uint32_t t = 800000; t < 860000; t += 10) {
		if (t % 170u < 10u) {
			flagged++;	// about 6 a second, as measured
		}
		CHECK(!gate(&r, true, true, 790000, t));
	}
	CHECK(r.ignored == 6000u);
	// TX over, the RF gone: the line reads high, and a press after it is one again
	keyed = false;
	CHECK(!gate(&r, false, true, 790000, 860000));
	CHECK(press_after(&r, true, 790000, 860010, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&r, false, true, 790000, 860200));

	// A press held when the radio keys stays held while it transmits: the key grounds the line, nothing arrives. Its own
	// break, and the key's bounce, while it settles, are not noise
	shared_ptt_t k = { 0 };
	CHECK(press_after(&k, true, 0, 900000, 10, 100) == SHARED_PTT_HOLD_MS);
	flagged++;	// the break, reported late
	CHECK(gate(&k, true, true, 0, 900000 + SHARED_PTT_HOLD_MS + 10u));
	flagged += 2u;	// bounce
	CHECK(gate(&k, true, true, 0, 900000 + SHARED_PTT_HOLD_MS + SHARED_PTT_SETTLE_MS - 1u));
	keyed = true;
	for (uint32_t t = 900100; t < 960000; t += 10) {
		CHECK(gate(&k, true, true, 0, t));
	}
	// Released with RF on the line: the pin still reads low, but a flagged character came. Released, and stays
	// released while the line stays low and noisy
	flagged++;
	CHECK(!gate(&k, true, true, 0, 960000));
	CHECK(k.noise_releases == 1u);
	for (uint32_t t = 960010; t < 961000; t += 10) {
		flagged++;
		CHECK(!gate(&k, true, true, 0, t));
	}
	keyed = false;	// the radio unkeyed: noisy-low still isn't a press until the pin reads high once
	CHECK(!gate(&k, true, true, 0, 961010));
	CHECK(!gate(&k, true, true, 0, 961100));
	CHECK(!gate(&k, false, true, 0, 961110));
	CHECK(press_after(&k, true, 0, 961120, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&k, false, true, 0, 961300));

	// The flagged count wraps (16 bits kept): a held press with no new character stays held across it
	flagged = 0xFFFFFFFEu;
	CHECK(press_after(&k, true, 0, 962000, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(gate(&k, true, true, 0, 962500));
	flagged = 0x00000003u;
	CHECK(!gate(&k, true, true, 0, 962510));
	CHECK(!gate(&k, false, true, 0, 962520));

	// A good byte during a held press: the line carries data, so the key is up (a held key grounds it), keyed or not
	CHECK(press_after(&k, true, 0, 963000, 10, 100) == SHARED_PTT_HOLD_MS);
	keyed = true;
	CHECK(gate(&k, true, true, 0, 963500));
	CHECK(!gate(&k, true, true, 963505, 963510));
	CHECK(!gate(&k, true, true, 963505, 963600));	// keyed: no new press on the low line
	keyed = false;
	CHECK(!gate(&k, false, true, 963505, 963700));

	// A late scan reports the press long after the pin went low: its break and bounce still settle from the report
	CHECK(!gate(&k, true, true, 0, 964000));
	CHECK(gate(&k, true, true, 0, 964000 + SHARED_PTT_HOLD_MS + 40u));
	flagged++;
	CHECK(gate(&k, true, true, 0, 964000 + SHARED_PTT_HOLD_MS + 40u + SHARED_PTT_SETTLE_MS - 1u));
	CHECK(gate(&k, true, true, 0, 964500));
	CHECK(!gate(&k, false, true, 0, 964600));

	if (failures == 0) {
		printf("sharedPttTest: all passed\n");
	}
	return failures != 0;
}
