// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// LIS3DH-class accelerometer driver (lis3dh.h)
#include "lis3dh.h"

int16_t lis3dh_to_mg(uint8_t lo, uint8_t hi)
{
	// The 12-bit two's complement result sits in the top of the 16-bit register pair; the arithmetic shift keeps the sign
	int16_t raw = (int16_t)(((uint16_t)hi << 8) | lo);
	return (int16_t)(raw >> 4);
}

bool lis3dh_probe(const lis3dh_bus_t *bus)
{
	uint8_t id = 0;
	return bus->read(bus->ctx, LIS3DH_REG_WHO_AM_I, &id, 1) && (id == LIS3DH_WHO_AM_I_VALUE || id == LIS3DH_WHO_AM_I_SC7A20);
}

bool lis3dh_init(const lis3dh_bus_t *bus)
{
	if (!lis3dh_probe(bus)) {
		return false;
	}
	return bus->write(bus->ctx, LIS3DH_REG_CTRL4, LIS3DH_BDU | LIS3DH_HR) &&
		bus->write(bus->ctx, LIS3DH_REG_CTRL1, LIS3DH_ODR_50HZ | LIS3DH_AXES_XYZ);
}

bool lis3dh_read(const lis3dh_bus_t *bus, lis3dh_sample_t *s, bool *fresh)
{
	uint8_t st = 0, b[6];
	if (!bus->read(bus->ctx, LIS3DH_REG_STATUS, &st, 1) ||
	    !bus->read(bus->ctx, LIS3DH_REG_OUT_X_L | LIS3DH_AUTO_INC, b, sizeof(b))) {
		return false;
	}
	s->x = lis3dh_to_mg(b[0], b[1]);
	s->y = lis3dh_to_mg(b[2], b[3]);
	s->z = lis3dh_to_mg(b[4], b[5]);
	if (fresh != NULL) {
		*fresh = (st & LIS3DH_ZYXDA) != 0;
	}
	return true;
}
