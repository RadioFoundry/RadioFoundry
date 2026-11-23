// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 HR_C6000 board values (docs/spec/hardware-gd77.md 5.2, 8): what this radio sets on top of the shared power-on
// sequence (modem-hrc6000.md 3) and how it switches the codec for PCM. Unverified on a radio.
#ifndef GD77_C6K_H
#define GD77_C6K_H

#include <stdint.h>
#include <stdbool.h>
#include "modem/hrc_common/hrc.h"

// The microphone on the codec's MIC_P input; 0xE4 = 0x40 (LINEOUT +2 dB, first mic stage 0 dB) + the gain (0..15)
#define GD77_MIC_INPUT		HRC_MIC_MIC_P
#define GD77_MIC_GAIN_BASE	0x40u
#define GD77_MIC_GAIN		11u

// A board register value: page, register, value
typedef struct {
	uint8_t page;
	uint8_t reg;
	uint8_t val;
} gd77_c6k_row_t;

extern const gd77_c6k_row_t gd77_c6k_rows[];
extern const uint8_t gd77_c6k_row_count;

// After hrc6000_power_on_init_clock: the board's microphone wiring, auxiliary tables and register values
bool gd77_c6k_board_init(hrc_t *dev);
// The codec set for PCM playback: the DAC to the line out (0xE2 bit 1), LINEOUT2 on (0xE0 bit 3); in DMR also the
// I2S bits of 0x22 kept set
bool gd77_c6k_pcm_out(hrc_t *dev, bool dmr);

#endif
