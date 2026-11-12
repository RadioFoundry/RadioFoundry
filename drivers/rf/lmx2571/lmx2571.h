// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// TI LMX2571 synthesizer in PLL mode with an external VCO (TI data sheet SNAS654A; docs/spec/hardware-md9600.md 5.3).
//
// Serial interface (data sheet 7.5): 24-bit words, MSB first, clocked in on the rising edge while LE is low and
// latched when LE rises: a R/W bit (0 = write), a 7-bit address and 16 data bits. Only the F1 register set is used.
// The N divider is PLL_N (R4 bits 11-0, at most 1023) plus PLL_NUM / PLL_DEN, both 24 bits split over R1 (bits 23-16,
// DEN high byte in 15-8, NUM high byte in 7-0) and R2 / R3 (bits 15-0). The phase detector runs at
// f_osc / PLL_R_PRE (R5 bits 7-0) with the multiplier bypassed and PLL_R (R5 bits 15-8) at 1.
//
// The board's register set has FSK_EN set in R8, and the data sheet asks for DEN = 0 in that mode; the fraction is then
// taken over 2^24 (unverified on a radio). The words are built for that denominator.
#ifndef LMX2571_H
#define LMX2571_H

#include <stdint.h>
#include <stdbool.h>
#include "bus/wire3.h"

#define LMX2571_WORD_BITS	24u
#define LMX2571_FRAC_BITS	24
#define LMX2571_N_MAX		1023u

enum {
	LMX2571_R0 = 0x00,	// RESET (bit 13), POWERDOWN (12), ..., FCAL_EN (0)
	LMX2571_R1 = 0x01,	// PLL_DEN_F1[23:16] << 8 | PLL_NUM_F1[23:16]
	LMX2571_R2 = 0x02,	// PLL_NUM_F1[15:0]
	LMX2571_R3 = 0x03,	// PLL_DEN_F1[15:0]
	LMX2571_R4 = 0x04,	// PLL_N_PRE_F1 (15), FRAC_ORDER_F1 (14-12), PLL_N_F1 (11-0)
	LMX2571_R5 = 0x05,	// PLL_R_F1 (15-8), PLL_R_PRE_F1 (7-0)
	LMX2571_R6 = 0x06,	// LF_R3, CHDIV2, CHDIV1, PFD_DELAY, MULT (F1)
	LMX2571_R7 = 0x07,	// output buffer settings, LF_R4 (F1)
	LMX2571_R8 = 0x08,	// FSK_EN, EXTVCO_CHDIV, EXTVCO_SEL, OUTBUF_TX_PWR (F1)
	LMX2571_R41 = 0x29,	// external VCO charge pump
	LMX2571_R42 = 0x2A,
};

#define LMX2571_R0_RESET	0x2000u
#define LMX2571_R0_FCAL_EN	0x0001u
#define LMX2571_R4_FRAC_ORDER(o)	((uint16_t)(((o) & 7u) << 12))
#define LMX2571_R5_R(r, pre)	((uint16_t)((((r) & 0xFFu) << 8) | ((pre) & 0xFFu)))

// The board's fixed register values and the reference
typedef struct {
	uint32_t f_osc_hz;
	uint8_t r_pre;		// PLL_R_PRE: f_pd = f_osc / r_pre (PLL_R 1, MULT bypassed in r6)
	uint8_t frac_order;	// FRAC_ORDER_F1
	uint16_t r0;		// R0 in normal operation, FCAL_EN clear
	uint16_t r6, r7, r8, r41, r42;
} lmx2571_cfg_t;

typedef struct {
	uint16_t n;
	uint32_t num;		// over 2^24
} lmx2571_ratio_t;

// Words per frequency: reset, R0, R4, R1, R2, R3, R5, R6, R7, R8, R41, R42, R0 with FCAL_EN, R0
#define LMX2571_WORDS		14u

static inline uint32_t lmx2571_word(uint8_t addr, uint16_t data)
{
	return ((uint32_t)(addr & 0x7Fu) << 16) | data;	// bit 23 = 0: write
}

// Pure: the ratio for f_vco from f_osc / r_pre. False when out of range
bool lmx2571_ratio(uint32_t f_osc_hz, uint8_t r_pre, uint32_t f_vco_hz, lmx2571_ratio_t *out);
uint32_t lmx2571_vco_hz(uint32_t f_osc_hz, uint8_t r_pre, const lmx2571_ratio_t *r);
// Pure: the whole sequence for f_vco in write order. Returns LMX2571_WORDS, or 0 when out of range
uint8_t lmx2571_words(const lmx2571_cfg_t *c, uint32_t f_vco_hz, uint32_t out[LMX2571_WORDS]);
// Build and write them on chip select cs. The calibration the last words start takes time the caller waits out
bool lmx2571_program(const wire3_bus_t *bus, uint8_t cs, const lmx2571_cfg_t *c, uint32_t f_vco_hz);

#endif
