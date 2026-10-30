// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Reference oscillator trim (ref_trim.h; docs/spec/radio-features.md 6.1)
#include "radio/ref_trim.h"

static volatile int16_t trim_ppb;

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
