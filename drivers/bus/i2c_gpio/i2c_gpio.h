// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// I2C master on two GPIO pins (bit-banged), for chips wired to plain pins (the board names them).
// Standard mode timing from the I2C-bus specification (NXP UM10204): START, 7-bit address + R/W, ACK per byte,
// repeated START for a register read, STOP. No clock stretching and a single master: SCL may be driven push-pull.
// SDA is open drain: sda(true) releases the line (a pull-up, or the pin's own, takes it high), sda(false) pulls it low.
#ifndef I2C_GPIO_H
#define I2C_GPIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bus/i2c_bus.h"

typedef struct {
	void (*scl)(bool high);
	void (*sda)(bool high);		// true = release (input), false = drive low
	bool (*sda_in)(void);		// the SDA line level
	void (*delay_us)(uint32_t us);
	uint32_t half_us;		// half a clock period: 5 = 100 kHz
} i2c_gpio_t;

// Write n bytes to the 7-bit address. True when the address and every byte were acknowledged
bool i2c_gpio_write(const i2c_gpio_t *bus, uint8_t addr7, const uint8_t *data, size_t n);
// Write wn bytes, repeated START, read rn bytes (ACK on all but the last). True when the writes were acknowledged
bool i2c_gpio_write_read(const i2c_gpio_t *bus, uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn);
// Nine clocks with SDA released and a STOP: frees a slave left holding SDA by an interrupted transfer (power-up)
void i2c_gpio_recover(const i2c_gpio_t *bus);

// The software master behind the generic interface (bus/i2c_bus.h): ctx is the i2c_gpio_t
bool i2c_gpio_bus_write(void *ctx, uint8_t addr7, const uint8_t *data, size_t n);
bool i2c_gpio_bus_write_read(void *ctx, uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn);
#define I2C_GPIO_BUS(gpio)	{ .write = i2c_gpio_bus_write, .write_read = i2c_gpio_bus_write_read, .ctx = (void *)(gpio) }

#endif
