// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT1141 PLL / VCO synthesizer (chip ID 0x1100), used as a superhet's local oscillator: chip driver, independent of
// the board (docs/spec/hardware-db25d.md 4.8). No datasheet exists; register values are the ones the working firmware
// uses.
//
// The chip sits on the same three-wire bus as a BK4819 (8-bit address, bit 7 = read, 16-bit data): the board passes its
// register bus (rf/rfbus/rfbus.h). Register 0x7F selects a second bank (1) for the duration of a table. The synthesizer
// runs a VCO of about 2.2-3.3 GHz divided by d (2..64) after a x2 / x3 multiplier choice (m); the PLL word in 0x62 /
// 0x63 is a 6.26 fixed-point ratio to the 26 MHz reference.
#ifndef AT1141_H
#define AT1141_H

#include <stdint.h>
#include <stdbool.h>
#include "rf/rfbus/rfbus.h"

#define AT1141_ID		0x1100u

typedef struct {
	rfbus_t *bus;
	uint16_t c15, c16, c19;		// registers 0x15, 0x16, 0x19 as read after the init table: the band select edits them
	uint32_t lo_hz;			// last tuned
} at1141_t;

// One tuning, worked out without the chip (host-testable)
typedef struct {
	uint8_t div;			// d: register 0x16 low byte
	bool mul;			// m: register 0x15 bit 0 (x3 when set, else x2)
	uint16_t r3f;			// register 0x3F
	uint32_t pll;			// registers 0x62 (low half) and 0x63 (high half)
} at1141_plan_t;

void at1141_attach(at1141_t *c, rfbus_t *bus);
uint16_t at1141_id(at1141_t *c);
// Power-on set-up: the tables, the register cache, the band for 400 MHz, output level 2. The chip is left enabled
void at1141_init(at1141_t *c);
void at1141_plan(uint32_t lo_hz, uint16_t c19, at1141_plan_t *p);
// Tune to lo_hz and enable the output
void at1141_tune(at1141_t *c, uint32_t lo_hz);
void at1141_enable(at1141_t *c);
void at1141_disable(at1141_t *c);
// Off for the transmitter (the output and the VCO), and back on after it with a restart
void at1141_off(at1141_t *c);
void at1141_restart(at1141_t *c);
// Output level 0..15 (0 = the lowest the firmware sets, 8 and above the highest)
void at1141_level(at1141_t *c, uint8_t level);
// The chip's GPIO 0..7 high or low
void at1141_gpio(at1141_t *c, uint8_t pin, bool high);

#endif
