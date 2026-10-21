// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// LMX2571 synthesizer words (lmx2571.h)
#include <stddef.h>
#include "lmx2571.h"

#define FRAC_ONE	(1u << LMX2571_FRAC_BITS)

bool lmx2571_ratio(uint32_t f_osc_hz, uint8_t r_pre, uint32_t f_vco_hz, lmx2571_ratio_t *out)
{
	if (f_osc_hz == 0 || r_pre == 0) {
		return false;
	}
	uint64_t total = ((((uint64_t)f_vco_hz * r_pre) << LMX2571_FRAC_BITS) + f_osc_hz / 2u) / f_osc_hz;
	uint64_t n = total >> LMX2571_FRAC_BITS;
	if (n == 0 || n > LMX2571_N_MAX) {
		return false;
	}
	out->n = (uint16_t)n;
	out->num = (uint32_t)(total & (FRAC_ONE - 1u));
	return true;
}

uint32_t lmx2571_vco_hz(uint32_t f_osc_hz, uint8_t r_pre, const lmx2571_ratio_t *r)
{
	if (r_pre == 0) {
		return 0;
	}
	uint64_t total = ((uint64_t)r->n << LMX2571_FRAC_BITS) + r->num;
	return (uint32_t)((total * f_osc_hz / r_pre) >> LMX2571_FRAC_BITS);
}

uint8_t lmx2571_words(const lmx2571_cfg_t *c, uint32_t f_vco_hz, uint32_t out[LMX2571_WORDS])
{
	lmx2571_ratio_t r;
	if (!lmx2571_ratio(c->f_osc_hz, c->r_pre, f_vco_hz, &r)) {
		return 0;
	}
	uint8_t i = 0;
	out[i++] = lmx2571_word(LMX2571_R0, LMX2571_R0_RESET);	// self-clearing: every register to its default
	out[i++] = lmx2571_word(LMX2571_R0, c->r0);
	out[i++] = lmx2571_word(LMX2571_R4, (uint16_t)(LMX2571_R4_FRAC_ORDER(c->frac_order) | r.n));
	out[i++] = lmx2571_word(LMX2571_R1, (uint16_t)(r.num >> 16));	// DEN[23:16] = 0
	out[i++] = lmx2571_word(LMX2571_R2, (uint16_t)(r.num & 0xFFFFu));
	out[i++] = lmx2571_word(LMX2571_R3, 0);				// DEN[15:0] = 0
	out[i++] = lmx2571_word(LMX2571_R5, LMX2571_R5_R(1u, c->r_pre));
	out[i++] = lmx2571_word(LMX2571_R6, c->r6);
	out[i++] = lmx2571_word(LMX2571_R7, c->r7);
	out[i++] = lmx2571_word(LMX2571_R8, c->r8);
	out[i++] = lmx2571_word(LMX2571_R41, c->r41);
	out[i++] = lmx2571_word(LMX2571_R42, c->r42);
	// The N divider changed: R0 with FCAL_EN calibrates the loop (data sheet 7.5.2), then R0 as in normal operation
	out[i++] = lmx2571_word(LMX2571_R0, (uint16_t)(c->r0 | LMX2571_R0_FCAL_EN));
	out[i++] = lmx2571_word(LMX2571_R0, c->r0);
	return i;
}

bool lmx2571_program(const wire3_bus_t *bus, uint8_t cs, const lmx2571_cfg_t *c, uint32_t f_vco_hz)
{
	uint32_t w[LMX2571_WORDS];
	uint8_t n = lmx2571_words(c, f_vco_hz, w);
	if (n == 0) {
		return false;
	}
	bool ok = true;
	for (uint8_t i = 0; i < n; i++) {
		ok &= bus->write(bus->ctx, cs, w[i], LMX2571_WORD_BITS);
	}
	return ok;
}
