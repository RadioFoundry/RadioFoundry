// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bit-banged I2C master (i2c_gpio.h). SDA only changes while SCL is low, except for START and STOP.
#include "i2c_gpio.h"

static void wait(const i2c_gpio_t *b)
{
	b->delay_us(b->half_us);
}

static void start(const i2c_gpio_t *b)
{
	// From idle (both high) or after a byte (SCL low, SDA released): SDA high, SCL high, then SDA falls
	b->sda(true);
	wait(b);
	b->scl(true);
	wait(b);
	b->sda(false);
	wait(b);
	b->scl(false);
	wait(b);
}

static void stop(const i2c_gpio_t *b)
{
	b->sda(false);
	wait(b);
	b->scl(true);
	wait(b);
	b->sda(true);
	wait(b);
}

// One byte out, MSB first; true when the slave pulled SDA low in the ninth clock
static bool put(const i2c_gpio_t *b, uint8_t v)
{
	for (int i = 7; i >= 0; i--) {
		b->sda(((v >> i) & 1u) != 0);
		wait(b);
		b->scl(true);
		wait(b);
		b->scl(false);
	}
	b->sda(true);
	wait(b);
	b->scl(true);
	wait(b);
	bool ack = !b->sda_in();
	b->scl(false);
	return ack;
}

// One byte in; ack = pull SDA low in the ninth clock (more bytes wanted)
static uint8_t get(const i2c_gpio_t *b, bool ack)
{
	uint8_t v = 0;
	b->sda(true);
	for (int i = 0; i < 8; i++) {
		wait(b);
		b->scl(true);
		wait(b);
		v = (uint8_t)((v << 1) | (b->sda_in() ? 1u : 0u));
		b->scl(false);
	}
	b->sda(!ack);
	wait(b);
	b->scl(true);
	wait(b);
	b->scl(false);
	b->sda(true);
	return v;
}

bool i2c_gpio_write(const i2c_gpio_t *bus, uint8_t addr7, const uint8_t *data, size_t n)
{
	start(bus);
	bool ok = put(bus, (uint8_t)(addr7 << 1));
	for (size_t i = 0; ok && i < n; i++) {
		ok = put(bus, data[i]);
	}
	stop(bus);
	return ok;
}

bool i2c_gpio_write_read(const i2c_gpio_t *bus, uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
	start(bus);
	bool ok = put(bus, (uint8_t)(addr7 << 1));
	for (size_t i = 0; ok && i < wn; i++) {
		ok = put(bus, w[i]);
	}
	if (ok) {
		start(bus);	// repeated START
		ok = put(bus, (uint8_t)((addr7 << 1) | 1u));
	}
	for (size_t i = 0; ok && i < rn; i++) {
		r[i] = get(bus, i + 1 < rn);
	}
	stop(bus);
	return ok;
}

void i2c_gpio_recover(const i2c_gpio_t *bus)
{
	bus->sda(true);
	for (int i = 0; i < 9; i++) {
		bus->scl(false);
		wait(bus);
		bus->scl(true);
		wait(bus);
	}
	stop(bus);
}

bool i2c_gpio_bus_write(void *ctx, uint8_t addr7, const uint8_t *data, size_t n)
{
	return i2c_gpio_write((const i2c_gpio_t *)ctx, addr7, data, n);
}

bool i2c_gpio_bus_write_read(void *ctx, uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
	return i2c_gpio_write_read((const i2c_gpio_t *)ctx, addr7, w, wn, r, rn);
}
