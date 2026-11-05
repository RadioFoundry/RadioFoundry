// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PTT on a pin shared with the serial line (shared_ptt.h)
#include "input/sharedptt/shared_ptt.h"

bool shared_ptt_pressed(shared_ptt_t *s, bool pin_low, const shared_ptt_line_t *line, uint32_t now_ms)
{
	if (!pin_low) {
		s->state = SHARED_PTT_UP;
		return false;
	}
	if (s->state == SHARED_PTT_NOISE) {
		return false;
	}
	// A good byte means the line carries data, whatever the state: a held key would ground it. While the radio
	// transmits, only a press already held stays trusted
	bool data = line->rx_seen && (uint32_t)(now_ms - line->rx_ms) < SHARED_PTT_QUIET_MS;
	if (data || (line->keyed && s->state != SHARED_PTT_PRESSED)) {
		s->ignored++;	// serial data, or the radio's own transmission on the line
		s->state = SHARED_PTT_UP;
		return false;
	}
	if (s->state == SHARED_PTT_PRESSED) {
		uint16_t flagged = (uint16_t)line->flagged;
		if (flagged == s->flagged) {
			return true;
		}
		if ((uint32_t)(now_ms - s->low_ms) < SHARED_PTT_SETTLE_MS) {
			s->flagged = flagged;	// the press's own break or bounce
			return true;
		}
		// A character on a line the key should hold low: the key is up, the line is picking up noise
		s->state = SHARED_PTT_NOISE;
		if (s->noise_releases != 0xFFu) {
			s->noise_releases++;
		}
		return false;
	}
	if (s->state == SHARED_PTT_UP) {
		s->state = SHARED_PTT_LOW;
		s->low_ms = now_ms;
	}
	if ((uint32_t)(now_ms - s->low_ms) < SHARED_PTT_HOLD_MS) {
		return false;
	}
	s->state = SHARED_PTT_PRESSED;
	s->low_ms = now_ms;	// the settle time runs from the report, however late the scan
	s->flagged = (uint16_t)line->flagged;
	return true;
}
