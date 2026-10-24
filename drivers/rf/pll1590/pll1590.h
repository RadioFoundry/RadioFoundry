// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// "PLL1590" fractional-N synthesizer (the boards that carry it document it in their hardware specs). The exact part is
// not identified, so this driver knows only the word layout: 24-bit words, MSB first, 20 data bits on top and the
// 4-bit register number in the low nibble. Six registers are written per frequency, in the order 5, 6, 3, 4, 1, 2:
// register 3 holds the reference divider R, register 4 a board constant, register 1 an 18-bit signed fraction and
// register 2 the integer N under a band prefix. Writing register 2 last makes the new ratio take effect (assumed, as
// on the other fractional-N parts; unverified).
//
// The ratio is N + f with N = round(f_vco / f_pd) and f the signed remainder in [-1/2, 1/2), coded as an 18-bit two's
// complement: above one half the integer rounds up and the fraction becomes a negative offset.
#ifndef PLL1590_H
#define PLL1590_H

#include <stdint.h>
#include <stdbool.h>
#include "bus/wire3.h"

#define PLL1590_WORD_BITS	24u
#define PLL1590_FRAC_BITS	18
#define PLL1590_WORDS		6u
#define PLL1590_N_MAX		0x7FFFu	// the band prefixes leave 15 bits for N

enum {
	PLL1590_REG_FRAC	= 1,
	PLL1590_REG_N		= 2,
	PLL1590_REG_REF		= 3,
	PLL1590_REG_CTRL	= 4,
	PLL1590_REG_5		= 5,
	PLL1590_REG_6		= 6,
};

#define PLL1590_REF_BASE	0x6000u	// register 3: 0x6000 | R

typedef struct {
	uint32_t f_osc_hz;	// reference oscillator
	uint8_t ref_div;	// R: f_pd = f_osc / R
	uint32_t n_prefix;	// register 2 bits above N (the band's prescaler setting)
	uint32_t ctrl;		// register 4 data (20 bits)
} pll1590_cfg_t;

typedef struct {
	uint16_t n;
	uint32_t frac;		// 18-bit two's complement
} pll1590_ratio_t;

// Pure: one 24-bit word
static inline uint32_t pll1590_word(uint8_t reg, uint32_t data)
{
	return ((data & 0xFFFFFu) << 4) | (reg & 0xFu);
}

// Pure: the ratio for f_vco. False when R is 0 or N is out of range
bool pll1590_ratio(uint32_t f_osc_hz, uint8_t ref_div, uint32_t f_vco_hz, pll1590_ratio_t *out);
// Pure: the frequency a ratio gives (Hz, rounded down), for tests and diagnostics
uint32_t pll1590_vco_hz(uint32_t f_osc_hz, uint8_t ref_div, const pll1590_ratio_t *r);
// Pure: the six words for f_vco in write order. Returns PLL1590_WORDS, or 0 when out of range (out untouched)
uint8_t pll1590_words(const pll1590_cfg_t *c, uint32_t f_vco_hz, uint32_t out[PLL1590_WORDS]);
// Build and write them on chip select cs. False: out of range (nothing written) or a bus error
bool pll1590_program(const wire3_bus_t *bus, uint8_t cs, const pll1590_cfg_t *c, uint32_t f_vco_hz);

#endif
