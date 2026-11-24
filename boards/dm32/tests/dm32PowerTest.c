// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DM-32UV's power levels (power_levels.h, hardware-dm32.md 11.4): the drives they give on the rows of the radio
// they were set on, their order on any rows, the scaling with a radio's own rows, the uncalibrated fallback, and the
// FM key-down ramp
#include <stdio.h>
#include <string.h>
#include "power_levels.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint16_t level(const bf_cal_power_points_t *p, bool uhf, uint8_t l)
{
	return bf_cal_drive_pos(p, dm32_level_pos(uhf, l));
}

// The rows at 445 and 146 MHz of the radio the levels were measured on, and the drives the wattmeter put at
// 100 mW, 1 W, 2.5 W, 5 W and the top (8 W / 9 W). 2.5 W lies between the 2 W and 5 W points of the sweep
static void test_measured(void)
{
	static const bf_cal_power_points_t uhf = { .low = 1467, .mid = 1964, .high = 2816 };
	static const bf_cal_power_points_t vhf = { .low = 1376, .mid = 1856, .high = 3024 };
	static const uint16_t want_uhf[DM32_POWER_LEVELS] = { 955, 1179, 1492, 2074, 2816 };
	static const uint16_t want_vhf[DM32_POWER_LEVELS] = { 900, 1115, 1404, 1856, 3024 };
	for (uint8_t l = 0; l < DM32_POWER_LEVELS; l++) {
		CHECK(level(&uhf, true, l) == want_uhf[l]);
		CHECK(level(&vhf, false, l) == want_vhf[l]);
	}
	// Past the last level: the top
	CHECK(level(&uhf, true, DM32_POWER_LEVELS) == 2816);
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

// On any radio's rows: the levels rise, the top is the high row, and VHF's 5 W is the mid row
static void test_order(void)
{
	static const uint32_t hz[] = { 136000000u, 146000000u, 174000000u, 400000000u, 445000000u, 449900000u,
				       480000000u };
	set_rows(92, 123, 176, 86, 116, 189);	// about the measured radio's rows
	for (unsigned i = 0; i < sizeof hz / sizeof hz[0]; i++) {
		bool uhf = hz[i] > 280000000u;
		bf_cal_power_points_t p;
		CHECK(bf_cal_power(hz[i], &p));
		for (uint8_t l = 1; l < DM32_POWER_LEVELS; l++) {
			CHECK(bf_cal_power_pos(hz[i], dm32_level_pos(uhf, l)) >
			      bf_cal_power_pos(hz[i], dm32_level_pos(uhf, (uint8_t)(l - 1u))));
		}
		CHECK(bf_cal_power_pos(hz[i], dm32_level_pos(uhf, DM32_POWER_LEVELS - 1u)) == p.high);
		CHECK(bf_cal_power_pos(hz[i], dm32_level_pos(uhf, DM32_POWER_LEVELS - 1u)) == bf_cal_power_level(hz[i], 8));
		CHECK(bf_cal_power_pos(hz[i], BF_CAL_POS_LOW) == bf_cal_power_level(hz[i], 1));
		if (!uhf) {
			CHECK(bf_cal_power_pos(hz[i], dm32_level_pos(false, 3)) == p.mid);
		}
	}
}

// A radio whose PA needs a fifth more drive for each row gets about a fifth more at every level
static void test_scaling(void)
{
	set_rows(80, 110, 160, 80, 110, 160);
	uint16_t a[DM32_POWER_LEVELS], b[DM32_POWER_LEVELS];
	for (uint8_t l = 0; l < DM32_POWER_LEVELS; l++) {
		a[l] = bf_cal_power_pos(435000000u, dm32_level_pos(true, l));
	}
	set_rows(96, 132, 192, 96, 132, 192);
	for (uint8_t l = 0; l < DM32_POWER_LEVELS; l++) {
		b[l] = bf_cal_power_pos(435000000u, dm32_level_pos(true, l));
		CHECK((uint32_t)b[l] * 100u >= (uint32_t)a[l] * 118u && (uint32_t)b[l] * 100u <= (uint32_t)a[l] * 122u);
	}
}

// The FM key-down from every level on the measured rows: from the level's drive, never up, down to the edge, which is
// under the 100 mW level and under the drive where that radio's PA gave nothing (925 on UHF, 875 on VHF)
static void test_ramp(void)
{
	static const bf_cal_power_points_t rows[2] = {
		{ .low = 1376, .mid = 1856, .high = 3024 },	// VHF, 146 MHz
		{ .low = 1467, .mid = 1964, .high = 2816 },	// UHF, 445 MHz
	};
	static const uint16_t dark[2] = { 875, 925 };
	for (uint8_t b = 0; b < 2; b++) {
		uint16_t edge = bf_cal_drive_pos(&rows[b], DM32_RAMP_EDGE_POS);
		CHECK(edge < dark[b] && edge < level(&rows[b], b != 0, 0));
		for (uint8_t l = 0; l < DM32_POWER_LEVELS; l++) {
			uint16_t d = level(&rows[b], b != 0, l);
			CHECK(dm32_ramp_down_level(d, edge, 0) == d);
			CHECK(dm32_ramp_down_level(d, edge, DM32_RAMP_STEPS) == edge);
			for (uint16_t i = 1; i <= DM32_RAMP_STEPS; i++) {
				CHECK(dm32_ramp_down_level(d, edge, i) <= dm32_ramp_down_level(d, edge, (uint16_t)(i - 1u)));
			}
			// The curve's ends are flat: the first and last steps move less than the middle ones
			uint16_t mid = (uint16_t)(DM32_RAMP_STEPS / 2u);
			CHECK(d - dm32_ramp_down_level(d, edge, 1) <
			      dm32_ramp_down_level(d, edge, (uint16_t)(mid - 1u)) - dm32_ramp_down_level(d, edge, mid));
		}
	}
	// A drive at or under the edge (a low drive set over the debug link) ramps down to 0
	CHECK(dm32_ramp_down_level(800, 880, 0) == 800 && dm32_ramp_down_level(800, 880, DM32_RAMP_STEPS) == 0);
}

int main(void)
{
	test_measured();
	test_order();
	test_scaling();
	test_ramp();
	static const uint8_t none[BF_CAL_LEN];
	bf_cal_set_image(none, false);
	for (uint8_t l = 0; l < DM32_POWER_LEVELS; l++) {
		CHECK(bf_cal_power_pos(146000000u, dm32_level_pos(false, l)) == bf_cal_power_level(146000000u, 0));
	}
	if (failures) {
		printf("dm32PowerTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("dm32PowerTest: all passed\n");
	return 0;
}
