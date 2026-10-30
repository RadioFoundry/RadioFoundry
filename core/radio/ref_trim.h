// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Reference oscillator trim (Settings > Talk & listen > Freq trim; docs/spec/radio-features.md 6.1): parts per billion
// of the reference, -25000..+25000 (+-25.0 ppm) in 100 ppb (0.1 ppm) steps, for a radio whose reference is off. A
// fraction of the reference moves every carrier by the same fraction, so one value holds on every band. The board's
// hal_radio_t.ref_trim moves receive and transmit together; a radio without the hook has no setting.
//
// Threading: any task. RAM only: the UI stores the setting (settings_t.ref_trim_ppb) and applies it at start-up.
#ifndef RADIO_REF_TRIM_H
#define RADIO_REF_TRIM_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

#define REF_TRIM_MAX_PPB	25000
#define REF_TRIM_STEP_PPB	100

// The radio has a trim
static inline bool ref_trim_available(void)
{
	return board.radio != NULL && board.radio->ref_trim != NULL;
}

// What a trim moves a carrier at mhz by, in Hz (rounded to the nearest)
static inline int32_t ref_trim_hz_at(int32_t ppb, uint32_t mhz)
{
	int32_t n = ppb * (int32_t)mhz;
	return (n + (n < 0 ? -500 : 500)) / 1000;
}

// Clamp to +-REF_TRIM_MAX_PPB, round to the nearest REF_TRIM_STEP_PPB
int16_t ref_trim_clamp(int32_t ppb);
// Set the trim (clamped and rounded) and hand it to the board
void radio_set_ref_trim(int32_t ppb);
// The trim last set (0 until the first call)
int16_t radio_ref_trim(void);

#endif
