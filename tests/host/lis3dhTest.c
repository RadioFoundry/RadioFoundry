// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The LIS3DH driver (drivers/sensor/lis3dh) against a register model: identity check, the start-up configuration,
// the 12-bit left-justified output with its sign, the new-sample flag and bus failures
#include <stdio.h>
#include <string.h>
#include "sensor/lis3dh/lis3dh.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct {
	uint8_t reg[0x40];
	bool fail;
	int writes;
} chip;

static bool b_write(void *ctx, uint8_t reg, uint8_t val)
{
	(void)ctx;
	if (chip.fail) {
		return false;
	}
	chip.reg[reg & 0x3F] = val;
	chip.writes++;
	return true;
}

static bool b_read(void *ctx, uint8_t reg, uint8_t *buf, size_t n)
{
	(void)ctx;
	if (chip.fail) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		buf[i] = chip.reg[((reg & 0x3Fu) + ((reg & LIS3DH_AUTO_INC) ? i : 0)) & 0x3Fu];
	}
	return true;
}

static const lis3dh_bus_t bus = { b_write, b_read, NULL };

static void reset(void)
{
	memset(&chip, 0, sizeof(chip));
	chip.reg[LIS3DH_REG_WHO_AM_I] = 0x33;
}

// The chip's output for a milli-g value: 12 bits in the top of the pair
static void put_mg(int axis, int mg)
{
	uint16_t raw = (uint16_t)((int16_t)mg * 16);
	chip.reg[LIS3DH_REG_OUT_X_L + 2 * axis] = (uint8_t)raw;
	chip.reg[LIS3DH_REG_OUT_X_L + 2 * axis + 1] = (uint8_t)(raw >> 8);
}

int main(void)
{
	// Conversion
	CHECK(lis3dh_to_mg(0x00, 0x00) == 0);
	CHECK(lis3dh_to_mg(0x00, 0x40) == 1024);	// +1 g
	CHECK(lis3dh_to_mg(0x00, 0xC0) == -1024);	// -1 g
	CHECK(lis3dh_to_mg(0x10, 0x00) == 1);		// one count
	CHECK(lis3dh_to_mg(0xF0, 0xFF) == -1);
	CHECK(lis3dh_to_mg(0x0F, 0x00) == 0);		// the four unused low bits don't count

	// Probe and init
	reset();
	CHECK(lis3dh_probe(&bus));
	CHECK(lis3dh_init(&bus));
	CHECK(chip.reg[LIS3DH_REG_CTRL1] == 0x47 && chip.reg[LIS3DH_REG_CTRL4] == 0x88);
	reset();
	chip.reg[LIS3DH_REG_WHO_AM_I] = 0x11;	// SC7A20, the clone on the DM-1801 Pro
	CHECK(lis3dh_probe(&bus) && lis3dh_init(&bus));
	reset();
	chip.reg[LIS3DH_REG_WHO_AM_I] = 0x32;
	CHECK(!lis3dh_probe(&bus) && !lis3dh_init(&bus) && chip.writes == 0);	// not a LIS3DH: nothing is written
	reset();
	chip.fail = true;
	CHECK(!lis3dh_probe(&bus) && !lis3dh_init(&bus));

	// Read
	reset();
	put_mg(0, 12);
	put_mg(1, -980);
	put_mg(2, 1000);
	chip.reg[LIS3DH_REG_STATUS] = LIS3DH_ZYXDA;
	lis3dh_sample_t s;
	bool fresh = false;
	CHECK(lis3dh_read(&bus, &s, &fresh));
	CHECK(s.x == 12 && s.y == -980 && s.z == 1000 && fresh);
	chip.reg[LIS3DH_REG_STATUS] = 0;
	CHECK(lis3dh_read(&bus, &s, &fresh) && !fresh);
	CHECK(lis3dh_read(&bus, &s, NULL));
	chip.fail = true;
	CHECK(!lis3dh_read(&bus, &s, &fresh));

	printf("# lis3dhTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
