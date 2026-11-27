// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When the input task asks the radio for the channel switch's position (hal_input_t.position, docs/spec/ui.md 3).
// A new position is asked for at once, and so is the position again when the zone changes (a zone key, the debug
// link), since a new zone starts on its first channel. The radio may not take it (transmitting, a hotspot, the
// codeplug not loaded yet), so every SWITCH_FOLLOW_RETRY_MS the task reads the radio's zone and position and asks
// again when the radio would take it, until the radio is on it or the zone is too short for it. A position the radio
// keeps refusing while it could take one (a channel it can't read) is given up after SWITCH_FOLLOW_ASKS asks, since
// every ask stops a scan and an APRS transmission and resets the battery saver. Then it leaves the radio alone until
// the knob moves or the zone changes (another zone, or the same one with another size after a codeplug write), so a
// scan stands. A host that picks the zone and position (core/net/radio_ctl.c) holds it: the radio is left on the
// host's choice, in that zone too, until the knob moves (switch_follow_hold). Pure, for the host test.
#ifndef INPUT_SWITCH_FOLLOW_H
#define INPUT_SWITCH_FOLLOW_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_FOLLOW_RETRY_MS	250u
#define SWITCH_FOLLOW_ASKS	4u	// asks again while the radio could take the position
#define SWITCH_HOLD_MS		2000u	// a hold waits this long for the radio to reach the host's zone

typedef enum {
	SWITCH_IDLE,	// nothing to do
	SWITCH_ASK,	// ask the radio for want now
	SWITCH_CHECK,	// read the radio's zone, position and zone size, then switch_follow_check
} switch_step_t;

typedef struct {
	int8_t want;		// the switch's position, -1 none yet
	bool pending;		// asked for, and the radio not seen on it yet
	bool zone_known;	// zone and size hold the zone last seen loaded
	uint8_t asks;		// asked again since pending was set
	uint16_t zone;
	uint16_t size;
	uint32_t checked_ms;
	bool hold;		// a host chose the zone and position: leave them until the knob moves
	uint16_t hold_zone;
	uint32_t hold_ms;
} switch_follow_t;

static inline void switch_follow_init(switch_follow_t *f)
{
	f->want = -1;
	f->pending = false;
	f->zone_known = false;
	f->asks = 0;
	f->zone = 0;
	f->size = 0;
	f->checked_ms = 0;
	f->hold = false;
	f->hold_zone = 0;
	f->hold_ms = 0;
}

// A host asked for a channel in zone (a select or a step): the knob's position isn't asked for there, and the zone
// change it brings doesn't count as one, until the knob moves
static inline void switch_follow_hold(switch_follow_t *f, uint16_t zone, uint32_t now_ms)
{
	f->hold = true;
	f->hold_zone = zone;
	f->hold_ms = now_ms;
	f->pending = false;
}

// Each scan, with the switch's settled position (-1: none)
static inline switch_step_t switch_follow_step(switch_follow_t *f, int8_t pos, uint32_t now_ms)
{
	if (pos < 0) {
		return SWITCH_IDLE;
	}
	if (pos != f->want) {
		f->hold = false;	// the knob moved: it wins again
		f->want = pos;
		f->pending = true;
		f->asks = 0;
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
	if (f->hold) {
		if (zone != f->hold_zone && (f->checked_ms - f->hold_ms) < SWITCH_HOLD_MS) {
			return false;	// the radio isn't in the host's zone yet
		}
		f->hold = false;
		if (zone == f->hold_zone) {
			f->zone = zone;	// the host's zone is the one the knob's choice was made for: no zone change
			f->size = zone_size;
			f->zone_known = true;
			f->pending = false;
			return false;
		}
	}
	if (f->zone_known && (zone != f->zone || zone_size != f->size)) {
		f->pending = true;	// a position given up as past the end may fit now
		f->asks = 0;
	}
	f->zone = zone;
	f->size = zone_size;
	f->zone_known = true;
	if (!f->pending || !ready) {
		return false;
	}
	if (zone_pos == (uint16_t)f->want || (uint16_t)f->want >= zone_size) {
		f->pending = false;
		return false;
	}
	if (f->asks >= SWITCH_FOLLOW_ASKS) {
		f->pending = false;	// refused while ready: give up until the knob or the zone moves
		return false;
	}
	f->asks++;
	return true;
}

#endif
