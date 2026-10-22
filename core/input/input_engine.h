// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The gesture engine behind core/input, without the RTOS: physical key masks and encoder counts in, logical
// events out through a callback. input_task.c runs it every INPUT_SCAN_MS; the host tests drive it directly.
#ifndef INPUT_ENGINE_H
#define INPUT_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "input/input.h"

#define INPUT_SCAN_MS		10
#define INPUT_KEYS_MAX		32	// key map entries handled (key bits are 0..31)
#define INPUT_HOLD_MS		1500	// hold-to-activate: a key whose long action is EMERGENCY gives it only after this
					// (or the long press time, if longer; docs/ux/principles.md P7)

// The hold time that gives a key's long action
static inline uint32_t input_long_threshold(uint8_t long_action, uint32_t long_ms)
{
	return long_action == ACT_EMERGENCY && long_ms < INPUT_HOLD_MS ? INPUT_HOLD_MS : long_ms;
}

typedef void (*input_emit_t)(const input_event_t *ev, void *ctx);

typedef struct {
	uint32_t t_down;	// when the key went down
	uint32_t t_next;	// next REPEAT
	bool down;
	bool long_done;		// LONG already sent for this hold
} input_key_t;

typedef struct {
	const keymap_entry_t *keymap;
	uint8_t keymap_len;
	bool started;
	uint32_t raw_prev;	// last raw sample (debounce: a bit counts once two samples agree)
	uint32_t held;		// debounced mask
	uint32_t ignore;	// keys held at start-up, ignored until released (no TX from a PTT held at power-on)
	uint32_t last_activity_ms;
	uint16_t long_ms;	// long press threshold, 0 = INPUT_LONG_MS
	uint16_t repeat_ms;	// repeat period, 0 = INPUT_REPEAT_MS
	input_key_t keys[INPUT_KEYS_MAX];
} input_engine_t;

// Injected keys (the debug link's KEY): held until a time, then released by a later scan. The hold and the gap after
// it count scans as well as ms. A scan can run late, after a wait for a bus the keys share (the DM-32UV's key rows are
// LCD data lines, held for a whole redraw): measured in ms only, a short press expired before the debounce had seen
// it twice and was lost
#define INPUT_INJECT_MIN_SCANS	3	// two agreeing scans to debounce, and one to spare

typedef struct {
	uint32_t mask;
	uint32_t until;		// hal_millis() the hold ends
	uint32_t released_ms;
	uint8_t seen;		// scans that took the held keys
	uint8_t idle;		// scans since the release (start at INPUT_INJECT_MIN_SCANS)
	bool active;
} input_inj_state_t;

// Whether a new hold can start: nothing held, and the release gap_ms and INPUT_INJECT_MIN_SCANS scans ago
static inline bool input_inj_ready(const input_inj_state_t *s, uint32_t now, uint32_t gap_ms)
{
	return !s->active && now - s->released_ms >= gap_ms && s->idle >= INPUT_INJECT_MIN_SCANS;
}

static inline void input_inj_hold(input_inj_state_t *s, uint32_t mask, uint32_t until)
{
	s->mask = mask;
	s->until = until;
	s->seen = 0;
	s->active = true;
}

// One scan at now: an expired hold is released once INPUT_INJECT_MIN_SCANS scans took it. The keys this scan sees
static inline uint32_t input_inj_scan(input_inj_state_t *s, uint32_t now)
{
	if (s->active && (int32_t)(now - s->until) >= 0 && s->seen >= INPUT_INJECT_MIN_SCANS) {
		s->active = false;
		s->mask = 0;
		s->released_ms = now;
		s->idle = 0;
	}
	if (s->active) {
		if (s->seen < UINT8_MAX) {
			s->seen++;
		}
		return s->mask;
	}
	if (s->idle < UINT8_MAX) {
		s->idle++;
	}
	return 0;
}

void input_engine_init(input_engine_t *e, const keymap_entry_t *keymap, uint8_t keymap_len, uint32_t now_ms);
// One scan: raw key mask and encoder detents since the last call
// Gesture timing in ms (0 = the defaults); keys held now keep their LONG / REPEAT state
void input_engine_set_timing(input_engine_t *e, uint16_t long_ms, uint16_t repeat_ms);
void input_engine_step(input_engine_t *e, uint32_t raw, int8_t enc, uint32_t now_ms, input_emit_t emit, void *ctx);

#endif
