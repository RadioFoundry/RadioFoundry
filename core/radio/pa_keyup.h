// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR PA key-up from the power cal table (docs/spec/pa-keyup.md): before each burst's first symbol the PA drive goes
// to a boost with the PA still off, then to a hold, then the PA comes on at the step's own drive. Both drives come from
// the step's drive d and the table's lowest and top drives at the transmit frequency:
//
//	boost = d + blend% * (top% * top - d)	from boost_us before the first symbol
//	hold  = max(lowest, hold% * d)		from hold_us before
//	d, the PA on				from pa_us before
//
// A low step's PA control comes up slowly from a small drive; the boost speeds it up by an amount that shrinks as
// d nears the top, so every step is up by the first symbol without a per-step table.
//
// A board with per-burst keying opts in: pa_keyup_init once with its constants and the earliest point its burst
// sequence can run, pa_keyup_plan at each key-up, then the three points in its burst sequence. A board that doesn't
// call it keys its PA as before. The constants are RAM: the debug link may change them (RADIO op 37) for tuning a
// radio, and a restart restores the board's.
//
// Threading: pa_keyup_set from the debug link task, pa_keyup_plan from the radio task at key-up. A key-up during a
// change may mix old and new constants: each set is checked whole, and a mixed one only times a point late.
#ifndef RADIO_PA_KEYUP_H
#define RADIO_PA_KEYUP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	uint8_t blend_pct;	// the boost's share of the way from d to the boost target, 0..100
	uint8_t top_pct;	// the boost target, % of the top step's drive, 50..PA_KEYUP_TOP_MAX_PCT
	uint8_t hold_pct;	// the hold, % of d (at least the lowest step's drive), 0..100
	uint16_t boost_us;	// the boost from this long before the first symbol; 0 = no boost or hold
	uint16_t hold_us;	// the hold from this long before (at most boost_us)
	uint16_t pa_us;		// the PA on at d this long before (at most hold_us)
} pa_keyup_cfg_t;

// The boost target's ceiling: a little over the top step, never far over what the cal table drives
#define PA_KEYUP_TOP_MAX_PCT	120u

typedef struct {
	uint16_t boost;		// 0 = none (then no hold either)
	uint16_t hold;
	uint16_t drive;		// d
} pa_keyup_t;

// The drives for a key-up (pure): d the step's drive, lowest and top the table's drives at the transmit frequency, max
// the board's drive limit. boost >= d and hold <= d, both within max
pa_keyup_t pa_keyup_drives(const pa_keyup_cfg_t *c, uint16_t d, uint16_t lowest, uint16_t top, uint16_t max);

// The constants are in range, the points in order and none earlier than max_ahead_us
bool pa_keyup_valid(const pa_keyup_cfg_t *c, uint16_t max_ahead_us);

// Once, by a board that keys this way: its constants (valid against max_ahead_us; kept by pointer, so static) and
// its drive limit
void pa_keyup_init(const pa_keyup_cfg_t *defaults, uint16_t max_ahead_us, uint16_t max_drive);
// The constants now; NULL when no board keys this way
const pa_keyup_cfg_t *pa_keyup_cfg(void);
// New constants (NULL: the board's); false when invalid or no board keys this way. The next key-up takes them
bool pa_keyup_set(const pa_keyup_cfg_t *c);
// The drives with the constants now, kept as the last key-up's for the debug link view
pa_keyup_t pa_keyup_plan(uint16_t d, uint16_t lowest, uint16_t top);
pa_keyup_t pa_keyup_last(void);

#endif
