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
static bool tx_seen;		// the radio has sent on the line
static uint32_t tx_ms;		// when it last did

// The gate as a board calls it, with the line's flagged count and TX state from above
static bool gate(shared_ptt_t *s, bool pin_low, bool rx_seen, uint32_t rx_ms, uint32_t now_ms)
{
	shared_ptt_line_t line = { .rx_seen = rx_seen, .rx_ms = rx_ms, .flagged = flagged, .keyed = keyed,
				  .tx_seen = tx_seen, .tx_ms = tx_ms };
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

// The pin idles high from t for ms, scanned every 10 ms: never a press
static void idle(shared_ptt_t *s, bool rx_seen, uint32_t rx, uint32_t t, uint32_t ms)
{
	for (uint32_t d = 0; d < ms; d += 10) {
		CHECK(!gate(s, false, rx_seen, rx, t + d));
	}
}

// From power-on: the gate's first call at t; the pin idles high long enough to count as idle
#define IDLE_FROM_START	(PLUG_PTT_SETTLE_MS + PLUG_PTT_ARM_MS + 10u)

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
	idle(&s, false, 0, 0, IDLE_FROM_START);

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
	// The gate shut a hold that had started: a key still needs the line to go high after the byte, and the hold counts
	// from the first low after that
	CHECK(!gate(&s, false, true, 5001, 5001 + SHARED_PTT_QUIET_MS - 5u));
	CHECK(press_after(&s, true, 5001, 5001 + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 5001, 5200));

	// A burst start that the next scan sees high again: the hold starts over from the next low
	CHECK(!gate(&s, true, true, 4935, 6000));
	CHECK(!gate(&s, false, true, 4935, 6010));
	CHECK(press_after(&s, true, 4935, 6020, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 4935, 6200));

	// The last byte just inside the quiet time: still ignored; past it, held: a press (the line idled high after the
	// byte, a real key)
	CHECK(!gate(&s, false, true, 7000, 7010));
	CHECK(!gate(&s, true, true, 7000, 7000 + SHARED_PTT_QUIET_MS - 1u));
	CHECK(press_after(&s, true, 7000, 7000 + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 7000, 7200));

	// The millisecond clock wraps between the byte and the scan, and during the hold
	CHECK(!gate(&s, true, true, 0xFFFFFFF0u, 0x10u));
	CHECK(!gate(&s, false, true, 0xFFFFFFF0u, 0x20u));
	CHECK(!gate(&s, false, true, 0xFFFFFFC0u, 0xFFFFFFF0u));
	CHECK(press_after(&s, true, 0xFFFFFFC0u, 0xFFFFFFF8u, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 0xFFFFFFC0u, 0x100u));

	// Ten minutes of host traffic with late scans: never a press. A real press afterwards still is one
	shared_ptt_t h = { 0 };
	CHECK(host_traffic(&h, 100000, 600000) == 0u);
	CHECK(h.ignored > 0u);
	idle(&h, true, 700000, 700000 + 5u, PLUG_PTT_ARM_MS + 10u);
	CHECK(press_after(&h, true, 700000, 700200, 10, 100) == SHARED_PTT_HOLD_MS);

	// The radio's own RF on the cable (a minute keyed, seen on the bench): a burst keyed the radio by the link, RF pulled
	// the line low with garbage flagged characters and no good byte, the gate took it for a press, and the press kept
	// the radio keyed. While the radio transmits, a new low never presses, however long
	shared_ptt_t r = { 0 };
	idle(&r, true, 790000, 790000, IDLE_FROM_START);
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
	idle(&k, true, 0, 899600, IDLE_FROM_START);
	CHECK(!gate(&k, false, true, 0, 899990));
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
	CHECK(!gate(&k, false, true, 0, 963900));

	// A late scan reports the press long after the pin went low: its break and bounce still settle from the report
	CHECK(!gate(&k, true, true, 0, 964000));
	CHECK(gate(&k, true, true, 0, 964000 + SHARED_PTT_HOLD_MS + 40u));
	flagged++;
	CHECK(gate(&k, true, true, 0, 964000 + SHARED_PTT_HOLD_MS + 40u + SHARED_PTT_SETTLE_MS - 1u));
	CHECK(gate(&k, true, true, 0, 964500));
	CHECK(!gate(&k, false, true, 0, 964600));

	// The line latched low (gotchas.md 16.5). From power-on, as after a chip reset: never a press, stuck after
	// SHARED_PTT_STUCK_MS, released only by the pin reading high
	shared_ptt_t b = { 0 };
	for (uint32_t t = 1000; t < 601000; t += 10) {
		CHECK(!gate(&b, true, false, 0, t));
		CHECK(shared_ptt_stuck(&b, t) == (t >= 1000 + SHARED_PTT_STUCK_MS));
	}
	CHECK(!gate(&b, false, false, 0, 601000));
	CHECK(!shared_ptt_stuck(&b, 601000));
	idle(&b, false, 0, 601000, PLUG_PTT_ARM_MS + 10u);	// a key held at power-on, let go, pressed
	CHECK(press_after(&b, false, 0, 601200, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!shared_ptt_stuck(&b, 601400));

	// Latched at the end of host traffic: the host's last byte and the radio's answer both after the last high read,
	// low from then on, past the quiet time
	shared_ptt_t e = { 0 };
	idle(&e, false, 0, 0, IDLE_FROM_START);
	CHECK(!gate(&e, false, true, 1000, 1005));
	tx_seen = true;
	tx_ms = 1500;
	for (uint32_t t = 2000; t < 62000; t += 10) {
		CHECK(!gate(&e, true, true, 1995, t));
	}
	CHECK(shared_ptt_stuck(&e, 62000));
	// A good byte: the line is alive after all
	CHECK(!gate(&e, true, true, 62001, 62010));
	CHECK(!shared_ptt_stuck(&e, 62010));
	tx_seen = false;

	// A key's bounce frames a good byte as it closes (or a hand mic sends one), with no send of the radio's own since
	// the line last read high: a press, never the latch (field reports: nothing for 2 s, then the stuck LEDs)
	shared_ptt_t m = { 0 };
	idle(&m, false, 0, 9600, IDLE_FROM_START);
	CHECK(!gate(&m, false, false, 0, 10000));
	CHECK(press_after(&m, true, 10003, 10003 + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!shared_ptt_stuck(&m, 13000));
	CHECK(!gate(&m, false, true, 10003, 13010));
	// The same after the radio answered a host long ago: its old send is from before the high read
	tx_seen = true;
	tx_ms = 12000;
	CHECK(!gate(&m, false, true, 10003, 13020));
	CHECK(press_after(&m, true, 13023, 13023 + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);
	tx_seen = false;

	// What the rule gives up: a bounce byte as the key closes, with a send of the radio's own since the last high read
	// (a host on the cable the radio answers), reads as the latch. Refused and stuck; the next press, after the line
	// reads high again, keys
	shared_ptt_t g2 = { 0 };
	idle(&g2, false, 0, 20000, IDLE_FROM_START);
	CHECK(!gate(&g2, false, true, 19000, 20400));
	tx_seen = true;
	tx_ms = 20404;
	CHECK(press_after(&g2, true, 20406, 20406 + SHARED_PTT_QUIET_MS, 10, 2500) == NEVER);
	CHECK(shared_ptt_stuck(&g2, 22500));
	CHECK(!gate(&g2, false, true, 20406, 22510));
	CHECK(press_after(&g2, true, 20406, 22520, 10, 100) == SHARED_PTT_HOLD_MS);
	tx_seen = false;

	// A long answer of the radio's: the host sends during it, the last host byte comes after the last high read and the
	// line clamps while the radio is still sending. The port reports a write under way as sending now (tx_ms follows the
	// clock), so the clamp reads as one after the radio's send: never a press
	shared_ptt_t w = { 0 };
	idle(&w, false, 0, 30000, IDLE_FROM_START);
	tx_seen = true;
	tx_ms = 30500;		// the write starts
	CHECK(!gate(&w, false, true, 30490, 30500));
	for (uint32_t t = 30510; t < 31000; t += 10) {
		tx_ms = t;	// still writing
		CHECK(!gate(&w, true, true, 30503, t));
	}
	tx_ms = 31000;		// the write ends; the line stays clamped
	for (uint32_t t = 31010; t < 33100; t += 10) {
		CHECK(!gate(&w, true, true, 30503, t));
	}
	CHECK(shared_ptt_stuck(&w, 33100));
	tx_seen = false;

	// The clamp lands just after a scan saw the line high: low within the guard of the radio's own send. Never a press
	shared_ptt_t c = { 0 };
	idle(&c, false, 0, 2000, IDLE_FROM_START + 400u);
	CHECK(!gate(&c, false, true, 3000, 3005));
	tx_seen = true;
	tx_ms = 3008;
	for (uint32_t t = 3010; t < 63010; t += 10) {
		CHECK(!gate(&c, true, true, 3000, t));
	}
	CHECK(shared_ptt_stuck(&c, 63010));
	CHECK(!gate(&c, false, true, 3000, 63020));
	CHECK(!shared_ptt_stuck(&c, 63020));
	// A key pressed well after the radio's send is a press, and a key held for a minute isn't stuck
	CHECK(press_after(&c, true, 3000, 63030, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(gate(&c, true, true, 3000, 123000));
	CHECK(!shared_ptt_stuck(&c, 123000));
	tx_seen = false;

	// Start-up (#716): the pin reads high while the lines settle, then rests low (a cable or mic holding it). Never a
	// press, however long the high inside the settle time; stuck, so the radio shows it
	shared_ptt_t u = { 0 };
	idle(&u, false, 0, 0, PLUG_PTT_SETTLE_MS);
	for (uint32_t t = PLUG_PTT_SETTLE_MS; t < 60000; t += 10) {
		CHECK(!gate(&u, true, false, 0, t));
	}
	CHECK(shared_ptt_stuck(&u, 60000));
	CHECK(!u.arm.armed);

	// The settle time counts from the first call, not the clock's zero, and across a clock wrap
	u = (shared_ptt_t){ 0 };
	idle(&u, false, 0, 0xFFFFFF80u, PLUG_PTT_SETTLE_MS);
	CHECK(!u.arm.armed);
	CHECK(press_after(&u, false, 0, 0xFFFFFF80u + PLUG_PTT_SETTLE_MS, 10, 5000) == NEVER);
	// and arms across the wrap: idle high after the settle time, then a press
	u = (shared_ptt_t){ 0 };
	idle(&u, false, 0, 0xFFFFFF80u, IDLE_FROM_START);
	CHECK(u.arm.armed);
	CHECK(press_after(&u, false, 0, 0xFFFFFF80u + IDLE_FROM_START, 10, 100) == SHARED_PTT_HOLD_MS);

	// High through start-up and after: idle only PLUG_PTT_ARM_MS after the settle time, not before
	u = (shared_ptt_t){ 0 };
	idle(&u, false, 0, 5000, PLUG_PTT_SETTLE_MS + PLUG_PTT_ARM_MS);
	CHECK(!u.arm.armed);
	idle(&u, false, 0, 5000 + PLUG_PTT_SETTLE_MS + PLUG_PTT_ARM_MS, 20);
	CHECK(u.arm.armed);
	CHECK(press_after(&u, false, 0, 5400, 10, 100) == SHARED_PTT_HOLD_MS);

	// After the settle time, glitches high shorter than the idle time don't count
	u = (shared_ptt_t){ 0 };
	idle(&u, false, 0, 0, PLUG_PTT_SETTLE_MS);
	for (uint32_t t = PLUG_PTT_SETTLE_MS; t < 10000; t += 200) {
		idle(&u, false, 0, t, 50);
		for (uint32_t d = 50; d < 200; d += 10) {
			CHECK(!gate(&u, true, false, 0, t + d));
		}
	}
	CHECK(!u.arm.armed);

	if (failures == 0) {
		printf("sharedPttTest: all passed\n");
	}
	return failures != 0;
}
