// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PTT on a pin shared with the serial line (shared_ptt.h)
#include "input/sharedptt/shared_ptt.h"

bool shared_ptt_pressed(shared_ptt_t *s, bool pin_low, bool rx_seen, uint32_t rx_ms, uint32_t now_ms)
{
	if (!pin_low) {
		s->low = false;
		return false;
	}
	if (rx_seen && (uint32_t)(now_ms - rx_ms) < SHARED_PTT_QUIET_MS) {
		s->ignored++;
		s->low = false;
		return false;
	}
	if (!s->low) {
		s->low = true;
		s->low_ms = now_ms;
	}
	return (uint32_t)(now_ms - s->low_ms) >= SHARED_PTT_HOLD_MS;
}
