// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DM-32UV's power levels (power_levels.h, hardware-dm32.md 11.4): the drives they give on the rows of the radio
// they were set on, the floor over the band, their order on any rows, the scaling with a radio's own rows, the
// uncalibrated fallback, and the PA ramps
#include <stdio.h>
#include <string.h>
#include "power_levels.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The held rows at 440 and 155 MHz of the radio the levels were measured on, and the drives there for 500 mW, 1 W,
// 2.5 W, 5 W and the top (8 W / 9 W)
static void test_measured(void)
{
	static const bf_cal_power_points_t uhf = { .low = 1389, .mid = 1981, .high = 2912 };
	static const bf_cal_power_points_t vhf = { .low = 1360, .mid = 1840, .high = 3024 };
	static const uint16_t want_uhf[DM32_POWER_LEVELS] = { 981, 1217, 1639, 2196, 2912 };
	static const uint16_t want_vhf[DM32_POWER_LEVELS] = { 925, 1064, 1419, 1833, 3024 };
	for (uint8_t l = 0; l < DM32_POWER_LEVELS; l++) {
		CHECK(dm32_level_drive(&uhf, 440000000u, l) == want_uhf[l]);
		CHECK(dm32_level_drive(&vhf, 155000000u, l) == want_vhf[l]);
	}
	// Past the last level: the top
	CHECK(dm32_level_drive(&uhf, 440000000u, DM32_POWER_LEVELS) == 2912);
}

// The floor: 1085 at 400 MHz down to 877 at 480 (and the lowest UHF frequencies at 400's), 925 on VHF
static void test_floor(void)
{
	CHECK(dm32_floor_drive(400025000u) == 1085 && dm32_floor_drive(390000000u) == 1085);
	CHECK(dm32_floor_drive(440000000u) == 981 && dm32_floor_drive(480000000u) == 877);
	CHECK(dm32_floor_drive(479975000u) == 878);
	CHECK(dm32_floor_drive(136000000u) == 925 && dm32_floor_drive(174000000u) == 925);
	for (uint32_t hz = 400000000u; hz < 480000000u; hz += 1000000u) {
		CHECK(dm32_floor_drive(hz + 1000000u) <= dm32_floor_drive(hz));
	}
}

// A calibration image with each band's low / mid / high rows a fixed step apart over the five frequency points
static void set_rows(uint8_t uhf_low, uint8_t uhf_mid, uint8_t uhf_high, uint8_t vhf_low, uint8_t vhf_mid,
		     uint8_t vhf_high)
{
	uint8_t img[BF_CAL_LEN];
	memset(img, 0, sizeof img);
	for (int i = 0; i < 5; i++) {
		img[0x40 + i] = (uint8_t)(uhf_low + i);
		img[0x45 + i] = (uint8_t)(uhf_mid + i);
		img[0x4A + i] = (uint8_t)(uhf_high - i);
		img[0xD0 + i] = (uint8_t)(vhf_low + i);
		img[0xD5 + i] = (uint8_t)(vhf_mid - i);
		img[0xDA + i] = (uint8_t)(vhf_high + i);
	}
	bf_cal_set_image(img, true);
}

// On any radio's rows: the levels rise and the top is the high row
static void test_order(void)
{
	static const uint32_t hz[] = { 136000000u, 146000000u, 174000000u, 400000000u, 445000000u, 449900000u,
				       480000000u };
	set_rows(92, 123, 176, 86, 116, 189);	// about the measured radio's rows
	for (unsigned i = 0; i < sizeof hz / sizeof hz[0]; i++) {
		bf_cal_power_points_t p;
		CHECK(bf_cal_power_held(hz[i], &p));
		for (uint8_t l = 1; l < DM32_POWER_LEVELS; l++) {
			CHECK(dm32_level_drive(&p, hz[i], l) > dm32_level_drive(&p, hz[i], (uint8_t)(l - 1u)));
		}
		CHECK(dm32_level_drive(&p, hz[i], DM32_POWER_LEVELS - 1u) == p.high);
	}
	// Weak rows (a high row of 2000 at 400 MHz puts 1 W at 836): no level falls under the lowest
	bf_cal_power_points_t weak = { .low = 900, .mid = 1300, .high = 2000 };
	for (uint8_t l = 1; l < DM32_POWER_LEVELS; l++) {
		CHECK(dm32_level_drive(&weak, 400000000u, l) >= dm32_floor_drive(400000000u));
		CHECK(dm32_level_drive(&weak, 155000000u, l) >= dm32_floor_drive(155000000u));
	}
}

// A radio whose PA needs a fifth more drive for each row gets about a fifth more at every level over the lowest
static void test_scaling(void)
{
	static const uint32_t hz[2] = { 146000000u, 435000000u };
	for (uint8_t b = 0; b < 2; b++) {
		set_rows(80, 110, 160, 80, 110, 160);
		bf_cal_power_points_t p, q;
		CHECK(bf_cal_power_held(hz[b], &p));
		set_rows(96, 132, 192, 96, 132, 192);
		CHECK(bf_cal_power_held(hz[b], &q));
		// The floor is a fixed drive, the same on both
		CHECK(dm32_level_drive(&p, hz[b], 0) == dm32_level_drive(&q, hz[b], 0));
		for (uint8_t l = 1; l < DM32_POWER_LEVELS; l++) {
			uint32_t x = dm32_level_drive(&p, hz[b], l), y = dm32_level_drive(&q, hz[b], l);
			CHECK(y * 100u >= x * 118u && y * 100u <= x * 122u);
		}
	}
}

#define RL(d, f, p, i, c, r)	dm32_ramp_level((d), (f), (p), (uint16_t)(i), (c), (r))

// The ramps from every level and at a drive under the floor (one set over the debug link): from the start through the
// knee to the drive, never back, with flat ends; the key-down's tail from the crawl's end, dark (under the drive where
// the measured radio's PA gave 5 mW or less there), to 0
static void test_ramp(void)
{
	static const bf_cal_power_points_t uhf = { .low = 1200, .mid = 1888, .high = 2560 };	// 479.975 MHz, held
	static const bf_cal_power_points_t vhf = { .low = 1360, .mid = 1840, .high = 3024 };	// 155 MHz, held
	static const uint16_t parts[2][2] = { { DM32_UP_CRAWL_STEPS, DM32_UP_RISE_STEPS },
					      { DM32_DOWN_CRAWL_STEPS, DM32_DOWN_RISE_STEPS } };
	static const uint8_t pct[2] = { DM32_UP_START_PCT, DM32_DOWN_END_PCT };
	static const uint32_t hz[3] = { 400025000u, 479975000u, 155000000u };
	static const uint16_t dark[3] = { 890, 790, 815 };
	for (uint8_t h = 0; h < 3; h++) {
		uint16_t fl = dm32_floor_drive(hz[h]);
		for (uint8_t l = 0; l <= DM32_POWER_LEVELS; l++) {
			const bf_cal_power_points_t *rows = hz[h] >= DM32_UHF_FROM_HZ ? &uhf : &vhf;
			uint16_t d = l < DM32_POWER_LEVELS ? dm32_level_drive(rows, hz[h], l) : 700u;
			// The key-down's crawl ends dark, and its tail goes on to 0; the key-up starts nearer the knee, where the
			// PA turns on
			uint16_t end = dm32_ramp_start(d, fl, DM32_DOWN_END_PCT);
			CHECK(end < dark[h]);
			CHECK(dm32_tail_level(end, 0) == end && dm32_tail_level(end, DM32_DOWN_TAIL_STEPS) == 0u);
			for (uint16_t i = 1; i <= DM32_DOWN_TAIL_STEPS; i++) {
				CHECK(dm32_tail_level(end, i) <= dm32_tail_level(end, (uint16_t)(i - 1u)));
			}
			for (uint8_t r = 0; r < 2; r++) {
				uint16_t st = dm32_ramp_start(d, fl, pct[r]);
				CHECK(st <= d);
				uint16_t c = parts[r][0], n = (uint16_t)(c + parts[r][1]);
				CHECK(RL(d, fl, pct[r], 0, c, parts[r][1]) == st && RL(d, fl, pct[r], n, c, parts[r][1]) == d);
				CHECK(RL(d, fl, pct[r], c, c, parts[r][1]) == (fl < d ? fl : d));	// the knee
				for (uint16_t i = 1; i <= n; i++) {
					CHECK(RL(d, fl, pct[r], i, c, parts[r][1]) >= RL(d, fl, pct[r], i - 1u, c, parts[r][1]));
				}
				// The crawl's ends are flat: its first step moves less than its middle ones
				uint16_t m = (uint16_t)(c / 2u);
				CHECK(RL(d, fl, pct[r], 1, c, parts[r][1]) - st <
				      RL(d, fl, pct[r], m, c, parts[r][1]) - RL(d, fl, pct[r], m - 1u, c, parts[r][1]));
			}
		}
	}
	// The key-up's crawl is the part the PA's turn-on needs (12 ms or more), and the edges keep the attack and release
	// times of ETSI EN 300 113 clauses 7.7.3 (25 ms) and 7.8.3 (20 ms) with 2 ms for the loop
	CHECK(DM32_UP_CRAWL_STEPS * DM32_UP_CRAWL_US >= 12000u);
	CHECK(DM32_UP_CRAWL_STEPS * DM32_UP_CRAWL_US + DM32_UP_RISE_STEPS * DM32_UP_RISE_US <= 23000u);
	CHECK(DM32_DOWN_CRAWL_STEPS * DM32_DOWN_CRAWL_US + DM32_DOWN_RISE_STEPS * DM32_DOWN_RISE_US +
	      DM32_DOWN_TAIL_STEPS * DM32_DOWN_TAIL_US <= 18000u);
	CHECK(DM32_UP_START_PCT > DM32_DOWN_END_PCT && DM32_UP_START_PCT < 100u);
	// The burst's fall ends inside the 1.5 ms of the power mask with the 0.3 ms after the last symbol's point
	CHECK(DM32_DMR_OFF_AFTER_US + DM32_DMR_FALL_US <= 1200u);
}

// Without calibration: the lowest level is its fixed drive, 1 W the measured radio's, the levels over it the fallback
// (1408), and the levels still rise or stay
static void test_uncalibrated(void)
{
	static const uint8_t none[BF_CAL_LEN];
	bf_cal_set_image(none, false);
	static const uint32_t hz[] = { 136000000u, 146000000u, 155000000u, 174000000u, 400025000u, 440000000u,
				       479975000u };
	for (unsigned i = 0; i < sizeof hz / sizeof hz[0]; i++) {
		CHECK(dm32_level_drive_at(hz[i], 0) == dm32_floor_drive(hz[i]));
		for (uint8_t l = 1; l < DM32_POWER_LEVELS; l++) {
			CHECK(dm32_level_drive_at(hz[i], l) >= dm32_level_drive_at(hz[i], (uint8_t)(l - 1u)));
			CHECK(dm32_level_drive_at(hz[i], l) <= 1408);
		}
		for (uint8_t l = 2; l < DM32_POWER_LEVELS; l++) {
			CHECK(dm32_level_drive_at(hz[i], l) == 1408);
		}
	}
	CHECK(dm32_level_drive_at(440000000u, 1) == 1217 && dm32_level_drive_at(155000000u, 1) == 1064);
}

int main(void)
{
	test_measured();
	test_floor();
	test_order();
	test_scaling();
	test_ramp();
	test_uncalibrated();
	if (failures) {
		printf("dm32PowerTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("dm32PowerTest: all passed\n");
	return 0;
}
