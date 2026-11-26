// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Turns the ADC reading of a volume potentiometer into a knob position from 0 to 100 % (docs/spec/radio-features.md 4.2).
// The potentiometer is logarithmic, so one straight line doesn't fit it. Two do: from the reading at the bottom to the
// reading at mid-rotation is 0 to 50 %, and from there to the reading at the top is 50 to 100 %. The board supplies the
// readings; nothing here touches hardware.
#ifndef RADIO_VOLUME_POT_H
#define RADIO_VOLUME_POT_H

#include <stdint.h>
#include <stdbool.h>

// A silent knob has to be turned this many counts past the mute zone before it counts as turned up. Without that gap,
// noise on a knob left at the edge would switch the position between 0 and 1
#define VOLUME_POT_HYSTERESIS	5
// For mute: this radio's knob doesn't silence the speaker at the bottom (or nobody has checked), so never report 0
#define VOLUME_POT_NO_MUTE	(-1)

typedef struct {
	int16_t min, mid, max;	// ADC readings with the knob at the bottom, at mid-rotation and at the top
	int16_t mute;		// readings up to min + mute are silent (0 %), or VOLUME_POT_NO_MUTE
} volume_pot_cal_t;

typedef struct {
	bool turned_up;		// the knob is above the silent zone. Zero it at start-up: the knob counts as silent
} volume_pot_t;

int8_t volume_pot_percent(volume_pot_t *s, const volume_pot_cal_t *cal, int32_t raw);

#endif
