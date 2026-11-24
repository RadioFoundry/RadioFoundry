// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DM-32UV's five power levels (hardware-dm32.md 11.4): the drive each takes at a TX frequency, from the radio's
// own calibration rows or, for the lowest, a fixed drive over the PA's turn-on, and the PA ramps. The names are in
// board.c (power_names, power_names_vhf), in the same order
#ifndef DM32_POWER_LEVELS_H
#define DM32_POWER_LEVELS_H

#include <stdint.h>
#include <stdbool.h>
#include "cal/baofeng/baofeng_cal.h"
#include "radio/apc_ramp.h"

#define DM32_POWER_LEVELS	5u
#define DM32_UHF_FROM_HZ	280000000u	// the PA / LNA band split (8.5): the levels follow it

// The lowest level, 500 mW: a drive over the PA's turn-on, not a place on the rows. Under a drive of about 840-955
// (both bands) the PA gives nothing or creeps up over tens of milliseconds; that edge moves 2-3 counts per MHz while
// the low row moves by hundreds, so a place on the rows put it anywhere from 0.55 to 0.70 of the low row. UHF falls
// with the frequency, VHF is flat
#define DM32_FLOOR_UHF_400	1085u	// at 400 MHz
#define DM32_FLOOR_UHF_PER_10MHZ 26u	// less per 10 MHz over 400
#define DM32_FLOOR_VHF		925u

static inline uint16_t dm32_floor_drive(uint32_t hz)
{
	if (hz < DM32_UHF_FROM_HZ) {
		return DM32_FLOOR_VHF;
	}
	uint32_t over = hz > 400000000u ? (hz - 400000000u) / 100000u : 0u;	// 0.1 MHz over 400
	uint32_t less = over * DM32_FLOOR_UHF_PER_10MHZ / 100u;
	return (uint16_t)(less < DM32_FLOOR_UHF_400 ? DM32_FLOOR_UHF_400 - less : 0u);
}

// The levels over the lowest: 1 W, 2.5 W, 5 W, then the high row (8 W UHF, 9 W VHF). VHF: places on the rows
// (bf_cal_drive_pos). UHF: parts of the high row in thousandths, because there the low row doesn't follow the PA (it
// falls from 400 to 435 MHz and rises again) and the levels as places on it were 1-1.6 dB off over the band
static inline uint16_t dm32_level_drive(const bf_cal_power_points_t *rows, uint32_t hz, uint8_t level)
{
	static const uint16_t vhf_pos[DM32_POWER_LEVELS - 1u] = { 782, 1122, 1985, BF_CAL_POS_HIGH };
	static const uint16_t uhf_part[DM32_POWER_LEVELS - 1u] = { 418, 563, 754, 1000 };
	if (level == 0u) {
		return dm32_floor_drive(hz);
	}
	uint8_t i = (uint8_t)((level < DM32_POWER_LEVELS ? level : DM32_POWER_LEVELS - 1u) - 1u);
	uint16_t floor = dm32_floor_drive(hz);
	uint16_t drive = hz < DM32_UHF_FROM_HZ ? bf_cal_drive_pos(rows, vhf_pos[i])
		: (uint16_t)(((uint32_t)rows->high * uhf_part[i] + 500u) / 1000u);
	return drive > floor ? drive : floor;	// a radio with weak rows: never under the lowest level
}

// A level's drive at a TX frequency on this radio's held rows (bf_cal_power_held). Without calibration the lowest level
// keeps its fixed drive, and the others take the rows of the radio the levels were set on (its held rows at 440 and
// 155 MHz) but never more than the fallback drive (11.3, about 2 W), the most an uncalibrated radio was given before:
// 1 W stays under it, 2.5 W and up are the fallback
static inline uint16_t dm32_level_drive_at(uint32_t hz, uint8_t level)
{
	static const bf_cal_power_points_t typical_uhf = { .low = 1389, .mid = 1981, .high = 2912 };
	static const bf_cal_power_points_t typical_vhf = { .low = 1360, .mid = 1840, .high = 3024 };
	bf_cal_power_points_t rows;
	if (bf_cal_power_held(hz, &rows)) {
		return dm32_level_drive(&rows, hz, level);
	}
	uint16_t drive = dm32_level_drive(hz < DM32_UHF_FROM_HZ ? &typical_vhf : &typical_uhf, hz, level);
	uint16_t fallback = bf_cal_power_level(hz, 0);
	return drive < fallback ? drive : fallback;
}

// The PA ramps (hardware-dm32.md 11.4): DAC B in two parts on the shared APC ramp's curve, with the PA selected. The
// crawl runs between the start, DM32_RAMP_START_PCT of the knee (dark on every frequency), and the knee, the lowest
// level's drive or the level's own when lower: the PA's turn-on lies between them, and on a fast PA loop (the top of
// the UHF band) crossing it in a few steps put the adjacent channel 25-35 dB under the carrier. The rise runs between
// the knee and the level's drive. Up: the crawl, then the rise; down: backwards. FM keys up and down on the long
// ramps; a DMR burst ends on the short ones
#define DM32_RAMP_START_PCT	80u
#define DM32_RAMP_STEP_US	50u
#define DM32_CRAWL_STEPS	60u	// 3 ms
#define DM32_RISE_STEPS		40u	// 2 ms
#define DM32_DMR_RAMP_STEP_US	25u
#define DM32_DMR_CRAWL_STEPS	12u	// 0.3 ms
#define DM32_DMR_RISE_STEPS	8u	// 0.2 ms

static inline uint16_t dm32_ramp_start(uint16_t drive, uint16_t floor)
{
	uint16_t knee = floor < drive ? floor : drive;
	return (uint16_t)((uint32_t)knee * DM32_RAMP_START_PCT / 100u);
}

// DAC B at step i of crawl + rise steps up from the start (step 0) to the drive (the last)
static inline uint16_t dm32_ramp_level(uint16_t drive, uint16_t floor, uint16_t i, uint16_t crawl, uint16_t rise)
{
	uint16_t knee = floor < drive ? floor : drive;
	uint16_t start = dm32_ramp_start(drive, floor);
	if (i <= crawl) {
		return (uint16_t)(start + apc_ramp_level((uint16_t)(knee - start), i, crawl));
	}
	return (uint16_t)(knee + apc_ramp_level((uint16_t)(drive - knee), (uint16_t)(i - crawl), rise));
}

#endif
