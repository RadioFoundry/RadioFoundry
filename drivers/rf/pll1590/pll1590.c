// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// "PLL1590" synthesizer words (pll1590.h; docs/spec/hardware-md9600.md 5.3)
#include <stddef.h>
#include "pll1590.h"

#define FRAC_ONE	(1u << PLL1590_FRAC_BITS)
#define FRAC_MASK	(FRAC_ONE - 1u)

bool pll1590_ratio(uint32_t f_osc_hz, uint8_t ref_div, uint32_t f_vco_hz, pll1590_ratio_t *out)
{
	if (f_osc_hz == 0 || ref_div == 0) {
		return false;
	}
	// The ratio in 2^-18 steps, rounded to the nearest: total = f_vco x R x 2^18 / f_osc
	uint64_t total = ((((uint64_t)f_vco_hz * ref_div) << PLL1590_FRAC_BITS) + f_osc_hz / 2u) / f_osc_hz;
	// N is the nearest integer: a remainder of one half or more rounds up and leaves a negative fraction
	uint64_t n = (total + FRAC_ONE / 2u) >> PLL1590_FRAC_BITS;
	if (n == 0 || n > PLL1590_N_MAX) {
		return false;
	}
	int64_t rem = (int64_t)total - (int64_t)(n << PLL1590_FRAC_BITS);	// -2^17 .. 2^17 - 1
	out->n = (uint16_t)n;
	out->frac = (uint32_t)rem & FRAC_MASK;
	return true;
}

uint32_t pll1590_vco_hz(uint32_t f_osc_hz, uint8_t ref_div, const pll1590_ratio_t *r)
{
	if (ref_div == 0) {
		return 0;
	}
	// Sign-extend the 18-bit fraction
	int64_t frac = (int64_t)(r->frac & FRAC_MASK);
	if (frac >= (int64_t)(FRAC_ONE / 2u)) {
		frac -= FRAC_ONE;
	}
	int64_t total = ((int64_t)r->n << PLL1590_FRAC_BITS) + frac;
	return (uint32_t)(((uint64_t)total * f_osc_hz / ref_div) >> PLL1590_FRAC_BITS);
}

uint8_t pll1590_words(const pll1590_cfg_t *c, uint32_t f_vco_hz, uint32_t out[PLL1590_WORDS])
{
	pll1590_ratio_t r;
	if (!pll1590_ratio(c->f_osc_hz, c->ref_div, f_vco_hz, &r)) {
		return 0;
	}
	out[0] = pll1590_word(PLL1590_REG_5, 0);
	out[1] = pll1590_word(PLL1590_REG_6, 0);
	out[2] = pll1590_word(PLL1590_REG_REF, PLL1590_REF_BASE | c->ref_div);
	out[3] = pll1590_word(PLL1590_REG_CTRL, c->ctrl);
	out[4] = pll1590_word(PLL1590_REG_FRAC, r.frac);
	out[5] = pll1590_word(PLL1590_REG_N, c->n_prefix | r.n);
	return PLL1590_WORDS;
}

bool pll1590_program(const wire3_bus_t *bus, uint8_t cs, const pll1590_cfg_t *c, uint32_t f_vco_hz)
{
	uint32_t w[PLL1590_WORDS];
	uint8_t n = pll1590_words(c, f_vco_hz, w);
	if (n == 0) {
		return false;
	}
	bool ok = true;
	for (uint8_t i = 0; i < n; i++) {
		ok &= bus->write(bus->ctx, cs, w[i], PLL1590_WORD_BITS);
	}
	return ok;
}
