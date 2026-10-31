// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RF chip register bus and its 3-wire bit-banged implementation (rfbus.h)
#include "rfbus.h"

#define READ_BIT	0x80u
#define BIT_US		1u	// clock low and high time
#define READ_TURN_US	2u	// after releasing the data line, before clocking the reply in

void rfbus_write_table(rfbus_t *bus, const rfbus_regval_t *t, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		rfbus_write(bus, t[i].reg, t[i].val);
	}
}

static uint32_t enter(const rfbus_3w_pins_t *p)
{
	return p->crit_enter != NULL ? p->crit_enter() : 0;
}

static void leave(const rfbus_3w_pins_t *p, uint32_t s)
{
	if (p->crit_exit != NULL) {
		p->crit_exit(s);
	}
}

static void send_bits(const rfbus_3w_pins_t *p, uint32_t v, int n)
{
	for (int i = n - 1; i >= 0; i--) {
		p->clk(false);
		p->data_out(((v >> i) & 1u) != 0);
		p->delay_us(BIT_US);
		p->clk(true);	// sampled by the chip on this edge
		p->delay_us(BIT_US);
	}
}

static void write_3w(rfbus_t *bus, uint8_t reg, uint16_t val)
{
	const rfbus_3w_pins_t *p = ((rfbus_3w_t *)bus)->pins;
	uint32_t s = enter(p);
	p->cs(false);
	// Bit 7 of the address is the read flag: a write never sets it
	send_bits(p, (uint32_t)(reg & 0x7Fu) << 16 | val, 24);
	p->clk(false);
	p->cs(true);
	leave(p, s);
}

static uint16_t read_3w(rfbus_t *bus, uint8_t reg)
{
	const rfbus_3w_pins_t *p = ((rfbus_3w_t *)bus)->pins;
	uint32_t s = enter(p);
	p->cs(false);
	send_bits(p, (reg & 0x7Fu) | READ_BIT, 8);
	p->clk(false);
	p->data_release();
	p->delay_us(READ_TURN_US);
	uint16_t v = 0;
	for (int i = 0; i < 16; i++) {
		p->clk(false);
		p->delay_us(BIT_US);
		p->clk(true);
		v = (uint16_t)((v << 1) | (p->data_in() ? 1u : 0u));	// read while the clock is high
		p->delay_us(BIT_US);
	}
	p->clk(false);
	p->cs(true);
	leave(p, s);
	return v;
}

void rfbus_3w_attach(rfbus_3w_t *b, const rfbus_3w_pins_t *pins)
{
	b->bus.write = write_3w;
	b->bus.read = read_3w;
	b->bus.delay_ms = pins->delay_ms;
	b->bus.writes = 0;
	b->pins = pins;
	pins->cs(true);		// lines idle: deselected, clock low
	pins->clk(false);
}
