// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD32F30x system clock: HXTAL through the PLL, with the high-driver mode the core needs above 108 MHz (GD32F30x user
// manual, RCU and PMU chapters). The USB clock is CK_PLL divided down to 48 MHz
#include "platform.h"

uint32_t SystemCoreClock = 8000000u;	// IRC8M after a reset

#define SPINS	200000u

static bool wait_set(volatile uint32_t *reg, uint32_t bit)
{
	for (uint32_t i = 0; i < SPINS; i++) {
		if (*reg & bit) {
			return true;
		}
	}
	return false;
}

static uint32_t usb_div(uint32_t sys_hz)
{
	// USBDPSC: CK_PLL / 1, 1.5, 2, 2.5, 3, 3.5 or 4; the one that gives 48 MHz
	switch (sys_hz) {
	case 48000000u: return 1u << 22;
	case 72000000u: return 0u;
	case 96000000u: return 3u << 22;
	case 120000000u: return RCU_CFG0_USBDPSC_DIV2_5;
	case 144000000u: return 1u << 29;
	case 168000000u: return (1u << 29) | (1u << 22);
	default: return 0u;	// no 48 MHz: USB won't run
	}
}

bool gd32_clock_init(const gd32_clock_t *c)
{
	// IRC8M as the system clock while the PLL is changed (the bootloader may have left the PLL running)
	RCU_CTL |= RCU_CTL_IRC8MEN;
	if (!wait_set(&RCU_CTL, RCU_CTL_IRC8MSTB)) {
		return false;
	}
	RCU_CFG0 &= ~RCU_CFG0_SCS_MASK;
	for (uint32_t i = 0; i < SPINS && (RCU_CFG0 & RCU_CFG0_SCSS_MASK) != 0; i++) {
	}
	RCU_CTL &= ~RCU_CTL_PLLEN;
	SystemCoreClock = 8000000u;

	if (c->hxtal_bypass) {
		RCU_CTL |= RCU_CTL_HXTALBPS;
	}
	RCU_CTL |= RCU_CTL_HXTALEN;
	if (!wait_set(&RCU_CTL, RCU_CTL_HXTALSTB)) {
		RCU_CTL &= ~RCU_CTL_HXTALEN;
		return false;
	}

	// The core voltage and high-driver mode for 120 MHz, before the switch to the PLL
	RCU_APB1EN |= RCU_APB1EN_PMUEN;
	(void)RCU_APB1EN;
	PMU_CTL = (PMU_CTL & ~PMU_CTL_LDOVS_MASK) | PMU_CTL_LDOVS_HIGH;

	uint32_t sys_hz = (c->hxtal_hz / (c->hxtal_div2 ? 2u : 1u)) * c->pll_mul;
	uint32_t cfg = RCU_CFG0;
	cfg &= ~(RCU_CFG0_AHBPSC_MASK | RCU_CFG0_APB1PSC_MASK | RCU_CFG0_APB2PSC_MASK | RCU_CFG0_ADCPSC_MASK |
		RCU_CFG0_PLLSEL | RCU_CFG0_PREDV0 | RCU_CFG0_PLLMF_MASK | RCU_CFG0_USBDPSC_MASK);
	cfg |= RCU_CFG0_APB1PSC_DIV2;	// APB1 at most 60 MHz; AHB and APB2 at the core clock
	cfg |= RCU_CFG0_ADCPSC_DIV6;	// the ADC at most 40 MHz
	cfg |= RCU_CFG0_PLLSEL | (c->hxtal_div2 ? RCU_CFG0_PREDV0 : 0u) | rcu_pllmf(c->pll_mul) | usb_div(sys_hz);
	RCU_CFG0 = cfg;
	RCU_CTL |= RCU_CTL_PLLEN;
	if (!wait_set(&RCU_CTL, RCU_CTL_PLLSTB)) {
		return false;
	}

	if (sys_hz > 108000000u) {
		PMU_CTL |= PMU_CTL_HDEN;
		if (!wait_set(&PMU_CS, PMU_CS_HDRF)) {
			return false;
		}
		PMU_CTL |= PMU_CTL_HDS;
		if (!wait_set(&PMU_CS, PMU_CS_HDSRF)) {
			return false;
		}
	}

	RCU_CFG0 = (RCU_CFG0 & ~RCU_CFG0_SCS_MASK) | RCU_CFG0_SCS_PLL;
	if (!wait_set(&RCU_CFG0, RCU_CFG0_SCSS_PLL)) {
		return false;
	}
	SystemCoreClock = sys_hz;
	return true;
}

uint32_t gd32_apb1_hz(void)
{
	return SystemCoreClock / 2u;
}

uint32_t gd32_apb2_hz(void)
{
	return SystemCoreClock;
}
