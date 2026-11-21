// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PTT on a connector that may be empty (plug_ptt.h)
#include "input/plugptt/plug_ptt.h"

bool plug_ptt_pressed(plug_ptt_t *p, bool pin_low, uint32_t now_ms)
{
	if (!p->started) {
		p->started = true;
		p->start_ms = now_ms;
	}
	if (!p->settled && (uint32_t)(now_ms - p->start_ms) >= PLUG_PTT_SETTLE_MS) {
		p->settled = true;
	}
	if (pin_low || !p->settled) {
		p->high = false;	// a high while settling doesn't start the arm time
		return pin_low && p->armed;
	}
	if (!p->high) {
		p->high = true;
		p->high_ms = now_ms;
	}
	if ((uint32_t)(now_ms - p->high_ms) >= PLUG_PTT_ARM_MS) {
		p->armed = true;
	}
	return false;
}
