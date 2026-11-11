// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR PA key-up from the power cal table (docs/spec/pa-keyup.md). A PA power loop takes its reference from the drive
// through a low-pass, and gives no output until the reference has added up to a fixed amount (the loop's control
// reaching the PA's knee). So a drive b, written with the output off, brings the output to a level's power after
//
//	T(d, b) = (knee * (top - dead) + slope * (d - dead)) / (b - dead)
//
// d is the level's drive, top the top level's and dead the drive under which the reference stays at zero; knee is
// the time the knee takes at the top drive and slope the extra time a level needs at its own drive. At b = d that is
// knee * (top - dead) / (d - dead) + slope: the top level is up after knee + slope, a low one takes far longer.
// Each level is keyed just in time from that one law:
//
//	T(d, d) <= window:	the drive goes to d that long before the first symbol, nothing else
//	otherwise:		a boost b for (window - settle), then d from settle before the first symbol; b is the
//				lowest drive that fits the window (T(d, b) = window - settle), at most
//				PA_KEYUP_BOOST_MAX_PCT of the top level's drive
//	the PA on		at pa_us before, or with the first drive when that comes later
//
// A loop that comes up fast runs past its level: the faster the approach, the further, and most at the low levels
// where the PA's output is steepest against the loop's control. ease (a drive over the dead zone, as a part of ref)
// is the fastest approach a board allows. A level over it comes up on the drive a = dead + ease, like a level of
// that drive, and takes its own drive once the loop regulates there, slope * (d - a) / (d - dead) before the first
// symbol: the time the law gives d to carry the loop from a's level to its own. It starts that much ahead of a's own
// key-up, before the window when it has to (the window is the boosts'): the board's earliest point bounds it. A boost
// stops at a as well
//
// So a level that is fast by itself leaves its time to the receiver, a slow one gets only the boost it needs (a
// short high boost overshoots with the timer's jitter), and no level starts earlier than the window.
//
// The constants hold no drive counts: they are times, and dead as a part of ref, a drive the radio's own power
// calibration gives at the transmit frequency (its lowest level's drive, or the drive where its table says the
// output reaches zero). top and d come from the same table. So one set of constants per band serves every radio of
// a design: what differs from radio to radio is in its calibration table, and nothing is stored per level or per
// frequency.
//
// A board with per-burst keying opts in: pa_keyup_init once with its constants and the earliest point its burst
// sequence can run, pa_keyup_plan at each key-up, then the points in its burst sequence. A board that doesn't call it
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
	uint16_t dead_pm;	// the drive that does nothing, in 1/1000 of ref: the loop's reference starts above it
	uint16_t knee_us;	// the top level's drive brings the loop to the PA's knee in this time
	uint16_t slope_us;	// and a level's own drive takes this much longer to bring it from there to the level
	uint16_t window_us;	// the earliest a key-up starts before the first symbol; a level slower than this is boosted
	uint16_t settle_us;	// a boost ends this long before the first symbol (under window_us)
	uint16_t pa_us;		// the PA on this long before, or with the first drive when that is later
	uint16_t ease_pm;	// the fastest approach: at most this drive over the dead zone, in 1/1000 of ref (0: no limit)
} pa_keyup_cfg_t;

// A board that keys its PA with the drive, one fixed time before the first symbol at every level
#define PA_KEYUP_FIXED(us)	{ .dead_pm = 0, .knee_us = 0, .slope_us = (us), .window_us = (us), .settle_us = 0, .pa_us = (us), .ease_pm = 0 }

// The boost's ceiling: a little over the top level, never far over what the cal table drives
#define PA_KEYUP_BOOST_MAX_PCT	120u

// All times before the first symbol
typedef struct {
	uint16_t boost;		// the first drive, 0 = none (the PA keyed at pa_us at d): over d a boost, under d an eased approach
	uint16_t drive;		// d
	uint16_t boost_us;	// the boost starts
	uint16_t pa_us;		// the PA on (never before the boost)
	uint16_t drive_us;	// the boost ends: the drive to d
} pa_keyup_t;

// The key-up for a level (pure): d the level's drive, top the table's top drive at the transmit frequency, ref the
// calibration's anchor for the dead zone there (0: none), max the board's drive limit. boost within max (under d
// only when eased), drive_us <= pa_us <= boost_us, and boost_us <= window_us unless eased
pa_keyup_t pa_keyup_drives(const pa_keyup_cfg_t *c, uint16_t d, uint16_t top, uint16_t ref, uint16_t max);

// A burst's points in time order, earliest first, for a board's burst sequence (a task's waits or a timer's steps).
// Without a boost one point: the PA keyed at d at pa_us. With one the boost, the PA and the drive to d. Points at the
// same time come in the enum's order
// PA_KEYUP_PT_BOARD: the first kind a board may give its own points (pa_keyup_insert)
typedef enum { PA_KEYUP_PT_BOOST, PA_KEYUP_PT_PA, PA_KEYUP_PT_DRIVE, PA_KEYUP_PT_BOARD } pa_keyup_kind_t;
typedef struct {
	uint16_t us;	// before the first symbol
	uint8_t kind;	// pa_keyup_kind_t
} pa_keyup_point_t;
#define PA_KEYUP_POINTS	3u
uint8_t pa_keyup_points(const pa_keyup_t *k, pa_keyup_point_t out[PA_KEYUP_POINTS]);

// A board's own point among them (a switch to transmit, a supply), in time order at us, ahead of the points at the same
// time (pure). pt has n points and room for one more; returns n + 1
#define PA_KEYUP_POINTS_MAX	(PA_KEYUP_POINTS + 1u)
uint8_t pa_keyup_insert(pa_keyup_point_t *pt, uint8_t n, uint16_t us, uint8_t kind);

// The points as a timer's steps (pure), for a board that times them from a reference event ref_us before the first
// symbol (an RF TX interrupt, a baseband's burst line): the points at one time in one step, in their order, the first
// step no earlier than min_us (the timer's shortest wait) after the reference. Computed at key-up: the timer's handler
// applies a step's points (pt[previous end] up to pt[end]), then waits for the next (pa_keyup_step_wait), or applies
// it at once when it is due within min_us. Returns the step count, 0 for no points
typedef struct {
	uint16_t us;	// the step's time before the first symbol
	uint8_t end;	// one past its last point in pt
} pa_keyup_step_t;
uint8_t pa_keyup_steps(const pa_keyup_point_t *pt, uint8_t n, uint16_t ref_us, uint16_t min_us,
		       pa_keyup_step_t out[PA_KEYUP_POINTS_MAX]);

// At the end of a step, in the timer's handler: the wait to the next step at next_us from the timer's base (the step
// it last counted from, base_us) when more than min_us are left after the elapsed_us the timer has counted since, else
// 0: the next step is due now and goes with this one, from the same base
static inline uint32_t pa_keyup_step_wait(uint16_t base_us, uint16_t next_us, uint32_t elapsed_us, uint16_t min_us)
{
	uint32_t step = (uint32_t)base_us - next_us;
	return elapsed_us + min_us < step ? step : 0u;
}

// The constants are in range: the window within max_ahead_us, the settle time under the window
bool pa_keyup_valid(const pa_keyup_cfg_t *c, uint16_t max_ahead_us);

// Once, by a board that keys this way: its constants (valid against max_ahead_us; kept by pointer, so static) and
// its drive limit
void pa_keyup_init(const pa_keyup_cfg_t *defaults, uint16_t max_ahead_us, uint16_t max_drive);
// A board whose bands have their own PA: the transmit band's constants (valid, static), before pa_keyup_plan. They are
// the board's from then on; constants the debug link set stay until it gives them back
void pa_keyup_select(const pa_keyup_cfg_t *defaults);
// The constants now; NULL when no board keys this way
const pa_keyup_cfg_t *pa_keyup_cfg(void);
// New constants (NULL: the board's); false when invalid or no board keys this way. The next key-up takes them
bool pa_keyup_set(const pa_keyup_cfg_t *c);
// The key-up with the constants now, kept as the last one for the debug link view. ref: the calibration's anchor for
// the dead zone at the transmit frequency (0: none). Without a board's constants: no boost and pa_us 0
pa_keyup_t pa_keyup_plan(uint16_t d, uint16_t top, uint16_t ref);
// The last one, with the top drive and the anchor it was planned against (either may be NULL)
pa_keyup_t pa_keyup_last(uint16_t *top, uint16_t *ref);
// The board's limits from pa_keyup_init: the earliest point and the highest drive
void pa_keyup_limits(uint16_t *max_ahead_us, uint16_t *max_drive);

#endif
