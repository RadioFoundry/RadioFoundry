// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pins from a board's table (k22f_pin.h): the PORT control register for function, pull, drive and interrupt edge,
// and for GPIO outputs the level before the direction
#include "platform.h"

void k22f_pin_config(uint8_t pin, uint32_t flags)
{
	if (!k22f_pin_valid(pin)) {
		return;
	}
	uint8_t port = k22f_pin_port(pin);
	if (k22f_pin_is_output(flags)) {
		k22f_pin_write(pin, (flags & K22F_HIGH) != 0);
	}
	PORT_PCR(port, k22f_pin_num(pin)) = k22f_pcr_value(flags) | PORT_PCR_ISF;	// a stale interrupt flag cleared
	if (k22f_pin_mux_of(flags) == 1u) {
		k22f_pin_dir(pin, (flags & K22F_OUT) != 0);
	}
}

void k22f_pins_apply(const k22f_pin_cfg_t *table, size_t n)
{
	SIM_SCGC5 |= SIM_SCGC5_PORTS;
	for (size_t i = 0; i < n; i++) {
		k22f_pin_config(table[i].pin, table[i].flags);
	}
}

// PDDR is a whole-port register: changed with interrupts off (the bit-banged buses turn SDA around from tasks)
void k22f_pin_dir(uint8_t pin, bool output)
{
	uint8_t port = k22f_pin_port(pin);
	uint32_t s = k22f_irq_save();
	if (output) {
		GPIO_PDDR(port) |= k22f_pin_bit(pin);
	} else {
		GPIO_PDDR(port) &= ~k22f_pin_bit(pin);
	}
	k22f_irq_restore(s);
}

uint32_t k22f_port_irq_pending(uint8_t port)
{
	return PORT_ISFR(port);
}

void k22f_port_irq_ack(uint8_t port, uint32_t bits)
{
	PORT_ISFR(port) = bits;
}
