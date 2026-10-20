// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// K22F clock set-up (reference manual 24.5.3, mode transitions). The loader in front of the application may leave any
// MCG mode running, so the switch goes through the fast internal reference first (FBI), then to the external clock
// with the FLL bypassed (FBE), the PLL locked in PLL-bypassed mode (PBE), and the PLL last (PEE). Every wait is
// bounded: when the external clock or the PLL never comes, the radio stays on the internal FLL (FEI) and says so.
#include "platform.h"

#define WAIT_LOOPS	200000u		// about 10 ms at the slowest clock passed through (4 MHz IRC)
#define FRDIV_HIGH_RANGE	3u	// FLL reference divider while the FLL is bypassed: any legal value, the FLL isn't used

uint32_t SystemCoreClock = K22F_FEI_HZ;
static uint32_t bus_hz = K22F_FEI_HZ;
static bool on_pll;

static bool wait_s(uint8_t mask, uint8_t value)
{
	for (uint32_t i = 0; i < WAIT_LOOPS; i++) {
		if ((MCG_S & mask) == value) {
			return true;
		}
	}
	return false;
}

static bool wait_pmstat(uint8_t value)
{
	for (uint32_t i = 0; i < WAIT_LOOPS; i++) {
		if (SMC_PMSTAT == value) {
			return true;
		}
	}
	return false;
}

// Back to the reset state: FLL from the slow internal reference, every divider 1 (20.97 MHz is legal for all)
static void fall_back_fei(void)
{
	SIM_CLKDIV1 = SIM_CLKDIV1_OUTDIV(1, 1, 1, 1);
	MCG_C6 = 0;				// PLL not selected
	MCG_C1 = MCG_C1_CLKS(0) | MCG_C1_IREFS;
	(void)wait_s(MCG_S_IREFST | MCG_S_CLKST_MASK, MCG_S_IREFST | MCG_S_CLKST(0));
	SystemCoreClock = K22F_FEI_HZ;
	bus_hz = K22F_FEI_HZ;
	on_pll = false;
}

k22f_status_t k22f_clock_init(const k22f_clock_t *c)
{
	if (k22f_clock_check(c) != 0) {
		return K22F_ERR_ARG;
	}

	// 1. Off whatever runs now: the fast internal reference (4 MHz), PLL deselected
	MCG_C2 = (uint8_t)(MCG_C2 | 0x01u);	// IRCS: fast internal reference
	MCG_C1 = MCG_C1_CLKS(1) | MCG_C1_IREFS | MCG_C1_IRCLKEN;
	if (!wait_s(MCG_S_CLKST_MASK, MCG_S_CLKST(1))) {
		fall_back_fei();
		return K22F_ERR_TIMEOUT;
	}
	MCG_C6 = 0;
	SIM_CLKDIV1 = SIM_CLKDIV1_OUTDIV(c->div_core, c->div_bus, c->div_flexbus, c->div_flash);

	// 2. Power mode for the target clock (HSRUN for a core above 80 MHz)
	if (c->hsrun) {
		SMC_PMPROT = SMC_PMPROT_AHSRUN;
		SMC_PMCTRL = SMC_PMCTRL_RUNM_HSRUN;
		if (!wait_pmstat(SMC_PMSTAT_HSRUN)) {
			fall_back_fei();
			return K22F_ERR_TIMEOUT;
		}
	}

	// 3. FBE: the external reference, very high range; the oscillator only for a crystal
	OSC_CR = OSC_CR_ERCLKEN;
	MCG_C2 = (uint8_t)(MCG_C2_RANGE(2) | (c->crystal ? MCG_C2_EREFS : 0u));
	MCG_C1 = MCG_C1_CLKS(2) | MCG_C1_FRDIV(FRDIV_HIGH_RANGE);
	if ((c->crystal && !wait_s(MCG_S_OSCINIT0, MCG_S_OSCINIT0)) ||
	    !wait_s(MCG_S_IREFST | MCG_S_CLKST_MASK, MCG_S_CLKST(2))) {
		fall_back_fei();
		return K22F_ERR_TIMEOUT;
	}

	// 4. PBE: the PLL locks while the external clock still runs the chip
	MCG_C5 = MCG_C5_PRDIV0(c->prdiv);
	MCG_C6 = (uint8_t)(MCG_C6_PLLS | MCG_C6_VDIV0(c->vdiv));
	if (!wait_s(MCG_S_PLLST, MCG_S_PLLST) || !wait_s(MCG_S_LOCK0, MCG_S_LOCK0)) {
		fall_back_fei();
		return K22F_ERR_TIMEOUT;
	}

	// 5. PEE
	MCG_C1 = (uint8_t)((MCG_C1 & ~MCG_C1_CLKS_MASK) | MCG_C1_CLKS(0));
	if (!wait_s(MCG_S_CLKST_MASK, MCG_S_CLKST(3))) {
		fall_back_fei();
		return K22F_ERR_TIMEOUT;
	}
	SIM_SOPT2 = (SIM_SOPT2 & ~SIM_SOPT2_PLLFLLSEL_MASK) | SIM_SOPT2_PLLFLLSEL(1);	// peripheral clocks from the PLL
	SystemCoreClock = k22f_core_hz(c);
	bus_hz = k22f_bus_hz(c);
	on_pll = true;
	return K22F_OK;
}

uint32_t k22f_bus_clock(void)
{
	return bus_hz;
}

bool k22f_clock_on_pll(void)
{
	return on_pll;
}
