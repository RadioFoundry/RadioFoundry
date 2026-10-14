// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Volume knob power switch from the battery sense line (power_switch.h)
#include "radio/power_switch.h"

bool pwrsw_on(pwrsw_t *s, uint32_t battery_mv, uint32_t now_ms)
{
	if (battery_mv >= PWRSW_OFF_MV) {
		s->low_count = 0;
		return true;
	}
	if (s->low_count == 0 || (uint32_t)(now_ms - s->low_ms) >= PWRSW_SAMPLE_MS) {
		if (s->low_count < PWRSW_SAMPLES) {
			s->low_count++;
		}
		s->low_ms = now_ms;
	}
	return s->low_count < PWRSW_SAMPLES;
}
