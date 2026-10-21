// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 factory calibration as the radio code consumes it (docs/spec/hardware-gd77.md 5.5). The storage side reads the
// calibration blocks and provides gd77_cal_at(); radio_gd77.c holds a weak stand-in that reports "no calibration" with
// the safe values below, so the board builds and receives without it. Without a valid point tx() refuses.
//
// The field layout of the calibration blocks isn't decoded yet, so this is the radio code's view only: one point,
// interpolated at a frequency by whoever reads the blocks. Everything here is unverified until a radio is on the bench.
#ifndef GD77_CAL_H
#define GD77_CAL_H

#include <stdint.h>
#include <stdbool.h>

#define GD77_CAL_POWER_LEVELS	2	// board.power_count: low, high

// Safe values when nothing is loaded: no drive at all, and the HR_C6000 modulation pair at mid scale
#define GD77_CAL_SAFE_DRIVE	0u
#define GD77_CAL_SAFE_GAIN	0x80u

typedef struct {
	bool valid;		// read and checked; false: the safe values, and tx() refuses
	// DAC0 drive per board power level while transmitting, in the 8-bit unit the radio uses (DAC0 = value x 16)
	uint8_t power_drive[GD77_CAL_POWER_LEVELS];
	// DAC0 while receiving, same unit (role unverified: the receive path sets DAC0 per channel too). rx_drive_valid
	// false: DAC0 stays as it is in receive
	bool rx_drive_valid;
	uint8_t rx_drive;
	uint8_t fm_mod2, fm_mod1;	// HR_C6000 4:0x45 / 4:0x46 for FM transmit
	uint8_t dmr_mod2, dmr_mod1;	// ... for DMR
	int8_t ref_offset;		// reference oscillator tune - 128 (4:0x47 / 4:0x04)
} gd77_cal_point_t;

// The calibration at hz. False when none is loaded or hz is outside it: out then holds the safe values
bool gd77_cal_at(uint32_t hz, gd77_cal_point_t *out);

// The safe values (no calibration)
static inline void gd77_cal_safe(gd77_cal_point_t *out)
{
	*out = (gd77_cal_point_t){
		.valid = false,
		.power_drive = { GD77_CAL_SAFE_DRIVE, GD77_CAL_SAFE_DRIVE },
		.rx_drive_valid = false,
		.rx_drive = 0,
		.fm_mod2 = GD77_CAL_SAFE_GAIN, .fm_mod1 = GD77_CAL_SAFE_GAIN,
		.dmr_mod2 = GD77_CAL_SAFE_GAIN, .dmr_mod1 = GD77_CAL_SAFE_GAIN,
		.ref_offset = 0,
	};
}

#endif
