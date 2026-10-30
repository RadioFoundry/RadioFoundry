// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An RF chip's register bus: 8-bit register address, 16-bit values. The chip drivers (BK4819, FD6818, AT1141 and the
// like) take an rfbus_t and never see the wires; a board gives each chip one, usually an rfbus_3w_t.
//
// rfbus_3w_t is the 3-wire serial bus these chips share, bit-banged: an enable per chip (low = selected), clock and a
// bidirectional data line; 8-bit address (bit 7 = read), then 16 data bits, MSB first, data changed while the clock is
// low and sampled on the rising edge. Each frame runs inside the board's critical section so an interrupt can use the
// bus too; crit_enter / crit_exit may be NULL where nothing else touches it.
#ifndef RFBUS_H
#define RFBUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct rfbus rfbus_t;

struct rfbus {
	void (*write)(rfbus_t *bus, uint8_t reg, uint16_t val);
	uint16_t (*read)(rfbus_t *bus, uint8_t reg);
	void (*delay_ms)(uint32_t ms);	// init and resets only; NULL = no wait
	uint32_t writes;		// frames written (diagnostics)
};

typedef struct {
	uint8_t reg;
	uint16_t val;
} rfbus_regval_t;

static inline void rfbus_write(rfbus_t *bus, uint8_t reg, uint16_t val)
{
	bus->write(bus, reg, val);
	bus->writes++;
}

static inline uint16_t rfbus_read(rfbus_t *bus, uint8_t reg)
{
	return bus->read(bus, reg);
}

static inline void rfbus_delay_ms(rfbus_t *bus, uint32_t ms)
{
	if (bus->delay_ms != NULL) {
		bus->delay_ms(ms);
	}
}

void rfbus_write_table(rfbus_t *bus, const rfbus_regval_t *t, size_t n);

// ---- The 3-wire bit-banged bus ----

typedef struct {
	void (*cs)(bool level);		// this chip's enable (low = selected)
	void (*clk)(bool level);
	void (*data_out)(bool level);	// drive the data pin (makes it an output)
	void (*data_release)(void);	// data pin to input
	bool (*data_in)(void);
	void (*delay_us)(uint32_t us);
	uint32_t (*crit_enter)(void);	// optional
	void (*crit_exit)(uint32_t saved);
	void (*delay_ms)(uint32_t ms);	// init only
} rfbus_3w_pins_t;

typedef struct {
	rfbus_t bus;			// first: a chip driver gets &x.bus
	const rfbus_3w_pins_t *pins;
} rfbus_3w_t;

void rfbus_3w_attach(rfbus_3w_t *b, const rfbus_3w_pins_t *pins);

#endif
