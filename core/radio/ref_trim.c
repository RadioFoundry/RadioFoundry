// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Reference oscillator trim (ref_trim.h; docs/spec/radio-features.md 6.1)
#include "radio/ref_trim.h"

static volatile int16_t trim_hz;

int16_t ref_trim_clamp(int32_t hz)
{
	if (hz > REF_TRIM_MAX_HZ) {
		hz = REF_TRIM_MAX_HZ;
	} else if (hz < -REF_TRIM_MAX_HZ) {
		hz = -REF_TRIM_MAX_HZ;
	}
	int32_t half = hz < 0 ? -REF_TRIM_STEP_HZ / 2 : REF_TRIM_STEP_HZ / 2;
	return (int16_t)((hz + half) / REF_TRIM_STEP_HZ * REF_TRIM_STEP_HZ);
}

void radio_set_ref_trim(int32_t hz)
{
	trim_hz = ref_trim_clamp(hz);
	if (ref_trim_available()) {
		board.radio->ref_trim(trim_hz);
	}
}

int16_t radio_ref_trim(void)
{
	return trim_hz;
}
