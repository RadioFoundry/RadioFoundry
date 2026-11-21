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

// The line high and quiet, scanned every 10 ms, for the arm time: from t on. Returns the time after it
static uint32_t arm(shared_ptt_t *s, bool rx_seen, uint32_t rx, uint32_t t)
{
	uint32_t end = t + SHARED_PTT_ARM_MS;
	for (; t <= end; t += 10) {
		CHECK(!gate(s, false, rx_seen, rx, t));
	}
	CHECK(s->armed);
	return t;
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
static uint32_t traffic_rx;	// host_traffic's last good byte

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
	traffic_rx = rx;
	return pressed;
}

// ---- The cable and the UART, bit by bit (the 2026-11-21 incident, hardware-dm32.md 6.2)
//
// A tool answered the boot ROM's greeting, 02 14 00 03 (stubtool.py ROM_REPLY), every 30 ms (ROM_REPEAT) from before
// power-on, through a Mac's PL2303 driver; the radio started its firmware and keyed by itself. The ROM and the
// firmware both run the line at 115200, so at the right rate those bytes are good ones, which the quiet time has
// always gated. The cable at another rate (pyserial on macOS leaves a port at 38400 when a custom rate fails), a break,
// or TXD idling low are not. This replays those on the line, at 1 us steps, into a UART like the firmware's: a start
// bit checked at its middle, 8 data bits and a stop bit sampled at their middles at 115200; a stop bit read low is a
// flagged character (framing error or break) and the receiver then waits for the line to go high. The pin is read
// every 10 ms, as the key scan does, and the radio is switched on at t = 0.
#define FW_BAUD		115200u
#define SIM_FOREVER	0xFFFFFFFFu

typedef struct {
	uint32_t baud;			// the cable's real rate
	uint32_t period_us;		// a ROM_REPLY written this often...
	uint32_t write_from, write_until;	// ...in this time (us)
	uint32_t low_from, low_until;	// the cable holds the line low (a break, TXD idling low) in this time
	uint32_t key_from, key_until;	// a finger holds the PTT (the line grounded)
	uint32_t bounce;		// the key bounces: this many short lows in the 2 ms before it closes
	uint32_t scan_phase;		// the first key scan (us)
} line_case_t;

typedef struct {
	uint32_t first_press_ms;	// SIM_FOREVER if none
	uint32_t good, flagged;		// characters the UART took
} line_result_t;

static line_result_t run_line(const line_case_t *c, uint32_t end_us)
{
	static const uint8_t reply[4] = { 0x02, 0x14, 0x00, 0x03 };
	line_result_t res = { SIM_FOREVER, 0, 0 };
	shared_ptt_t g = { 0 };
	// the cable's serializer: bytes waiting (the driver buffers what the line can't carry yet) and the frame on the
	// wire
	uint32_t queued = 0, qpos = 0, next_write = c->write_from;
	bool sending = false;
	double bit_us = 1e6 / c->baud, frame_t0 = 0;
	uint16_t frame = 0;
	// the firmware's UART
	double fw_bit = 1e6 / FW_BAUD, rx_t0 = 0;
	bool rx_busy = false, rx_wait_high = false;
	int rx_bit = 0;
	uint32_t rx_flagged = 0, rx_ms = 0;
	bool rx_seen = false;
	uint32_t next_scan = c->scan_phase;
	for (uint32_t t = 0; t < end_us; t++) {
		if (t >= next_write && t < c->write_until) {
			if (queued < 4096u) {
				queued += 4u;
			}
			next_write += c->period_us;
		}
		if (!sending && queued != 0) {
			uint8_t b = reply[qpos];
			qpos = (qpos + 1u) & 3u;
			queued--;
			frame = (uint16_t)(0x200u | ((uint16_t)b << 1));	// start 0, 8 data LSB first, stop 1
			frame_t0 = t;
			sending = true;
		}
		bool level = true;
		if (sending) {
			int bit = (int)((t - frame_t0) / bit_us);
			if (bit >= 10) {
				sending = false;
			} else {
				level = (frame >> bit) & 1u;
			}
		}
		if (t >= c->low_from && t < c->low_until) {
			level = false;
		}
		if (t >= c->key_from && t < c->key_until) {
			level = false;
		} else if (c->bounce != 0 && t + 2000u >= c->key_from && t < c->key_from) {
			// short lows before the contact closes: 30 us each, spread over 2 ms
			uint32_t before = c->key_from - t;
			if ((before / (2000u / c->bounce)) % 2u == 0u && before % (2000u / c->bounce) < 30u) {
				level = false;
			}
		}
		// UART
		if (!rx_busy) {
			if (rx_wait_high) {
				rx_wait_high = !level;
			} else if (!level) {
				rx_busy = true;
				rx_t0 = t;
				rx_bit = 0;
			}
		}
		if (rx_busy && t >= rx_t0 + (rx_bit + 0.5) * fw_bit) {
			if (rx_bit == 0 && level) {
				rx_busy = false;		// a glitch, not a start bit
			} else if (rx_bit == 9) {
				rx_busy = false;
				if (level) {
					res.good++;
					rx_seen = true;
					rx_ms = t / 1000u;
				} else {
					res.flagged++;
					rx_flagged++;
					rx_wait_high = true;
				}
			} else {
				rx_bit++;
			}
		}
		if (t == next_scan) {
			next_scan += 10000u;
			shared_ptt_line_t line = { .rx_seen = rx_seen, .rx_ms = rx_ms, .flagged = rx_flagged, .keyed = false };
			if (shared_ptt_pressed(&g, !level, &line, t / 1000u) && res.first_press_ms == SIM_FOREVER) {
				res.first_press_ms = t / 1000u;
			}
		}
	}
	return res;
}

static line_case_t quiet_line(void)
{
	line_case_t c = { .baud = FW_BAUD, .period_us = 30000u, .write_from = SIM_FOREVER, .write_until = 0,
			  .low_from = SIM_FOREVER, .low_until = 0, .key_from = SIM_FOREVER, .key_until = 0,
			  .bounce = 0, .scan_phase = 3000u };
	return c;
}

static void line_cases(void)
{
	static const uint32_t rates[] = { 300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400,
					  375000, 460800, 921600 };
	line_result_t res;
	line_case_t c;

	// The greeting answered every 30 ms from before power-on, at every rate the cable might really be at: never a press
	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		for (uint32_t phase = 0; phase < 10000u; phase += 3331u) {
			c = quiet_line();
			c.baud = rates[i];
			c.write_from = 0;
			c.write_until = SIM_FOREVER;
			c.scan_phase = phase;
			res = run_line(&c, 4000000u);
			CHECK(res.first_press_ms == SIM_FOREVER);
			if (res.first_press_ms != SIM_FOREVER) {
				printf("  greeting at %u baud, scan phase %u us: pressed at %u ms\n", rates[i], phase,
				       res.first_press_ms);
			}
			if (rates[i] == FW_BAUD) {
				CHECK(res.good > 0u && res.flagged == 0u);	// the right rate: good bytes, as on the bench
			}
		}
	}

	// A break, or TXD idling low, from before power-on: never
	c = quiet_line();
	c.low_from = 0;
	c.low_until = SIM_FOREVER;
	res = run_line(&c, 5000000u);
	CHECK(res.first_press_ms == SIM_FOREVER);

	// The greeting at 115200 (good bytes), or at a wrong rate (garbage), then the cable drops the line and holds it
	// (a port closed with a break, TXD idling low): never, however soon or late within the arm time it drops
	static const uint32_t drop_after_ms[] = { 0, 10, 31, 60, 100, 300, 600, 900 };
	for (unsigned r = 0; r < 3; r++) {
		for (unsigned i = 0; i < sizeof(drop_after_ms) / sizeof(drop_after_ms[0]); i++) {
			c = quiet_line();
			c.baud = r == 0 ? FW_BAUD : r == 1 ? 38400u : 375000u;
			c.write_from = 0;
			c.write_until = 2000000u;
			c.low_from = 2000000u + drop_after_ms[i] * 1000u;
			c.low_until = SIM_FOREVER;
			res = run_line(&c, 5000000u);
			CHECK(res.first_press_ms == SIM_FOREVER);
			if (res.first_press_ms != SIM_FOREVER) {
				printf("  greeting at %u baud, line low %u ms after: pressed at %u ms\n", c.baud,
				       drop_after_ms[i], res.first_press_ms);
			}
		}
	}

	// Armed (the line quiet from power-on), then a host starts: the greeting, a 300 ms break in it, the greeting again.
	// Never
	c = quiet_line();
	c.write_from = 2000000u;
	c.write_until = SIM_FOREVER;
	c.low_from = 2600000u;
	c.low_until = 2900000u;
	res = run_line(&c, 4000000u);
	CHECK(res.first_press_ms == SIM_FOREVER);
	// ... and at a wrong rate, then held low
	c.baud = 38400u;
	c.write_until = 2600000u;
	c.low_until = SIM_FOREVER;
	res = run_line(&c, 5000000u);
	CHECK(res.first_press_ms == SIM_FOREVER);

	// A finger on a quiet line: a press within the hold and a scan or two
	c = quiet_line();
	c.key_from = 1500000u;
	c.key_until = 2000000u;
	res = run_line(&c, 2500000u);
	CHECK(res.first_press_ms >= 1520u && res.first_press_ms <= 1540u);
	// ... with its contact bouncing (characters just before the line goes low, at every scan phase): still a press. A
	// bounce can make a good byte, which then waits out the quiet time first
	for (uint32_t phase = 0; phase < 10000u; phase += 500u) {
		c.bounce = 4;
		c.scan_phase = phase;
		res = run_line(&c, 2500000u);
		CHECK(res.first_press_ms >= 1520u &&
		      res.first_press_ms <= 1500u + SHARED_PTT_QUIET_MS + SHARED_PTT_HOLD_MS + 30u);
		if (res.first_press_ms > 1500u + SHARED_PTT_QUIET_MS + SHARED_PTT_HOLD_MS + 30u) {
			printf("  bounced press, scan phase %u us: pressed at %u ms\n", phase, res.first_press_ms);
		}
	}
	// Held from power-on: nothing, until it is let go for the arm time and pressed again
	c = quiet_line();
	c.key_from = 0;
	c.key_until = 2000000u;
	res = run_line(&c, 2900000u);
	CHECK(res.first_press_ms == SIM_FOREVER);
	c.key_from = 0;
	c.key_until = 1000000u;
	c.low_from = 2200000u;		// the finger again, as a held low
	c.low_until = 2600000u;
	res = run_line(&c, 2700000u);
	CHECK(res.first_press_ms >= 2220u && res.first_press_ms <= 2240u);
	// After the greeting stops and the line has been high and quiet for the arm time: a press works again
	c = quiet_line();
	c.write_from = 0;
	c.write_until = 2000000u;
	c.key_from = 2000000u + SHARED_PTT_ARM_MS * 1000u + 100000u;
	c.key_until = c.key_from + 300000u;
	res = run_line(&c, c.key_until);
	CHECK(res.first_press_ms != SIM_FOREVER && res.first_press_ms <= c.key_from / 1000u + 40u);
}

// ---- Random sequences over everything that reaches the line
//
// A world around the gate, a millisecond at a time: a finger on the key (its contact bouncing as it closes and
// opens), a cable that talks at the right rate or a wrong one, goes quiet, or holds the line low (a break, TXD idling
// low, a port closed badly), the radio answering on the line and, now and then, the line latching low as gotchas.md
// 16.5 describes, a host keying the radio over the link, the radio's own RF pulling the line low while it transmits,
// the core's TX time-out, chip resets (the latch stays) and power cycles (it goes). The key scan runs every 10 ms,
// sometimes late. Every scan checks what the gate says against what the world did:
// - Never keyed by the cable: a press is reported only while the line is low in a run the finger was part of, with
//   nothing received since the finger let go (a line the finger held and something else still holds reads the same:
//   the TX time-out ends it), or in a run that started after the line had received nothing for SHARED_PTT_ARM_MS
//   but the key's own bounces (nothing on the line tells that from a finger, shared_ptt.h). The radio's own RF on the
//   cable doesn't count as received: it isn't the cable's doing.
// - Stuck never while the line reads high, during a press or while good bytes arrive, and always once it has been
//   latched for SHARED_PTT_STUCK_MS (after the last good byte's quiet time) with the radio not transmitting, armed or
//   not.
// And from time to time, and at the end, the world is left alone (cable quiet or out, finger off, link PTT off, a power
// cycle if the line is latched) until it has been quiet for the arm time, and a finger press, bounced or not, must be a
// press within the hold, and the quiet time when its bounce made a good byte: no sequence may leave the PTT dead.
#define W_HIST		2048u		// milliseconds of history kept (more than the arm time)
#define W_TOT_MS	5000u		// the core's TX time-out, short so that runs reach it
#define W_LINK_MS	10000u		// the link's PTT ends by itself this long after it was keyed
#define W_BOUNCE_MS	2u		// the contact bounces this long before it closes and after it opens

enum { W_IDLE, W_TALK, W_HOLD };	// the cable: quiet and high (or out), talking, holding the line low

typedef struct {
	shared_ptt_t g;
	uint32_t now, boot_ms, next_scan;
	// the UART and the radio, as the gate's line struct reports them
	bool rx_seen, tx_seen;
	uint32_t rx_ms, tx_ms, flagged;
	// what acts on the line
	bool finger, latched, link_ptt, pressed, tot;
	uint8_t cable;
	bool talk_good;			// talking at the firmware's rate: good bytes (else flagged garbage)
	uint32_t bounce_at;		// when the contact's pending bounce character arrives
	uint8_t bounce_kind;		// what it is: 0 none, 1 flagged, 2 a good byte
	uint32_t link_ms, press_ms, latch_ms, keyed_ms;
	bool keyed;
	// what the line did
	bool low;
	uint8_t hist[W_HIST];		// per millisecond: 1 the line low, 2 a character from the cable or the key, 4 an
					// edge of the finger or of a low that could be a press
	bool run_finger;		// the current low run had the finger in it, and nothing arrived since it let go
	bool run_quiet;			// the current low run started after the arm time with nothing received
	bool lifted;			// the finger let go during the current low run
	uint32_t report_ms;		// when the gate first reported the current press
	uint32_t char_ms;		// the last character of any kind (the radio's RF too), or the line low
	// what went wrong, and what happened at all
	uint32_t bad_press, bad_stuck_high, bad_stuck_press, bad_stuck_data, missed_stuck, dead;
	uint32_t presses, stucks, latches, bounced;
} world_t;

static uint32_t w_rng = 0x9E3779B9u;

static uint32_t w_rnd(uint32_t n)
{
	w_rng ^= w_rng << 13;
	w_rng ^= w_rng >> 17;
	w_rng ^= w_rng << 5;
	return w_rng % n;
}

static void w_boot(world_t *w, bool power)
{
	shared_ptt_t zero = { 0 };
	w->g = zero;
	w->boot_ms = w->now;
	w->rx_seen = w->tx_seen = false;
	w->flagged = 0;
	w->pressed = false;
	w->tot = false;
	w->link_ptt = false;
	if (power) {
		w->latched = false;
	}
	w->latch_ms = w->now;	// a latch that stays is new to the gate: stuck again after the stuck time
}

static void w_char(world_t *w, bool good, bool rf)
{
	if (good) {
		w->rx_seen = true;
		w->rx_ms = w->now;
	} else {
		w->flagged++;
	}
	w->char_ms = w->now;
	if (!rf) {
		w->hist[w->now % W_HIST] |= 2u;	// the radio's own RF isn't the cable's doing
	}
	if (w->low && w->lifted && (uint32_t)(w->now - w->report_ms) >= SHARED_PTT_SETTLE_MS) {
		w->run_finger = false;	// the key is up and the line received: whatever holds it now isn't the finger
	}
}

// Nothing arrived in the arm time before t but a bounce: a character within W_EDGE_MS of the finger going down or up
// (or of the end of a low run that could have been a press), or just before t
#define W_EDGE_MS	25u
static bool w_quiet_before(const world_t *w, uint32_t t)
{
	// The gate sees nothing from before it started: what counts is the arm time since then
	if ((uint32_t)(t - w->boot_ms) < SHARED_PTT_ARM_MS) {
		return false;
	}
	// The gate counts the arm time from the scan that saw a character, up to a late scan's 20 ms after it came
	for (uint32_t d = W_EDGE_MS + 1u; d + 20u <= SHARED_PTT_ARM_MS && d <= (uint32_t)(t - w->boot_ms); d++) {
		uint32_t m = t - d;
		if (!(w->hist[m % W_HIST] & 2u)) {
			continue;
		}
		bool edge = false;
		for (uint32_t e = 0; e <= 2u * W_EDGE_MS && !edge; e++) {
			edge = (w->hist[(m - W_EDGE_MS + e) % W_HIST] & 4u) != 0;
		}
		if (!edge) {
			return false;
		}
	}
	return true;
}

static void w_scan(world_t *w)
{
	shared_ptt_line_t line = { .rx_seen = w->rx_seen, .rx_ms = w->rx_ms, .flagged = w->flagged, .keyed = w->keyed,
				   .tx_seen = w->tx_seen, .tx_ms = w->tx_ms };
	bool was = w->pressed;
	w->pressed = shared_ptt_pressed(&w->g, w->low, &line, w->now);
	bool stuck = shared_ptt_stuck(&w->g, w->now);
	if (w->pressed) {
		w->presses++;
		if (!was) {
			w->report_ms = w->now;
		}
		if (!w->run_finger && !w->run_quiet) {
			w->bad_press++;
		}
		if (stuck) {
			w->bad_stuck_press++;
		}
	}
	if (stuck) {
		w->stucks++;
	}
	if (!w->low && stuck) {
		w->bad_stuck_high++;
	}
	if (stuck && w->rx_seen && (uint32_t)(w->now - w->rx_ms) < SHARED_PTT_QUIET_MS) {
		w->bad_stuck_data++;	// good bytes arrive: the line isn't clamped
	}
	// Stuck runs from the first low scan after the last good byte's quiet time, with the radio not transmitting
	uint32_t wait = SHARED_PTT_STUCK_MS + SHARED_PTT_QUIET_MS + 40u;
	if (w->latched && !stuck && (uint32_t)(w->now - w->latch_ms) >= wait &&
	    (uint32_t)(w->now - w->keyed_ms) >= wait && (!w->rx_seen || (uint32_t)(w->now - w->rx_ms) >= wait)) {
		w->missed_stuck++;
	}
}

// One millisecond of the world
static void w_step(world_t *w)
{
	w->now++;
	w->hist[w->now % W_HIST] = 0;
	// The radio keys on a press (until the time-out) or on the link
	if (w->link_ptt && (uint32_t)(w->now - w->link_ms) >= W_LINK_MS) {
		w->link_ptt = false;
	}
	if (!w->pressed) {
		w->tot = false;
		w->press_ms = w->now;
	} else if ((uint32_t)(w->now - w->press_ms) >= W_TOT_MS) {
		w->tot = true;
	}
	w->keyed = (w->pressed && !w->tot) || w->link_ptt;
	if (w->keyed) {
		w->keyed_ms = w->now;
	}
	bool grounded = w->finger || w->latched || w->cable == W_HOLD;
	bool wire = false;	// a host byte's bits on the line this millisecond
	if (w->cable == W_TALK && !grounded) {
		wire = w_rnd(3) == 0;
		if (wire && w_rnd(2) == 0) {
			w_char(w, w->talk_good, false);
			// The radio answers a request; now and then it starts while the host still sends, and the line latches
			if (w->talk_good && w_rnd(4) == 0) {
				w->tx_seen = true;
				w->tx_ms = w->now;
				if (w_rnd(40) == 0) {
					w->latched = true;
					w->latch_ms = w->now;
					w->latches++;
				}
			}
		}
	}
	bool rf = w->keyed && !grounded && w->cable != W_TALK && ((w->now / 300u) % 3u) == 0u;
	if (rf && w_rnd(170) == 0) {
		w_char(w, false, true);	// RF on the cable: garbage characters
	}
	if (w->bounce_at == w->now) {
		w->bounce_at = 0;
		if (w->bounce_kind != 0 && !w->latched && w->cable != W_HOLD) {
			w_char(w, w->bounce_kind == 2, false);
		}
	}
	bool low = grounded || rf || (wire && w_rnd(2) == 0);
	if (!low && w->low && (w->run_finger || w->run_quiet)) {
		w->hist[w->now % W_HIST] |= 4u;
	}
	if (low && !w->low) {
		w->run_finger = w->finger;
		w->run_quiet = w_quiet_before(w, w->now);
		w->lifted = false;
		if (w->run_finger || w->run_quiet) {
			w->hist[w->now % W_HIST] |= 4u;
		}
	}
	if (low && w->finger) {
		w->run_finger = true;
		w->lifted = false;
	}
	w->low = low;
	if (low) {
		w->char_ms = w->now;	// (and the line low)
		w->hist[w->now % W_HIST] |= 1u;
	}
	if (w->now >= w->next_scan) {
		// 1 to 3 scans back to back when the task runs late
		uint32_t scans = w_rnd(12) == 0 ? 2u + w_rnd(2) : 1u;
		for (uint32_t i = 0; i < scans; i++) {
			w_scan(w);
		}
		w->next_scan = w->now + 10u + (w_rnd(5) == 0 ? w_rnd(9) : 0u);
	}
}

static void w_run(world_t *w, uint32_t ms)
{
	for (uint32_t i = 0; i < ms; i++) {
		w_step(w);
	}
}

static void w_finger(world_t *w, bool down, uint8_t bounce)
{
	if (down == w->finger) {
		return;
	}
	w->hist[w->now % W_HIST] |= 4u;
	if (down) {
		// The bounce comes before the contact closes for good
		w->bounce_kind = bounce;
		w->bounce_at = w->now + 1u;
		w_run(w, W_BOUNCE_MS);
		bool was_low = w->latched || w->cable == W_HOLD;
		w->finger = true;
		if (!was_low) {
			w_char(w, false, false);	// the break: a grounded line is one long 0
		}
	} else {
		w->finger = false;
		if (w->low) {
			w->lifted = true;
		}
		w->bounce_kind = bounce;
		w->bounce_at = w->now + 1u;
	}
}

// Leave the world alone for the arm time, then press: the press must come, within the hold, or the quiet time when
// the bounce made a good byte. Returns false if it never did
static bool w_press_works(world_t *w, uint8_t bounce)
{
	w_finger(w, false, 0);
	w->cable = W_IDLE;
	w->link_ptt = false;
	if (w->latched) {
		w_boot(w, true);	// only a power cycle releases it
	}
	// Until the radio is off the air (a press its RF holds ends at its first character) and the line has been quiet
	// for the arm time
	do {
		w_run(w, 100u);
	} while (w->keyed || (uint32_t)(w->now - w->char_ms) < SHARED_PTT_ARM_MS + 60u ||
		 (uint32_t)(w->now - w->boot_ms) < SHARED_PTT_ARM_MS + 60u);
	uint32_t t0 = w->now;
	w_finger(w, true, bounce);
	uint32_t limit = SHARED_PTT_HOLD_MS + 40u + (bounce == 2u ? SHARED_PTT_QUIET_MS : 0u);
	bool ok = false;
	while ((uint32_t)(w->now - t0) <= limit + W_BOUNCE_MS && !ok) {
		w_step(w);
		ok = w->pressed;
	}
	w_run(w, 100);
	ok = ok && w->pressed;
	if (ok && bounce == 2u) {
		w->bounced++;
	}
	w_finger(w, false, w_rnd(3));
	w_run(w, 30);
	if (!ok) {
		w->dead++;
	}
	return ok;
}

// One random thing happens
static void w_event(world_t *w)
{
	switch (w_rnd(12)) {
	case 0:
	case 1:
		w_finger(w, !w->finger, w_rnd(3));
		break;
	case 2:
		w->cable = W_TALK;
		w->talk_good = w_rnd(3) != 0;
		break;
	case 3:
	case 4:
		w->cable = W_IDLE;		// the host stops, or the cable is pulled
		break;
	case 5:
		if (w->cable != W_HOLD) {
			w->cable = W_HOLD;	// a break, TXD idling low, a port closed with data going out
			if (!w->finger && !w->latched) {
				w_char(w, false, false);
			}
		}
		break;
	case 6:
		// The host keys or unkeys over the link: a request, the radio's answer
		if (!w->finger && !w->latched && w->cable != W_HOLD) {
			w->link_ptt = !w->link_ptt;
			w->link_ms = w->now;
			for (int i = 0; i < 6; i++) {
				w_char(w, true, false);
				w_step(w);
			}
			w->tx_seen = true;
			w->tx_ms = w->now;
		}
		break;
	case 7:
		w_boot(w, false);		// a chip reset: the latch stays
		break;
	case 8:
		w_boot(w, true);		// a power cycle
		break;
	case 9:
		w->tx_seen = true;		// the radio sends on its own
		w->tx_ms = w->now;
		break;
	default:
		break;				// time passes
	}
}

static void random_worlds(void)
{
	static world_t w;
	uint32_t dead = 0, presses = 0, stucks = 0, latches = 0, bounced = 0, disarms = 0;
	for (uint32_t seed = 1; seed <= 150u; seed++) {
		world_t zero = { 0 };
		w = zero;
		w_rng = 0x9E3779B9u ^ (seed * 2654435761u);
		w.now = 1000;
		w_boot(&w, true);
		w.next_scan = w.now + 3u;
		for (int i = 0; i < 400; i++) {
			w_event(&w);
			w_run(&w, 1u + w_rnd(w_rnd(4) == 0 ? 3000u : 200u));
			disarms += w.g.disarms;
			if (w_rnd(25) == 0) {
				(void)w_press_works(&w, (uint8_t)w_rnd(3));
			}
		}
		for (uint8_t bounce = 0; bounce < 3u; bounce++) {
			(void)w_press_works(&w, bounce);
		}
		if (w.bad_press || w.bad_stuck_high || w.bad_stuck_press || w.bad_stuck_data || w.missed_stuck || w.dead) {
			printf("  world %u: %u pressed by the cable, stuck %u high, %u pressed, %u with data, "
			       "%u not stuck, %u dead\n", seed, w.bad_press, w.bad_stuck_high, w.bad_stuck_press,
			       w.bad_stuck_data, w.missed_stuck, w.dead);
		}
		CHECK(w.bad_press == 0u);
		CHECK(w.bad_stuck_high == 0u && w.bad_stuck_press == 0u && w.bad_stuck_data == 0u);
		CHECK(w.missed_stuck == 0u);
		CHECK(w.dead == 0u);
		dead += w.dead;
		presses += w.presses;
		stucks += w.stucks;
		latches += w.latches;
		bounced += w.bounced;
	}
	// The worlds did reach what they check: presses, latches reported stuck, bounced presses, traffic that disarmed
	CHECK(presses > 0u && stucks > 0u && latches > 0u && bounced > 0u && disarms > 0u);
}

int main(void)
{
	shared_ptt_t s = { 0 };

	// Power-on: a line low from the start never presses, however long (a cable holding it, 2026-11-21)
	shared_ptt_t p = { 0 };
	for (uint32_t t = 0; t < 10000; t += 10) {
		CHECK(!gate(&p, true, false, 0, t));
	}
	CHECK(!p.armed && p.ignored == 1000u);
	// Released for less than the arm time, then low again: still nothing, and the arm time starts over
	for (uint32_t t = 10000; t < 10000 + SHARED_PTT_ARM_MS - 20u; t += 10) {
		CHECK(!gate(&p, false, false, 0, t));
	}
	CHECK(press_after(&p, false, 0, 10000 + SHARED_PTT_ARM_MS - 20u, 10, 5000) == NEVER);
	// Released for the arm time: a press is one again
	uint32_t ta = arm(&p, false, 0, 20000);
	CHECK(press_after(&p, false, 0, ta, 10, 100) == SHARED_PTT_HOLD_MS);

	(void)arm(&s, false, 0, 0);

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

	// During PPP (#126): a late key scan catches up with two scans in the same millisecond, both on the low bits of
	// the first byte of a burst, 65 ms after the last byte. Not a press; the next scan on time finds the byte arrived.
	// The polling before had already disarmed the gate, so all three are ignored
	CHECK(!gate(&s, true, true, 4935, 5000));
	CHECK(!gate(&s, true, true, 4935, 5000));
	CHECK(!gate(&s, true, true, 5001, 5010));
	CHECK(s.ignored == 203u && !s.armed && s.disarms == 1u);
	// After traffic the 50 ms quiet time is not enough: a low within the arm time never presses, however long it is
	// held. Once the line has read high and quiet for the arm time, a press is one again
	CHECK(!gate(&s, false, true, 5001, 5001 + SHARED_PTT_QUIET_MS - 5u));
	CHECK(press_after(&s, true, 5001, 5001 + SHARED_PTT_QUIET_MS, 10, SHARED_PTT_ARM_MS) == NEVER);
	ta = arm(&s, true, 5001, 7000);
	CHECK(press_after(&s, true, 5001, ta, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 5001, ta + 200u));

	// A burst start that the next scan sees high again, before its byte is reported: the hold starts over from the
	// next low
	ta += 300u;
	CHECK(!gate(&s, true, true, 5001, ta));
	CHECK(!gate(&s, false, true, 5001, ta + 10u));
	CHECK(press_after(&s, true, 5001, ta + 20u, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 5001, ta + 200u));

	// One good byte on an idle line, then a low 10 ms after it: the key's own bounce, maybe. Ignored for the quiet time
	// from the byte, then a press
	ta += 300u;
	CHECK(!gate(&s, false, true, ta - 1u, ta));
	CHECK(!gate(&s, true, true, ta - 1u, ta + 10u));
	CHECK(!gate(&s, true, true, ta - 1u, ta - 1u + SHARED_PTT_QUIET_MS - 1u));
	CHECK(press_after(&s, true, ta - 1u, ta - 1u + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, ta - 1u, ta + 300u));
	// One good byte on an idle line, and no low within the bounce time: traffic. A low 50 ms after it, past the quiet
	// time, never presses (before, it did: a host that sent one byte and closed its port with a break keyed the radio)
	ta += 1000u;
	CHECK(!gate(&s, false, true, ta - 1u, ta));
	CHECK(!gate(&s, false, true, ta - 1u, ta + 10u));
	CHECK(!gate(&s, false, true, ta - 1u, ta + 40u));
	CHECK(!s.armed);
	CHECK(press_after(&s, true, ta - 1u, ta + SHARED_PTT_QUIET_MS, 10, SHARED_PTT_ARM_MS) == NEVER);
	rx = ta - 1u;
	(void)arm(&s, true, rx, ta + 2200u);

	// The millisecond clock wraps between the byte and the scan, and during the hold
	CHECK(!gate(&s, false, true, rx, 0xFFFFFF00u));
	CHECK(!gate(&s, false, true, 0xFFFFFFF0u, 0xFFFFFFF4u));	// a byte: the key's bounce, a low follows
	CHECK(!gate(&s, true, true, 0xFFFFFFF0u, 0xFFFFFFFCu));
	CHECK(press_after(&s, true, 0xFFFFFFF0u, 0xFFFFFFF0u + SHARED_PTT_QUIET_MS, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&s, false, true, 0xFFFFFFF0u, 0x100u));

	// Ten minutes of host traffic with late scans, starting armed: never a press, and the traffic disarms the gate. A
	// real press after the line has been high and quiet for the arm time still is one
	shared_ptt_t h = { 0 };
	(void)arm(&h, false, 0, 99000 - SHARED_PTT_ARM_MS);
	CHECK(host_traffic(&h, 100000, 600000) == 0u);
	CHECK(h.ignored > 0u && h.disarms > 0u);
	ta = arm(&h, true, traffic_rx, 700100);
	CHECK(press_after(&h, true, traffic_rx, ta, 10, 100) == SHARED_PTT_HOLD_MS);

	// The radio's own RF on the cable (a minute keyed, seen on the bench): a burst keyed the radio by the link, RF pulled
	// the line low with garbage flagged characters and no good byte, the gate took it for a press, and the press kept
	// the radio keyed. While the radio transmits, a new low never presses, however long
	shared_ptt_t r = { 0 };
	(void)arm(&r, true, 790000, 798000);
	keyed = true;
	for (uint32_t t = 800000; t < 860000; t += 10) {
		if (t % 170u < 10u) {
			flagged++;	// about 6 a second, as measured
		}
		CHECK(!gate(&r, true, true, 790000, t));
	}
	CHECK(r.ignored == 6000u);
	CHECK(r.armed);		// the line low while keyed is not traffic on a released line
	// TX over, the RF gone: the line reads high, and a press after it is one again
	keyed = false;
	CHECK(!gate(&r, false, true, 790000, 860000));
	CHECK(press_after(&r, true, 790000, 860010, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&r, false, true, 790000, 860200));

	// A press held when the radio keys stays held while it transmits: the key grounds the line, nothing arrives. Its own
	// break, and the key's bounce, while it settles, are not noise
	shared_ptt_t k = { 0 };
	(void)arm(&k, true, 0, 898000);
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
	// ... and, since RF and a cable's break can't be told apart, until the line has been quiet for the arm time
	CHECK(!k.armed);
	CHECK(press_after(&k, true, 0, 961120, 10, 100) == NEVER);
	CHECK(!gate(&k, false, true, 0, 961300));
	ta = arm(&k, true, 0, 961310);
	CHECK(press_after(&k, true, 0, ta, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&k, false, true, 0, ta + 100u));

	// The flagged count wraps (16 bits kept): a held press with no new character stays held across it
	flagged = 0xFFFFFFFEu;	// as far as the gate can tell, characters on an idle line: it wants the arm time again
	for (uint32_t t = ta + 200u; t < ta + 300u; t += 10) {
		CHECK(!gate(&k, false, true, 0, t));
	}
	CHECK(!k.armed);
	ta = arm(&k, true, 0, ta + 300u);
	CHECK(press_after(&k, true, 0, ta, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(gate(&k, true, true, 0, ta + 500u));
	flagged = 0x00000003u;
	// ... and characters on it with the radio not transmitting: the key is up, and the line picks up something that
	// isn't the radio's RF. Released, and no press until the line has been high and quiet for the arm time
	CHECK(!gate(&k, true, true, 0, ta + 510u));
	CHECK(!gate(&k, false, true, 0, ta + 520u));
	CHECK(!k.armed);
	CHECK(press_after(&k, true, 0, ta + 530u, 10, 100) == NEVER);

	// A good byte during a held press: the line carries data, so the key is up (a held key grounds it), keyed or not
	ta = arm(&k, true, 0, ta + 700u);
	CHECK(press_after(&k, true, 0, ta, 10, 100) == SHARED_PTT_HOLD_MS);
	keyed = true;
	CHECK(gate(&k, true, true, 0, ta + 500u));
	CHECK(!gate(&k, true, true, ta + 505u, ta + 510u));
	CHECK(!gate(&k, true, true, ta + 505u, ta + 600u));	// keyed: no new press on the low line
	keyed = false;
	CHECK(!gate(&k, false, true, ta + 505u, ta + 700u));
	CHECK(!k.armed);	// a good byte is a host, not RF

	// A late scan reports the press long after the pin went low: its break and bounce still settle from the report
	uint32_t krx = ta + 505u;
	ta = arm(&k, true, krx, ta + 900u);
	CHECK(!gate(&k, true, true, krx, ta));
	CHECK(gate(&k, true, true, krx, ta + SHARED_PTT_HOLD_MS + 40u));
	flagged++;
	CHECK(gate(&k, true, true, krx, ta + SHARED_PTT_HOLD_MS + 40u + SHARED_PTT_SETTLE_MS - 1u));
	CHECK(gate(&k, true, true, krx, ta + 500u));
	CHECK(!gate(&k, false, true, krx, ta + 600u));

	// The line latched low (gotchas.md 16.5). From power-on, as after a chip reset: never a press, stuck after
	// SHARED_PTT_STUCK_MS, released only by the pin reading high. Disarmed throughout (the line has never been high),
	// so this also exercises that the stuck diagnostic still accrues while disarmed (never a press either way)
	shared_ptt_t b = { 0 };
	for (uint32_t t = 1000; t < 601000; t += 10) {
		CHECK(!gate(&b, true, false, 0, t));
		CHECK(shared_ptt_stuck(&b, t) == (t >= 1000 + SHARED_PTT_STUCK_MS));
	}
	CHECK(!b.armed);
	CHECK(!gate(&b, false, false, 0, 601000));
	CHECK(!shared_ptt_stuck(&b, 601000));
	// A key held at power-on, let go: still disarmed (one high scan isn't the arm time), so no press yet
	CHECK(!gate(&b, true, false, 0, 601010));
	CHECK(!shared_ptt_stuck(&b, 601200));
	ta = arm(&b, false, 0, 601020);
	CHECK(press_after(&b, false, 0, ta, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!shared_ptt_stuck(&b, ta + 200u));

	// Latched at the end of host traffic: low from the last good byte on, past the quiet time
	shared_ptt_t e = { 0 };
	CHECK(!gate(&e, false, true, 1000, 1005));	// one high scan, well short of the arm time: stays disarmed
	for (uint32_t t = 2000; t < 62000; t += 10) {
		CHECK(!gate(&e, true, true, 1995, t));
	}
	CHECK(shared_ptt_stuck(&e, 62000));
	// A good byte: the line is alive after all
	CHECK(!gate(&e, true, true, 62001, 62010));
	CHECK(!shared_ptt_stuck(&e, 62010));

	// The clamp lands just after a scan saw the line high: low within the guard of the radio's own send. Never a press
	shared_ptt_t c = { 0 };
	CHECK(!gate(&c, false, true, 3000, 3005));	// one high scan, well short of the arm time: stays disarmed
	tx_seen = true;
	tx_ms = 3008;
	for (uint32_t t = 3010; t < 63010; t += 10) {
		CHECK(!gate(&c, true, true, 3000, t));
	}
	CHECK(shared_ptt_stuck(&c, 63010));
	CHECK(!gate(&c, false, true, 3000, 63020));
	CHECK(!shared_ptt_stuck(&c, 63020));
	tx_seen = false;
	// A key pressed well after the radio's send is a press, once armed, and a key held for a minute isn't stuck
	ta = arm(&c, true, 3000, 63030);
	CHECK(press_after(&c, true, 3000, ta, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(gate(&c, true, true, 3000, ta + 60000u));
	CHECK(!shared_ptt_stuck(&c, ta + 60000u));

	// Characters on a released line in two scans, then a low: the cable's (a key bounces once), never a press, until
	// the line has been high and quiet for the arm time
	shared_ptt_t q = { 0 };
	ta = arm(&q, false, 0, 1000000);
	flagged++;
	CHECK(!gate(&q, false, false, 0, ta));
	flagged++;
	CHECK(!gate(&q, false, false, 0, ta + 10u));
	CHECK(press_after(&q, false, 0, ta + 20u, 10, 2000) == NEVER);
	CHECK(!q.armed && q.disarms == 1u);
	CHECK(!gate(&q, false, false, 0, ta + 2100u));
	CHECK(press_after(&q, false, 0, ta + 2110u, 10, 100) == NEVER);	// high once is not the arm time
	ta = arm(&q, false, 0, ta + 2200u);
	CHECK(press_after(&q, false, 0, ta, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&q, false, false, 0, ta + 200u));
	// One scan with characters (a key's bounce caught high) then low at the next: a press
	flagged++;
	CHECK(!gate(&q, false, false, 0, ta + 1000u));
	CHECK(press_after(&q, false, 0, ta + 1010u, 10, 100) == SHARED_PTT_HOLD_MS);
	// Let go with a bounce, pressed again 200 ms later with another: a press (one bounce per edge is a key)
	flagged++;
	CHECK(!gate(&q, false, false, 0, ta + 1200u));
	flagged++;
	CHECK(!gate(&q, false, false, 0, ta + 1400u));
	CHECK(press_after(&q, false, 0, ta + 1410u, 10, 100) == SHARED_PTT_HOLD_MS);
	CHECK(!gate(&q, false, false, 0, ta + 1600u));
	CHECK(q.armed && q.disarms == 1u);
	// One scan with characters, and the next scan still high: not a bounce. Disarmed
	flagged++;
	CHECK(!gate(&q, false, false, 0, ta + 2000u));
	CHECK(!gate(&q, false, false, 0, ta + 2010u));
	CHECK(!gate(&q, false, false, 0, ta + 2040u));
	CHECK(!q.armed && q.disarms == 2u);
	// Characters at a press's edge, and again at its release 60 ms later: too soon for a key's two bounces. Disarmed
	ta = arm(&q, false, 0, ta + 2100u);
	flagged++;
	CHECK(press_after(&q, false, 0, ta, 10, 100) == SHARED_PTT_HOLD_MS);
	flagged++;
	CHECK(!gate(&q, false, false, 0, ta + 60u));
	CHECK(!q.armed && q.disarms == 3u);
	// Good bytes on two scans while released (a host talking) disarm the gate: a low 200 ms after the last one never
	// presses, the arm time runs from the last byte, and then a press is one
	ta = arm(&q, false, 0, ta + 100u);
	uint32_t tb = ta + 1000u;
	CHECK(!gate(&q, false, true, tb - 1u, tb));
	CHECK(!gate(&q, false, true, tb + 9u, tb + 10u));
	CHECK(!q.armed && q.disarms == 4u);
	CHECK(press_after(&q, true, tb + 9u, tb + 200u, 10, 3000) == NEVER);
	CHECK(!gate(&q, false, true, tb + 9u, tb + 3300u));
	ta = arm(&q, true, tb + 9u, tb + 3310u);
	CHECK(press_after(&q, true, tb + 9u, ta, 10, 100) == SHARED_PTT_HOLD_MS);

	// The dead PTT of the first try at this gate on top of the latch check (2026-11-21). A host talks, goes quiet, and
	// the line is high and quiet for well over the arm time. Then the key is pressed, and its contact's bounce makes a
	// good byte 1 ms before it closes, so the first scan after the byte already reads the pin low. The latch check
	// wants the pin seen high since the last good byte, so it took the press for a line clamped at the end of traffic,
	// and refused it for as long as it was held; a key whose bounce does that every time never keyed again. The byte
	// is the press's own: ignored for the quiet time, then a press, every time
	shared_ptt_t d = { 0 };
	uint32_t td = 3000000u, drx = td;
	for (; td < 3002000u; td += 10) {
		if (td % 30u == 0u) {
			drx = td - 1u;		// the greeting, every 30 ms
		}
		CHECK(!gate(&d, (td / 10u) % 3u == 0u, true, drx, td));
	}
	CHECK(!d.armed);
	td = arm(&d, true, drx, td + 100u) + 100u;
	for (int i = 0; i < 5; i++) {
		uint32_t bounce = td + 5u;	// the byte; the scan at td + 10 reads the pin low
		CHECK(!gate(&d, false, true, drx, td));
		uint32_t after = press_after(&d, true, bounce, td + 10u, 10, 3000);
		CHECK(after != NEVER && td + 10u + after <= bounce + SHARED_PTT_QUIET_MS + SHARED_PTT_HOLD_MS + 10u);
		CHECK(!shared_ptt_stuck(&d, td + 10u + after));
		drx = bounce;
		td += 500u;
	}
	CHECK(d.armed && d.disarms == 0u);

	// A host keys the radio over the link and closes its port with a break while it transmits: the line low from then
	// on, with one flagged character, which while keyed reads as the radio's own RF. The link's time-out ends the
	// transmission; the line still low must not key it again
	shared_ptt_t l = { 0 };
	uint32_t tl = arm(&l, false, 0, 4000000u);
	uint32_t lrx = tl;			// the request that keyed the radio
	CHECK(!gate(&l, false, true, lrx, tl + 1u));
	keyed = true;
	tl = arm(&l, true, lrx, tl + 100u);	// quiet for the arm time, transmitting
	flagged++;				// the break
	for (uint32_t t = tl; t < tl + 10000u; t += 10) {
		CHECK(!gate(&l, true, true, lrx, t));
	}
	keyed = false;				// the link's time-out
	CHECK(press_after(&l, true, lrx, tl + 10000u, 10, 10000) == NEVER);

	line_cases();
	random_worlds();

	if (failures == 0) {
		printf("sharedPttTest: all passed\n");
	}
	return failures != 0;
}
