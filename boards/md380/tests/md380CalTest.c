// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MD-380's calibration reader (boards/md380/calibration_md380.c, hardware-md380.md 11) on a synthetic table with a
// real radio's frequency list: the points from the band ends, the odd points filled in, the interpolation, the DAC
// scaling, and the default band when the ends are unreadable
#include <stdio.h>
#include <string.h>
#include "md380.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint8_t flash[512];

bool stm32_spi_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len)
{
	if (reg < 1 || reg > 2 || offset + len > 256) {
		return false;
	}
	memcpy(buf, &flash[(reg - 1u) * 256u + offset], len);
	return true;
}

// The frequency list at 0x1B0 as two radios have it (issue #260): 8-byte entries, the band 401.0-479.9 MHz in the
// upper halves of the first and the last
static const uint8_t FREQ_LIST[0x48] = {
	0x00, 0x35, 0x10, 0x40, 0x00, 0x45, 0x11, 0x40, 0x00, 0x35, 0x10, 0x41, 0x00, 0x45, 0x11, 0x41,
	0x00, 0x35, 0x10, 0x42, 0x00, 0x45, 0x11, 0x42, 0x00, 0x35, 0x10, 0x43, 0x00, 0x45, 0x11, 0x43,
	0x00, 0x35, 0x10, 0x44, 0x00, 0x45, 0x11, 0x44, 0x00, 0x35, 0x10, 0x45, 0x00, 0x45, 0x11, 0x45,
	0x00, 0x35, 0x10, 0x46, 0x00, 0x45, 0x11, 0x46, 0x00, 0x35, 0x10, 0x47, 0x00, 0x45, 0x11, 0x47,
	0x00, 0x95, 0x99, 0x47, 0x00, 0x50, 0x99, 0x47,
};

// A row's even points from first, falling by step; the odd ones hold junk the reader must not use
static void row(uint16_t at, uint8_t first, uint8_t step)
{
	for (int i = 0; i < 9; i++) {
		flash[at + i] = (i & 1) ? 0xEE : (uint8_t)(first - (i / 2) * step);
	}
}

static void make_table(void)
{
	memset(flash, 0x80, sizeof(flash));
	flash[0x009] = 0x64;
	row(0x010, 200, 10);	// high power
	row(0x020, 100, 9);	// low power
	row(0x030, 120, 4);	// receiver tuning
	row(0x130, 90, 2);	// DMR modulation
	row(0x140, 80, 2);
	row(0x170, 70, 2);	// FM modulation
	row(0x180, 60, 2);
	memcpy(&flash[0x1B0], FREQ_LIST, sizeof(FREQ_LIST));
}

static uint16_t dac(uint8_t b)
{
	return (uint16_t)((b * 255u) >> 4);
}

int main(void)
{
	md380_cal_point_t c;

	make_table();
	CHECK(md380_cal_load());
	CHECK(strcmp(md380_cal_source(), "factory") == 0);

	// Odd points are the means of their neighbors, rounded down
	CHECK(md380_cal_byte(0x011) == 195);
	CHECK(md380_cal_byte(0x017) == 165);
	CHECK(md380_cal_byte(0x021) == 95);	// (100 + 91) / 2
	CHECK(md380_cal_byte(0x0E1) == 0x80);	// not a row
	CHECK(md380_cal_byte(0x009) == 0x64);

	// Points from 401.0 and 479.9 MHz by halving: 4010 4108 4207 4305 4404 4502 4601 4700 4799 (100 kHz)
	CHECK(md380_cal_at(440400000u, &c) && c.valid);	// point 4
	CHECK(c.power_dac[1] == dac(180));
	CHECK(c.power_dac[0] == dac(82));
	CHECK(c.rx_tune_dac == dac(112));
	CHECK(c.dmr_i == 86 && c.dmr_q == 76 && c.fm_i == 66 && c.fm_q == 56);
	CHECK(c.ref_tune == 0x64);
	CHECK(md380_cal_at(410800000u, &c) && c.power_dac[1] == dac(195));	// point 1, a filled one
	CHECK(md380_cal_at(470099999u, &c) && c.power_dac[1] == dac(165));	// point 7: 100 kHz units, truncated

	// 448.7 MHz is between points 4 (4404, 180) and 5 (4502, 175): 180 - 5 x 83 / 98, truncated toward 180
	CHECK(md380_cal_at(448700000u, &c));
	CHECK(c.power_dac[1] == dac(176));
	CHECK(c.power_dac[1] == 2805);

	// Clamped at both ends
	CHECK(md380_cal_at(400000000u, &c) && c.power_dac[1] == dac(200));
	CHECK(md380_cal_at(480000000u, &c) && c.power_dac[1] == dac(160));

	// Unreadable band ends: 400.0-470.0 MHz, points 50 or so apart (point 4 at 435.0)
	make_table();
	flash[0x1F3] = 0xFF;
	CHECK(md380_cal_load());
	CHECK(strcmp(md380_cal_source(), "factory/grid") == 0);
	CHECK(md380_cal_at(435000000u, &c) && c.power_dac[1] == dac(180));
	CHECK(md380_cal_at(470000000u, &c) && c.power_dac[1] == dac(160));

	// An erased table is refused, with safe values
	memset(flash, 0xFF, sizeof(flash));
	CHECK(!md380_cal_load());
	CHECK(!md380_cal_at(448700000u, &c) && !c.valid);

	if (failures) {
		printf("md380CalTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("md380CalTest: all passed\n");
	return 0;
}
