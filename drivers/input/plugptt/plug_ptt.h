// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PTT on a connector that may be empty. A press pulls the line low, but with nothing plugged in the line can rest low
// too, and that must not key the radio at power-on. Whatever plugs in holds the line high while idle, so a low counts
// as a press only once the line has read high for PLUG_PTT_ARM_MS. A line that is low from power-on never keys until
// it has idled high, so a press is a fall from idle, never a level found at start-up.
//
// Reads in the first PLUG_PTT_SETTLE_MS after the first call don't arm it: at start-up a line can read high while the
// pad, the connector's pull-up and whatever is plugged in come up, then fall to where it rests, which would read as a
// press held from power-on (docs/spec/gotchas.md 16.3). Pulling the plug while armed reads as a held press: the
// transmit timeout ends it (nothing does with the timeout off).
#ifndef PLUG_PTT_H
#define PLUG_PTT_H

#include <stdbool.h>
#include <stdint.h>

// Longer than a glitch on an empty connector's line, short against plugging in and reaching for the PTT
#define PLUG_PTT_ARM_MS		100u
// Longer than the lines take to settle after the key scan starts, short against reaching for the PTT after power-on
#define PLUG_PTT_SETTLE_MS	250u

typedef struct {
	uint32_t start_ms;	// the first call
	uint32_t high_ms;	// when the line was first read high after settling; valid while high
	bool started;
	bool settled;		// PLUG_PTT_SETTLE_MS have passed since the first call
	bool high;
	bool armed;		// the line has read high for PLUG_PTT_ARM_MS: something is plugged in
} plug_ptt_t;

// pin_low: the PTT line as read now. now_ms: a millisecond clock (wrapping)
bool plug_ptt_pressed(plug_ptt_t *p, bool pin_low, uint32_t now_ms);

#endif
