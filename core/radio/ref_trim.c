// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Reference oscillator trim (ref_trim.h; docs/spec/radio-features.md 6.1)
#include "radio/ref_trim.h"

static volatile int16_t trim_ppb;

int16_t ref_trim_clamp(int32_t ppb)
{
	if (ppb > REF_TRIM_MAX_PPB) {
		ppb = REF_TRIM_MAX_PPB;
	} else if (ppb < -REF_TRIM_MAX_PPB) {
		ppb = -REF_TRIM_MAX_PPB;
	}
	int32_t half = ppb < 0 ? -REF_TRIM_STEP_PPB / 2 : REF_TRIM_STEP_PPB / 2;
	return (int16_t)((ppb + half) / REF_TRIM_STEP_PPB * REF_TRIM_STEP_PPB);
}

void radio_set_ref_trim(int32_t ppb)
{
	trim_ppb = ref_trim_clamp(ppb);
	if (ref_trim_available()) {
		board.radio->ref_trim(trim_ppb);
	}
}

int16_t radio_ref_trim(void)
{
	return trim_ppb;
}
