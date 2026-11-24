// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DM-32UV's five power levels (hardware-dm32.md 11.4): where each sits on the radio's own calibration rows, per
// band. The names are in board.c (power_names, power_names_vhf), in the same order
#ifndef DM32_POWER_LEVELS_H
#define DM32_POWER_LEVELS_H

#include <stdint.h>
#include <stdbool.h>
#include "cal/baofeng/baofeng_cal.h"
#include "radio/apc_ramp.h"

#define DM32_POWER_LEVELS	5u

// bf_cal_drive_pos positions, lowest level first. The lower four are the same power on both bands, the top is the
// high row: 100 mW, 1 W, 2.5 W, 5 W, 8 W (UHF) / 9 W (VHF)
static inline uint16_t dm32_level_pos(bool uhf, uint8_t level)
{
	static const uint16_t pos[2][DM32_POWER_LEVELS] = {
		{ 654, 810, 1058, 2000, BF_CAL_POS_HIGH },	// VHF
		{ 651, 804, 1050, 2129, BF_CAL_POS_HIGH },	// UHF
	};
	return pos[uhf ? 1 : 0][level < DM32_POWER_LEVELS ? level : DM32_POWER_LEVELS - 1u];
}

// The PA ramps (hardware-dm32.md 11.4): DAC B between the PA's turn-on edge and the level's drive on the shared APC
// ramp's curve, with the PA selected. The edge is a place on the rows too, under the 100 mW level's: under it the PA
// gives nothing, so the whole ramp is on air at every level, and its rate doesn't hang on how fast the PA loop is (at
// the top of the UHF band the loop rose in 0.09 ms where elsewhere it took 0.4-1.6 ms). A drive at or under the edge
// ramps from 0. FM keys up and down on the long ramp; a DMR burst ends on the short one
#define DM32_RAMP_EDGE_POS	600u
#define DM32_RAMP_STEPS		40u
#define DM32_RAMP_STEP_US	50u
#define DM32_DMR_RAMP_STEPS	8u
#define DM32_DMR_RAMP_STEP_US	25u

// DAC B at step 0..steps: up from the edge (step 0) to the drive (the last), or down from the drive to the edge
static inline uint16_t dm32_ramp_level(uint16_t drive, uint16_t edge, uint16_t step, uint16_t steps, bool up)
{
	uint16_t base = edge < drive ? edge : 0u;
	if (step > steps) {
		step = steps;
	}
	return (uint16_t)(base + apc_ramp_level((uint16_t)(drive - base), up ? step : (uint16_t)(steps - step), steps));
}

#endif
