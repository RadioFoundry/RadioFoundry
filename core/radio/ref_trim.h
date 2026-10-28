// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Reference oscillator trim (Settings > Talk & listen > Freq trim; docs/spec/radio-features.md 6.1): Hz at the carrier,
// -3000..+3000 in 100 Hz steps, for a radio whose reference is off. The board's hal_radio_t.ref_trim moves receive and
// transmit together; a radio without the hook has no setting.
//
// Threading: any task. RAM only: the UI stores the setting (settings_t.ref_trim_hz) and applies it at start-up.
#ifndef RADIO_REF_TRIM_H
#define RADIO_REF_TRIM_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

#define REF_TRIM_MAX_HZ		3000
#define REF_TRIM_STEP_HZ	100

// The radio has a trim
static inline bool ref_trim_available(void)
{
	return board.radio != NULL && board.radio->ref_trim != NULL;
}

// Clamp to +-REF_TRIM_MAX_HZ, round to the nearest REF_TRIM_STEP_HZ
int16_t ref_trim_clamp(int32_t hz);
// Set the trim (clamped and rounded) and hand it to the board
void radio_set_ref_trim(int32_t hz);
// The trim last set (0 until the first call)
int16_t radio_ref_trim(void);

#endif
