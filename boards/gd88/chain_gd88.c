// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-88 SoC A serial output chain (docs/spec/hardware-gd88.md 9.3): 64 outputs behind three pins. A write takes the
// latch (PTB3) low, shifts the 64 bits with data on PTB1 and a PTB0 clock pulse per bit, bit 0 of byte 0 first, then
// takes the latch high again. A shadow keeps the outputs, and every write sends all of them. All zero is the power-on
// state (the first stage sets it), with the transmitter off.
//
// A write runs with interrupts off (64 bits take about 20 us), so any task may set a bit. The zero write is RAM code
// with no lock, for board_rf_safe with interrupts off.
#include "gd88.h"
#include "pins.h"
#include "platform.h"

#define BANK_B		GPIOB_BASE
#define CLK_BIT		(1u << (PIN_CHAIN_CLK - 32u))
#define DATA_BIT	(1u << (PIN_CHAIN_DATA - 32u))
#define LATCH_BIT	(1u << (PIN_CHAIN_LATCH - 32u))
#define EDGE_SPINS	4u		// a few hundred ns per edge; the chain's registers need tens of ns

static volatile uint64_t shadow;

static HRC_RAMFUNC void edge_wait(void)
{
	for (volatile uint32_t i = EDGE_SPINS; i != 0; i--) {
	}
}

// Interrupts off; the pins are outputs. Bank B's data register is written directly: the chain's three pins are the
// only ones changed, every other pin of the bank keeps its level. lo = bits 0-31, hi = bits 32-63 (32-bit halves:
// no library call, so it stays RAM code)
static HRC_RAMFUNC void shift(uint32_t lo, uint32_t hi)
{
	volatile uint32_t *dr = (volatile uint32_t *)(BANK_B + GPIO_DR);
	uint32_t base = *dr & ~(CLK_BIT | DATA_BIT | LATCH_BIT);
	*dr = base;			// latch low, clock low
	for (uint32_t i = 0; i < CHAIN_BITS; i++) {
		uint32_t w = i < 32u ? lo : hi;
		uint32_t d = ((w >> (i & 31u)) & 1u) != 0 ? DATA_BIT : 0;
		*dr = base | d;
		edge_wait();
		*dr = base | d | CLK_BIT;
		edge_wait();
		*dr = base | d;
	}
	edge_wait();
	*dr = base | LATCH_BIT;		// the new bits to the outputs
}

void gd88_chain_init(void)
{
	hrc_pin_out(PIN_CHAIN_LATCH, true);
	hrc_pin_out(PIN_CHAIN_CLK, false);
	hrc_pin_out(PIN_CHAIN_DATA, false);
	uint32_t s = hrc_irq_save();
	shadow = 0;
	shift(0, 0);
	hrc_irq_restore(s);
}

void gd88_chain_set(uint8_t bit, bool on)
{
	if (bit >= CHAIN_BITS) {
		return;		// CHAIN_NONE: the function's bit isn't known
	}
	uint32_t s = hrc_irq_save();
	uint64_t v = on ? (shadow | (1ull << bit)) : (shadow & ~(1ull << bit));
	if (v != shadow) {
		shadow = v;
		shift((uint32_t)v, (uint32_t)(v >> 32));
	}
	hrc_irq_restore(s);
}

// From board_rf_safe, interrupts already off
HRC_RAMFUNC void gd88_chain_zero(void)
{
	shadow = 0;
	shift(0, 0);
}

uint64_t gd88_chain_shadow(void)
{
	return shadow;
}
