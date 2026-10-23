// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// LIS3DH-class 3-axis accelerometer: chip driver, independent of the board. From the public LIS3DH datasheet
// (STMicroelectronics, "LIS3DH MEMS digital output motion sensor", DocID17530): WHO_AM_I 0x33, CTRL_REG1 data rate and
// axis enables, CTRL_REG4 block-data-update and high-resolution mode, 12-bit left-justified output in OUT_X_L..OUT_Z_H.
//
// The board supplies a bus with one register write and one burst read (the chip's auto-increment sets the register's
// bit 7, which the driver does). Polled, no interrupts. Not reentrant: one task.
#ifndef LIS3DH_H
#define LIS3DH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define LIS3DH_WHO_AM_I_VALUE	0x33u
#define LIS3DH_WHO_AM_I_SC7A20	0x11u	// a register-compatible clone (found on some radios)

#define LIS3DH_REG_WHO_AM_I	0x0Fu
#define LIS3DH_REG_CTRL1	0x20u
#define LIS3DH_REG_CTRL4	0x23u
#define LIS3DH_REG_STATUS	0x27u
#define LIS3DH_REG_OUT_X_L	0x28u
#define LIS3DH_AUTO_INC		0x80u	// register address bit 7: the burst read steps to the next register

// CTRL_REG1: output data rate (bits 7:4) and the axis enables (bits 2:0)
#define LIS3DH_ODR_50HZ		0x40u
#define LIS3DH_AXES_XYZ		0x07u
// CTRL_REG4: block data update (a sample's two bytes stay together while read), high-resolution (12 bit), +-2 g
#define LIS3DH_BDU		0x80u
#define LIS3DH_HR		0x08u
// STATUS_REG
#define LIS3DH_ZYXDA		0x08u	// a new sample on all three axes

typedef struct {
	bool (*write)(void *ctx, uint8_t reg, uint8_t val);
	bool (*read)(void *ctx, uint8_t reg, uint8_t *buf, size_t n);	// reg is sent as given (auto-increment bit included)
	void *ctx;
} lis3dh_bus_t;

typedef struct {
	int16_t x, y, z;	// milli-g
} lis3dh_sample_t;

// True when a LIS3DH (or an SC7A20) answers its WHO_AM_I
bool lis3dh_probe(const lis3dh_bus_t *bus);
// 50 Hz, all axes, block data update, high resolution, +-2 g. False when the bus or the chip doesn't answer
bool lis3dh_init(const lis3dh_bus_t *bus);
// The latest sample. *fresh tells whether it is new since the last read (may be NULL). False on a bus error
bool lis3dh_read(const lis3dh_bus_t *bus, lis3dh_sample_t *s, bool *fresh);
// One axis's output byte pair as milli-g at +-2 g high resolution (12 bit, 1 mg per count)
int16_t lis3dh_to_mg(uint8_t lo, uint8_t hi);

#endif
