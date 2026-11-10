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
//	hold  = hold% * d, above dead		from then on
//	d					from drive_us before
//	the PA on				at pa_us before (its own point: with the boost, or later)
//
// d is the step's drive and top the table's top drive at the transmit frequency. dead is the drive under which the
// loop's reference stays at zero, knee the amount the reference must add up to (drive counts above dead times ms),
// slope the extra time per count a higher step needs (about the low-pass's time constant). Nothing is stored per step
// or per frequency.
//
// A board with per-burst keying opts in: pa_keyup_init once with its constants and the earliest point its burst
// sequence can run, pa_keyup_plan at each key-up (pa_keyup_top gives the top drive), then the points in its burst
// sequence: pa_keyup_points in time order, or pa_keyup_steps for a one-shot timer from a burst edge. A board that doesn't call it
// keys its PA as before. The constants are RAM: the debug link may change them (RADIO op 37) for tuning a radio, and
// a restart restores the board's.
//
// Threading: pa_keyup_set from the debug link task, pa_keyup_plan from the radio task at key-up. Each set is checked
// whole and switched in whole, and a key-up takes one set: the plan and the board's points come from the same.
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

// The top step's drive at hz: the largest drive(hz, level) over the board's levels, 0 when none transmits there
uint16_t pa_keyup_top(uint16_t (*drive)(uint32_t hz, uint8_t level), uint32_t hz, uint8_t levels);

// A burst's points, each this long before the first symbol (pa-keyup.md 3). On a tie, in this order
typedef enum {
	PA_KEYUP_AT_BOARD,	// the board's own point (an RF switch to transmit), 0 = none
	PA_KEYUP_AT_BOOST,	// the drive to the boost
	PA_KEYUP_AT_PA,		// the PA on: at d without a boost, else the PA alone (the drive has its own points)
	PA_KEYUP_AT_HOLD,	// the drive to the hold
	PA_KEYUP_AT_DRIVE,	// the drive to d
} pa_keyup_at_t;

typedef struct {
	uint8_t at;		// pa_keyup_at_t
	uint16_t ahead_us;
} pa_keyup_point_t;

#define PA_KEYUP_POINTS		5u

// The points earliest first (pure). boost false or k without one: the board's point and the PA's alone. Returns how
// many
uint8_t pa_keyup_points(const pa_keyup_cfg_t *c, const pa_keyup_t *k, bool boost, uint16_t board_us,
			pa_keyup_point_t out[PA_KEYUP_POINTS]);

// For a board that times the points with a one-shot timer from an edge lead_us before the first symbol (the
// baseband's burst line): the points as steps after that edge, those at one time in one step (pure). A point at or
// before the edge goes at 1 us
typedef struct {
	uint16_t at_us;		// after the edge, rising
	uint16_t drive;		// with set_drive
	bool set_drive;
	bool pa_on;
} pa_keyup_step_t;

typedef struct {
	pa_keyup_step_t s[PA_KEYUP_POINTS];
	uint8_t n;		// at least 1: the PA's point
} pa_keyup_steps_t;

void pa_keyup_steps(const pa_keyup_cfg_t *c, const pa_keyup_t *k, bool boost, uint16_t lead_us, pa_keyup_steps_t *out);

// Once, by a board that keys this way: its constants (valid against max_ahead_us; kept by pointer, so static) and
// its drive limit
void pa_keyup_init(const pa_keyup_cfg_t *defaults, uint16_t max_ahead_us, uint16_t max_drive);
// The constants now; NULL when no board keys this way
const pa_keyup_cfg_t *pa_keyup_cfg(void);
// New constants (NULL: the board's); false when invalid or no board keys this way. The next key-up takes them
bool pa_keyup_set(const pa_keyup_cfg_t *c);
// The key-up with the constants now, kept as the last one for the debug link view. used (may be NULL): a copy of the
// constants it took, for the board's points (unchanged when no board keys this way)
pa_keyup_t pa_keyup_plan(uint16_t d, uint16_t top, pa_keyup_cfg_t *used);
pa_keyup_t pa_keyup_last(void);

#endif
