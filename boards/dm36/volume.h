// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef BOARD_VOLUME_H
#define BOARD_VOLUME_H
#include "radio/radio.h"	// RADIO_VOLUME_MAX

// The volume/power knob is read on ADC0 and is not in the audio path (hardware-dm36.md 9.3): the firmware sets the gain

// FM: the FD6818 AF gain field (register 0x48 bits 3:0, the upper bits as stock, hardware-dm36.md 9.1) spread evenly
// over the core's steps, 1 at the lowest audible step to 15 at the top; step 0 is muted by the amplifier gate. DMR keeps
// the shared HR_C7000 line-out curve
static uint16_t board_volume_fm(unsigned step)
{
	unsigned s = step > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : step;
	return (uint16_t)(0xB280u | (s == 0 ? 1u : 1u + (s - 1u) * 14u / (RADIO_VOLUME_MAX - 1u)));
}
#endif
