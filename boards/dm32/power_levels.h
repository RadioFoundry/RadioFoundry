// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DM-32UV's five power levels (hardware-dm32.md 11.4): where each sits on the radio's own calibration rows, per
// band. The names are in board.c (power_names, power_names_vhf), in the same order
#ifndef DM32_POWER_LEVELS_H
#define DM32_POWER_LEVELS_H

#include <stdint.h>
#include <stdbool.h>
#include "cal/baofeng/baofeng_cal.h"

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

#endif
