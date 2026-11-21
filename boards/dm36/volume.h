// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef BOARD_VOLUME_H
#define BOARD_VOLUME_H
#include "radio/radio.h"	// RADIO_VOLUME_MAX

// The speaker uses fine gain at DAC 1 for quiet low steps. APRS keeps the DAC curve its decoder was tuned on.
static uint16_t board_volume_fm(unsigned step, bool aprs)
{
	unsigned s = step > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : step;
	unsigned index = s == 0 ? 0 : s - 1u;	// the amplifier gate mutes step 0
	if (aprs) {
		return (uint16_t)(0xB281u + index * 14u / (RADIO_VOLUME_MAX - 1u));
	}
	// Hold the DAC at 1 and advance the measured AF fine gain (bits 9:4) from 3 to 63.
	return (uint16_t)(0xB001u | ((3u + 4u * index) << 4));
}
#endif
