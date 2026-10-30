// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Reference oscillator trim (ref_trim.h; docs/spec/radio-features.md 6.1)
#include "radio/ref_trim.h"
#if CONFIG_REF_TRIM

static volatile int16_t trim_ppb;
static int16_t board_ppb;	// the trim the board has: 0 from its start

bool radio_set_ref_trim(int32_t ppb)
{
	trim_ppb = ref_trim_clamp(ppb);
	if (!ref_trim_available() || trim_ppb == board_ppb) {
		return false;	// unchanged: no retune (the settings are applied at every start)
	}
	board_ppb = trim_ppb;
	return board.radio->ref_trim(trim_ppb);
}

int16_t radio_ref_trim(void)
{
	return trim_ppb;
}
#endif
