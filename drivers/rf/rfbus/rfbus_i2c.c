// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RF chip register bus on I2C (rfbus_i2c.h)
#include "rfbus_i2c.h"

static uint32_t enter(const rfbus_i2c_t *b)
{
	return b->crit_enter != NULL ? b->crit_enter() : 0;
}

static void leave(const rfbus_i2c_t *b, uint32_t s)
{
	if (b->crit_exit != NULL) {
		b->crit_exit(s);
	}
}

static void write_i2c(rfbus_t *bus, uint8_t reg, uint16_t val)
{
	rfbus_i2c_t *b = (rfbus_i2c_t *)bus;
	const uint8_t f[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
	uint32_t s = enter(b);
	bool ok = i2c_gpio_write(b->i2c, b->addr7, f, sizeof f);
	leave(b, s);
	if (!ok) {
		b->nacks++;
	}
}

static uint16_t read_i2c(rfbus_t *bus, uint8_t reg)
{
	rfbus_i2c_t *b = (rfbus_i2c_t *)bus;
	uint8_t r[2] = { 0, 0 };
	uint32_t s = enter(b);
	bool ok = i2c_gpio_write_read(b->i2c, b->addr7, &reg, 1, r, sizeof r);
	leave(b, s);
	if (!ok) {
		b->nacks++;
		return 0;
	}
	return (uint16_t)(r[0] << 8 | r[1]);
}

void rfbus_i2c_attach(rfbus_i2c_t *b, const i2c_gpio_t *i2c, uint8_t addr7, void (*delay_ms)(uint32_t ms))
{
	b->bus.write = write_i2c;
	b->bus.read = read_i2c;
	b->bus.delay_ms = delay_ms;
	b->bus.writes = 0;
	b->i2c = i2c;
	b->addr7 = addr7;
	b->crit_enter = NULL;
	b->crit_exit = NULL;
	b->nacks = 0;
	i2c_gpio_recover(i2c);
}
