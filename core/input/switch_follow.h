// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When the input task asks the radio for the channel switch's position (hal_input_t.position, docs/spec/ui.md 3).
// A new position is asked for at once, and so is the position again when the zone changes (a zone key, the debug
// link), since a new zone starts on its first channel. The radio may not take it (transmitting, a hotspot, the
// codeplug not loaded yet), so every SWITCH_FOLLOW_RETRY_MS the task reads the radio's zone and position and asks
// again when the radio would take it, until the radio is on it or the zone is too short for it. Then it leaves the
// radio alone until the knob moves or the zone changes, so a scan stands. Pure, for the host test.
#ifndef INPUT_SWITCH_FOLLOW_H
#define INPUT_SWITCH_FOLLOW_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_FOLLOW_RETRY_MS	250u

typedef enum {
	SWITCH_IDLE,	// nothing to do
	SWITCH_ASK,	// ask the radio for want now
	SWITCH_CHECK,	// read the radio's zone, position and zone size, then switch_follow_check
} switch_step_t;

typedef struct {
	int8_t want;		// the switch's position, -1 none yet
	bool pending;		// asked for, and the radio not seen on it yet
	bool zone_known;	// zone holds the zone last seen loaded
	uint16_t zone;
	uint32_t checked_ms;
} switch_follow_t;

static inline void switch_follow_init(switch_follow_t *f)
{
	f->want = -1;
	f->pending = false;
	f->zone_known = false;
	f->zone = 0;
	f->checked_ms = 0;
}

// Each scan, with the switch's settled position (-1: none)
static inline switch_step_t switch_follow_step(switch_follow_t *f, int8_t pos, uint32_t now_ms)
{
	if (pos < 0) {
		return SWITCH_IDLE;
	}
	if (pos != f->want) {
		f->want = pos;
		f->pending = true;
		f->checked_ms = now_ms;
		return SWITCH_ASK;
	}
	if (now_ms - f->checked_ms >= SWITCH_FOLLOW_RETRY_MS) {
		f->checked_ms = now_ms;
		return SWITCH_CHECK;
	}
	return SWITCH_IDLE;
}

// After SWITCH_CHECK: the radio's zone index and position, the zone's size (0: none loaded) and whether the radio
// takes a position now (not transmitting, not a hotspot). True: ask again now
static inline bool switch_follow_check(switch_follow_t *f, uint16_t zone, uint16_t zone_pos, uint16_t zone_size,
	bool ready)
{
	if (zone_size == 0) {
		return false;	// nothing loaded yet: pending stays as it is
	}
	if (f->zone_known && zone != f->zone) {
		f->pending = true;
	}
	f->zone = zone;
	f->zone_known = true;
	if (!f->pending || !ready) {
		return false;
	}
	if (zone_pos == (uint16_t)f->want || (uint16_t)f->want >= zone_size) {
		f->pending = false;
		return false;
	}
	return true;
}

#endif
