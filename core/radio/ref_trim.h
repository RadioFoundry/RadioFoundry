// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Reference oscillator trim (Settings > Talk & listen > Freq trim; docs/spec/radio-features.md 6.1): parts per billion
// of the reference, -10000..+10000 (+-10.0 ppm) in 100 ppb (0.1 ppm) steps, for a radio whose reference is off. A
// fraction of the reference moves every carrier by the same fraction, so one value holds on every band. The board's
// hal_radio_t.ref_trim moves receive and transmit together; a radio without the hook has no setting.
//
// Threading: any task. RAM only: the UI stores the setting (settings_t.ref_trim_ppb) and applies it at start-up.
#ifndef RADIO_REF_TRIM_H
#define RADIO_REF_TRIM_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "config.h"

#define REF_TRIM_MAX_PPB	10000
#define REF_TRIM_STEP_PPB	100

// The radio has a trim (never without CONFIG_REF_TRIM, so its callers fold away)
static inline bool ref_trim_available(void)
{
	return CONFIG_REF_TRIM && board.radio != NULL && board.radio->ref_trim != NULL;
}

// What a trim moves a carrier at mhz by, in Hz (rounded to the nearest)
static inline int32_t ref_trim_hz_at(int32_t ppb, uint32_t mhz)
{
	int32_t n = ppb * (int32_t)mhz;
	return (n + (n < 0 ? -500 : 500)) / 1000;
}

// The frequency a board that trims by retuning programs its synthesizer with for hz: hz moved by ppb, rounded to the
// Hz (exact to about 1 Hz; 32-bit, so an interrupt can use it). Programming f x (1 + t) on a reference that runs
// (1 + e) puts the carrier at f x (1 + t + e): the trim cancels the error as a trimmed reference would
static inline uint32_t ref_trim_tune_hz(uint32_t hz, int32_t ppb)
{
	int32_t n = (int32_t)(hz / 10000u) * ppb;
	return hz + (uint32_t)((n + (n < 0 ? -50000 : 50000)) / 100000);
}

// Clamp to +-REF_TRIM_MAX_PPB, round to the nearest REF_TRIM_STEP_PPB
static inline int16_t ref_trim_clamp(int32_t ppb)
{
	if (ppb > REF_TRIM_MAX_PPB) {
		ppb = REF_TRIM_MAX_PPB;
	} else if (ppb < -REF_TRIM_MAX_PPB) {
		ppb = -REF_TRIM_MAX_PPB;
	}
	int32_t half = ppb < 0 ? -REF_TRIM_STEP_PPB / 2 : REF_TRIM_STEP_PPB / 2;
	return (int16_t)((ppb + half) / REF_TRIM_STEP_PPB * REF_TRIM_STEP_PPB);
}
#if CONFIG_REF_TRIM
// Set the trim (clamped and rounded) and hand it to the board
void radio_set_ref_trim(int32_t ppb);
// The trim last set (0 until the first call)
int16_t radio_ref_trim(void);
#else
static inline void radio_set_ref_trim(int32_t ppb) { (void)ppb; }
static inline int16_t radio_ref_trim(void) { return 0; }
#endif

#endif
