// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR PA key-up from the power cal table (docs/spec/pa-keyup.md). A PA power loop takes its reference from the drive
// through a low-pass, and gives no output until the reference has added up to a fixed amount (the loop's control
// reaching the PA's knee). From a low step's drive alone that takes milliseconds. So before each burst's first symbol
// the drive goes to a boost for a time that carries that amount, then back, early enough that the low-pass has
// forgotten the boost when the output appears (the output follows the reference, so what is left of the boost then
// is overshoot):
//
//	boost = boost% * top			from boost_us before the first symbol, for
//	t = (knee + slope * (d - dead)) / (boost - dead)
//	hold  = hold% * d			from then on
//	d					from drive_us before
//	the PA on				at pa_us before (its own point: with the boost, or later)
//
// d is the step's drive and top the table's top drive at the transmit frequency. dead is the drive under which the
// loop's reference stays at zero, knee the amount the reference must add up to (drive counts above dead times ms),
// slope the extra time per count a higher step needs (about the low-pass's time constant). Nothing is stored per step
// or per frequency.
//
// A board with per-burst keying opts in: pa_keyup_init once with its constants and the earliest point its burst
// sequence can run, pa_keyup_plan at each key-up, then the points in its burst sequence. A board that doesn't call it
// keys its PA as before. The constants are RAM: the debug link may change them (RADIO op 37) for tuning a radio, and
// a restart restores the board's.
//
// Threading: pa_keyup_set from the debug link task, pa_keyup_plan from the radio task at key-up. A key-up during a
// change may mix old and new constants: each set is checked whole, and a mixed one only times a point late.
#ifndef RADIO_PA_KEYUP_H
#define RADIO_PA_KEYUP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	uint16_t dead;		// drive counts that do nothing: the loop's reference starts above this
	uint16_t knee;		// what the reference adds up to before the PA gives output, counts above dead x ms
	uint16_t slope_us;	// more boost time per count of (d - dead), us x counts / the boost above dead
	uint8_t boost_pct;	// the boost, % of the top step's drive, 50..PA_KEYUP_BOOST_MAX_PCT
	uint8_t hold_pct;	// the hold after the boost, % of d, 0..100
	uint16_t boost_us;	// the boost from this long before the first symbol; 0 = no boost or hold
	uint16_t pa_us;		// the PA on this long before (at most boost_us)
	uint16_t drive_us;	// the drive to d this long before (at most boost_us)
} pa_keyup_cfg_t;

// The boost's ceiling: a little over the top step, never far over what the cal table drives
#define PA_KEYUP_BOOST_MAX_PCT	120u

typedef struct {
	uint16_t boost;		// 0 = none (then no hold either: the PA keyed at pa_us at d)
	uint16_t hold;
	uint16_t drive;		// d
	uint16_t hold_us;	// the boost ends (the hold starts) this long before the first symbol
} pa_keyup_t;

// The key-up for a step (pure): d the step's drive, top the table's top drive at the transmit frequency, max the
// board's drive limit. hold <= d, boost within max, drive_us <= hold_us <= boost_us
pa_keyup_t pa_keyup_drives(const pa_keyup_cfg_t *c, uint16_t d, uint16_t top, uint16_t max);

// The constants are in range, the points in order and none earlier than max_ahead_us
bool pa_keyup_valid(const pa_keyup_cfg_t *c, uint16_t max_ahead_us);

// Once, by a board that keys this way: its constants (valid against max_ahead_us; kept by pointer, so static) and
// its drive limit
void pa_keyup_init(const pa_keyup_cfg_t *defaults, uint16_t max_ahead_us, uint16_t max_drive);
// The constants now; NULL when no board keys this way
const pa_keyup_cfg_t *pa_keyup_cfg(void);
// New constants (NULL: the board's); false when invalid or no board keys this way. The next key-up takes them
bool pa_keyup_set(const pa_keyup_cfg_t *c);
// The key-up with the constants now, kept as the last one for the debug link view
pa_keyup_t pa_keyup_plan(uint16_t d, uint16_t top);
pa_keyup_t pa_keyup_last(void);

#endif
