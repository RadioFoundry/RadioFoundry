// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Volume knob power switch read from the battery sense line (docs/spec/radio-features.md 5.5). On radios whose knob
// switch only breaks the battery divider while the MCU holds its own supply with a latch (docs/spec/radios.md), the
// battery reading drops to about 0 V the moment the knob turns off. A few low readings in a row mean "off"; one low
// reading (a TX load dip, an ADC glitch) doesn't. Pure logic: the board feeds it its unfiltered battery reading.
#ifndef RADIO_POWER_SWITCH_H
#define RADIO_POWER_SWITCH_H

#include <stdint.h>
#include <stdbool.h>

#define PWRSW_OFF_MV		5500	// below this the knob is off: far under a flat 2S pack (6.0 V critical)
#define PWRSW_SAMPLES		3	// consecutive low readings, at least PWRSW_SAMPLE_MS apart
#define PWRSW_SAMPLE_MS		10

typedef struct {
	uint8_t low_count;
	uint32_t low_ms;	// when the last low reading was counted
} pwrsw_t;

// One reading; true while the knob is on. Call it as often as the board's switch_on() is polled
bool pwrsw_on(pwrsw_t *s, uint32_t battery_mv, uint32_t now_ms);

#endif
