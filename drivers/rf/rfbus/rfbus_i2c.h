// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An RF chip's register bus (rfbus.h) on a two-wire I2C bus: device address, 8-bit register, then the 16-bit value
// high byte first; a read is the register, a repeated START and two bytes. Each frame runs inside the critical section
// when one is given. A frame the chip doesn't acknowledge is counted in nacks, and its read returns 0
#ifndef RFBUS_I2C_H
#define RFBUS_I2C_H
#include "rfbus.h"
#include "bus/i2c_gpio/i2c_gpio.h"

typedef struct {
	rfbus_t bus;			// first: a chip driver gets &x.bus
	const i2c_gpio_t *i2c;
	uint8_t addr7;			// 7-bit device address
	uint32_t (*crit_enter)(void);	// optional, set after attach
	void (*crit_exit)(uint32_t saved);
	uint32_t nacks;			// frames not acknowledged (diagnostics)
} rfbus_i2c_t;

// Frees the bus (i2c_gpio_recover) and sets the ops
void rfbus_i2c_attach(rfbus_i2c_t *b, const i2c_gpio_t *i2c, uint8_t addr7, void (*delay_ms)(uint32_t ms));

#endif
