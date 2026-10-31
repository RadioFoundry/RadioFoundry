// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// FTM edge-aligned PWM on one channel (reference manual chapter 39), for a dimmable backlight. The counter runs from
// the bus clock up to MOD; the channel is high from 0 until CnV. Written in the FTM's basic mode (FTMEN = 0), where a
// new CnV takes effect at the next period, so a change never makes a short or long pulse.
#include "platform.h"
#include "periph_math.h"

typedef struct {
	uint32_t base;
	uint32_t gate;		// SIM_SCGC6 bit
	uint16_t mod;
} ftm_t;

static ftm_t ftm[2] = {
	{ .base = 0x40038000u, .gate = 1u << 24 },	// FTM0
	{ .base = 0x40026000u, .gate = 1u << 6 },	// FTM3
};

#define FTM_SC(b)		K22F_REG32((b) + 0x00u)
#define FTM_CNT(b)		K22F_REG32((b) + 0x04u)
#define FTM_MOD(b)		K22F_REG32((b) + 0x08u)
#define FTM_CSC(b, n)		K22F_REG32((b) + 0x0Cu + 8u * (uint32_t)(n))
#define FTM_CV(b, n)		K22F_REG32((b) + 0x10u + 8u * (uint32_t)(n))
#define FTM_CNTIN(b)		K22F_REG32((b) + 0x4Cu)

static ftm_t *get(uint8_t inst)
{
	return inst == 0u ? &ftm[0] : (inst == 3u ? &ftm[1] : NULL);
}

bool k22f_pwm_start(uint8_t inst, uint8_t ch, uint32_t pwm_hz)
{
	ftm_t *f = get(inst);
	uint8_t ps;
	uint16_t mod;
	if (f == NULL || ch > 7u || !k22f_pwm_period(k22f_bus_clock(), pwm_hz, &ps, &mod)) {
		return false;
	}
	SIM_SCGC6 |= f->gate;
	FTM_SC(f->base) = 0;			// stopped while it is set up
	FTM_CNTIN(f->base) = 0;
	FTM_CNT(f->base) = 0;
	FTM_MOD(f->base) = mod;
	FTM_CSC(f->base, ch) = FTM_CSC_MSB | FTM_CSC_ELSB;
	FTM_CV(f->base, ch) = 0;		// off until the first set
	FTM_SC(f->base) = FTM_SC_CLKS_SYSTEM | FTM_SC_PS(ps);
	f->mod = mod;
	return true;
}

void k22f_pwm_set(uint8_t inst, uint8_t ch, uint8_t percent)
{
	ftm_t *f = get(inst);
	if (f != NULL && ch <= 7u) {
		FTM_CV(f->base, ch) = k22f_pwm_cv(f->mod, percent);
	}
}
