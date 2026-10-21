// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Skyworks SKY72310 fractional-N frequency synthesizer (docs/spec/hardware-md380.md 5.2, from the Skyworks
// SKY72310-11 data sheet 203639L): register words, the divider arithmetic, and a write sequence over a board-supplied
// three-wire bus. Generic: the board knows the reference oscillator, the VCO and how its pins are shared.
//
// Serial interface: 16-bit words, MSB first, 4 address bits then 12 data bits, latched when CS goes high. All
// registers are write only. The main divide ratio N = f_vco / f_pd (f_pd = f_osc / reference divider) is programmed
// in signed mode as N_reg = round(N) - 32 (9 bits, 6..505 in fractional mode) plus an 18-bit two's complement
// dividend = round(2^18 x (N - round(N))), -0.5 .. +0.5. The unsigned mode (floor(N), a fraction 0 .. 1) put the MD-380
// one phase detector step low whenever the fraction was over one half (issue 108). A new ratio takes effect when the
// dividend MSB register is written, so the order is divider, dividend LSB, dividend MSB.
#ifndef SKY72310_H
#define SKY72310_H

#include <stdint.h>
#include <stdbool.h>

// Register addresses (data sheet table 2)
enum {
	SKY72310_REG_DIVIDER	= 0x0,
	SKY72310_REG_DIVIDEND_MSB = 0x1,
	SKY72310_REG_DIVIDEND_LSB = 0x2,
	SKY72310_REG_REF_DIV	= 0x5,
	SKY72310_REG_PD_GAIN	= 0x6,
	SKY72310_REG_POWER_MUX	= 0x7,
	SKY72310_REG_MOD_CTRL	= 0x8,
	SKY72310_REG_MOD_DATA	= 0x9,
	SKY72310_REG_DELTA_SIGMA = 0xE,
};

// Register 7 bits (figure 10)
#define SKY72310_PWR_FULL_DOWN		0x001u	// everything but the oscillator and the serial interface
#define SKY72310_PWR_SYNTH_DOWN		0x002u
#define SKY72310_PWR_INTEGER_N		0x004u	// the delta-sigma modulator off: integer-N
#define SKY72310_PWR_10BIT		0x008u	// 10-bit modulator instead of 18-bit
#define SKY72310_MUX_SHIFT		6	// bits 8-6: Mux_out source (table 4)
#define SKY72310_MUX_TRISTATE		0x200u

// Mux_out sources (table 4)
enum {
	SKY72310_MUX_OSC = 0,		// reference oscillator
	SKY72310_MUX_FREF = 2,		// divided reference
	SKY72310_MUX_FPD = 4,		// divided VCO (phase detector input)
	SKY72310_MUX_SERIAL = 5,	// serial data out (loop-back test)
};

// Register 14 (figure 13): bits 11-7 fixed 00001, bit 5 the signed mode
#define SKY72310_DS_BASE		0x080u
#define SKY72310_DS_SIGNED		0x020u

#define SKY72310_N_MIN		38u	// fractional mode: N_reg 6..505 (N 37.5..537.5, whole N 38..537 kept clear)
#define SKY72310_N_MAX		537u
#define SKY72310_FPD_MAX_HZ	50000000u
#define SKY72310_FRAC_BITS	18

// The three divide-ratio words for one VCO frequency
typedef struct {
	uint16_t n_reg;		// 9 bits
	uint32_t dividend;	// 18 bits, two's complement
} sky72310_ratio_t;

// Pure: the divide ratio for f_vco from an oscillator f_osc divided by ref_div (1..32). False when the ratio is out
// of the fractional range or the phase detector frequency is over 50 MHz
bool sky72310_ratio(uint32_t f_osc_hz, uint8_t ref_div, uint32_t f_vco_hz, sky72310_ratio_t *out);

// Pure: the VCO frequency a ratio gives (Hz, rounded down), for tests and diagnostics
uint32_t sky72310_vco_hz(uint32_t f_osc_hz, uint8_t ref_div, const sky72310_ratio_t *r);

// Pure: one 16-bit serial word
static inline uint16_t sky72310_word(uint8_t reg, uint16_t data)
{
	return (uint16_t)(((uint16_t)(reg & 0xFu) << 12) | (data & 0x0FFFu));
}

typedef struct {
	// One 16-bit word, MSB first, framed by CS (low during the word, high after it latches)
	bool (*write)(void *ctx, uint16_t word);
	// Optional: the lock detect pin (true = locked); NULL = not wired
	bool (*locked)(void *ctx);
	void (*delay_us)(uint32_t us);
	void *ctx;
} sky72310_bus_t;

typedef struct {
	const sky72310_bus_t *bus;
	uint32_t f_osc_hz;	// reference oscillator (TCXO) frequency
	uint8_t ref_div;	// reference divider currently programmed, 0 = not yet
	uint8_t pd_gain;	// 0..31 (0.125..1 mA/rad, data sheet register 6)
	uint8_t mux;		// SKY72310_MUX_xxx for Mux_out
	uint32_t vco_hz;	// last frequency programmed, 0 = none
	uint32_t writes;	// words written (diagnostics)
} sky72310_t;

void sky72310_attach(sky72310_t *dev, const sky72310_bus_t *bus, uint32_t f_osc_hz, uint8_t pd_gain);
// Power up in fractional-N, 18-bit unsigned mode with no modulation, Mux_out as dev->mux
bool sky72310_init(sky72310_t *dev);
// Program f_vco with the reference divider ref_div (written only when it changes). False: out of range (nothing
// written) or a bus error
bool sky72310_set_vco(sky72310_t *dev, uint8_t ref_div, uint32_t f_vco_hz);
// Whole synthesizer powered down (true) or up again (false); the last frequency is kept
bool sky72310_power_down(sky72310_t *dev, bool down);
// Wait up to timeout_us for lock detect; true when locked (or when the board has no lock detect wired)
bool sky72310_wait_lock(sky72310_t *dev, uint32_t timeout_us);

#endif
