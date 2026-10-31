// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT32F43x system clock: HEXT through the PLL (AT32F435/437 reference manual, CRM and PWC chapters). The order is the
// stock firmware's (docs/spec/hardware-d578uv.md 10): the regulator's output and the flash divider first, the bus
// dividers, then the PLL, and the switch to it with the automatic step on. The USB clock is the 48 MHz HICK, not the
// PLL (usb.c)
#include "platform.h"

uint32_t SystemCoreClock = 8000000u;	// HICK after a reset (the 48 MHz HICK divided by 6)

#define SPINS	200000u
#define MHZ	1000000u

static bool wait_set(volatile uint32_t *reg, uint32_t bit)
{
	for (uint32_t i = 0; i < SPINS; i++) {
		if (*reg & bit) {
			return true;
		}
	}
	return false;
}

// MS, NS and FR for sys_hz from the crystal, as the stock firmware sets them: the reference (crystal / MS) at the
// highest whole-MHz rate up to 16 MHz, FR /2, NS what is left (12 MHz x 20 / 2 = 120 MHz from a 12 MHz crystal). Zero
// when sys_hz is not a whole multiple of the reference
static uint32_t pll_cfg(uint32_t hext_hz, uint32_t sys_hz)
{
	for (uint32_t ms = 1; ms <= 15u; ms++) {
		uint32_t ref = hext_hz / ms;
		if (ref * ms != hext_hz || ref > 16u * MHZ) {
			continue;
		}
		uint32_t vco = sys_hz * 2u;
		uint32_t ns = vco / ref;
		if (ref < 2u * MHZ || ns * ref != vco || ns > 500u) {
			return 0;
		}
		return CRM_PLLCFG_MS(ms) | CRM_PLLCFG_NS(ns) | CRM_PLLCFG_FR(1u);
	}
	return 0;
}

bool at32_clock_init(const at32_clock_t *c)
{
	// HICK as the system clock while the PLL is changed (the bootloader may have left the PLL running)
	CRM_CTRL |= CRM_CTRL_HICKEN;
	if (!wait_set(&CRM_CTRL, CRM_CTRL_HICKSTBL)) {
		return false;
	}
	CRM_CFG &= ~CRM_CFG_SCLKSEL_MASK;
	for (uint32_t i = 0; i < SPINS && (CRM_CFG & CRM_CFG_SCLKSTS_MASK) != 0; i++) {
	}
	CRM_CTRL &= ~CRM_CTRL_PLLEN;
	SystemCoreClock = 8000000u;

	const uint32_t pll = pll_cfg(c->hext_hz, c->sys_hz);
	if (pll == 0) {
		return false;
	}
	if (c->hext_bypass) {
		CRM_CTRL |= CRM_CTRL_HEXTBYPS;
	}
	CRM_CTRL |= CRM_CTRL_HEXTEN;
	if (!wait_set(&CRM_CTRL, CRM_CTRL_HEXTSTBL)) {
		CRM_CTRL &= ~CRM_CTRL_HEXTEN;
		return false;
	}

	// The regulator's output and the flash interface's divider for the fast clock, as the stock firmware sets them
	CRM_APB1EN |= CRM_APB1EN_PWCEN;
	(void)CRM_APB1EN;
	PWC_LDOOV = (PWC_LDOOV & ~PWC_LDOOV_MASK) | 1u;
	FLASH_DIVR = (FLASH_DIVR & ~FLASH_DIVR_MASK) | 1u;

	uint32_t cfg = CRM_CFG;
	cfg &= ~(CRM_CFG_AHBDIV_MASK | CRM_CFG_APB1DIV_MASK | CRM_CFG_APB2DIV_MASK);
	cfg |= CRM_CFG_APB1DIV_2 | CRM_CFG_APB2DIV_2;	// both APBs at half the core clock, as the stock firmware runs
	CRM_CFG = cfg;
	CRM_PLLCFG = pll | CRM_PLLCFG_RCS_HEXT;
	CRM_CTRL |= CRM_CTRL_PLLEN;
	if (!wait_set(&CRM_CTRL, CRM_CTRL_PLLSTBL)) {
		return false;
	}

	// The automatic step raises the clock in stages, so the regulator isn't hit by the whole change at once
	CRM_MISC2 |= CRM_MISC2_AUTO_STEP_ON;
	CRM_CFG = (CRM_CFG & ~CRM_CFG_SCLKSEL_MASK) | CRM_CFG_SCLKSEL_PLL;
	bool ok = false;
	for (uint32_t i = 0; i < SPINS && !ok; i++) {
		ok = (CRM_CFG & CRM_CFG_SCLKSTS_MASK) == CRM_CFG_SCLKSTS_PLL;
	}
	CRM_MISC2 &= ~CRM_MISC2_AUTO_STEP_MASK;
	if (!ok) {
		return false;
	}
	SystemCoreClock = c->sys_hz;
	return true;
}

uint32_t at32_apb1_hz(void)
{
	return SystemCoreClock / 2u;
}

uint32_t at32_apb2_hz(void)
{
	return SystemCoreClock / 2u;
}
