// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The FM squelch on the chip's noise reading (radio-features.md 3.5), shared by the radio core and a second receiver
// that runs its own squelch (hal_rx2.h), so both open and close on the same signal at the same level and time
#ifndef RADIO_SQUELCH_H
#define RADIO_SQUELCH_H

#include <stdbool.h>
#include <stdint.h>

#define RADIO_SQUELCH_DEFAULT	5
#define RADIO_SQUELCH_MAX	9

// Software squelch on the chip's noise reading (1 dB units, larger = more noise). Level 0 is always open.
// Level 1..9 opens when noise < RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP, so 1 = 64 (loose) .. 9 = 40 (tight),
// and closes once noise stays RADIO_SQL_HYST_DB above that for RADIO_SQL_CLOSE_MS: RADIO_SQL_CLOSE_POLLS of the core's
// polls (RADIO_POLL_MS), or as many of a receiver's own as make that time.
#define RADIO_SQL_OPEN_L1	64
#define RADIO_SQL_DB_PER_STEP	3
#define RADIO_SQL_HYST_DB	3
#define RADIO_SQL_CLOSE_MS	50
#define RADIO_SQL_CLOSE_POLLS	2
// A carrier that goes: the noise jumps far over the line at once, while a fade crosses it by a little. A reading this
// far above the close line closes at once, which halves the squelch tail (radio-features.md 3.5)
#define RADIO_SQL_FAST_CLOSE_DB	12

// One noise reading against a level: whether a shut squelch opens, an open one holds, and an open one closes at once
typedef struct {
	bool opens;
	bool holds;
	bool fast;
} radio_squelch_read_t;

// A receiver's own squelch (radio_squelch_step)
typedef struct {
	bool open;
	uint8_t closing;	// readings in a row that didn't hold
} radio_squelch_t;

// The noise reading a level opens below (255 for level 0: always open)
static inline uint8_t radio_squelch_open_below(uint8_t level)
{
	if (level == 0) {
		return 255;
	}
	if (level > RADIO_SQUELCH_MAX) {
		level = RADIO_SQUELCH_MAX;
	}
	return (uint8_t)(RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP);
}

// tone_ok: the RX tone heard (true with none set)
static inline radio_squelch_read_t radio_squelch_read(uint8_t level, uint8_t noise, bool tone_ok)
{
	int below = radio_squelch_open_below(level);
	radio_squelch_read_t r;
	r.opens = tone_ok && (level == 0 || noise < below);
	r.holds = tone_ok && (level == 0 || noise < below + RADIO_SQL_HYST_DB);
	r.fast = level != 0 && noise >= below + RADIO_SQL_HYST_DB + RADIO_SQL_FAST_CLOSE_DB;
	return r;
}

// One poll of a receiver's own squelch, polled every poll_ms; returns whether it is open now
static inline bool radio_squelch_step(radio_squelch_t *s, radio_squelch_read_t r, uint32_t poll_ms)
{
	uint32_t polls = (RADIO_SQL_CLOSE_MS + poll_ms - 1u) / poll_ms;
	if (!s->open) {
		s->open = r.opens;
	} else if (r.holds) {
		s->closing = 0;
	} else if (r.fast || ++s->closing >= polls) {
		s->open = false;
	}
	if (!s->open) {
		s->closing = 0;
	}
	return s->open;
}

#endif
