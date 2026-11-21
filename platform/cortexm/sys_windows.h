// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Memory and register windows a Cortex-M platform without its own peripheral map offers the debug link
// (hal_system_t.mem_window for READ_MEM, .reg for CHIP_REG's SoC form, docs/spec/debuglink.md 3.24). Only what every
// Cortex-M4 has mapped: program flash, SRAM below the stack top, and the System Control Space's NVIC and SCB words (its
// reserved gaps left out). The peripheral area has unmapped holes whose read is a bus fault, and SysTick's control word clears its count flag on a
// read, so neither is in a window.
#ifndef CORTEXM_SYS_WINDOWS_H
#define CORTEXM_SYS_WINDOWS_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_system.h"

#define CORTEXM_FLASH_BASE	0x08000000u
#define CORTEXM_SRAM_BASE	0x20000000u
#define CORTEXM_NVIC_LO		0xE000E100u	// NVIC: enables, pendings, actives, priorities (240 interrupts)
#define CORTEXM_NVIC_HI		0xE000E4EFu
#define CORTEXM_SCB_LO		0xE000ED00u	// SCB: CPUID to the fault and feature words
#define CORTEXM_SCB_HI		0xE000ED8Fu

extern uint32_t _estack[];

static inline bool cortexm_mem_window(unsigned i, hal_mem_window_t *w, uint32_t flash_kb)
{
	switch (i) {
	case 0:
		*w = (hal_mem_window_t){ CORTEXM_FLASH_BASE, CORTEXM_FLASH_BASE + flash_kb * 1024u - 1u, false };
		return true;
	case 1:
		*w = (hal_mem_window_t){ CORTEXM_SRAM_BASE, (uint32_t)(uintptr_t)_estack - 1u, false };
		return true;
	case 2:
		*w = (hal_mem_window_t){ CORTEXM_NVIC_LO, CORTEXM_NVIC_HI, true };
		return true;
	case 3:
		*w = (hal_mem_window_t){ CORTEXM_SCB_LO, CORTEXM_SCB_HI, true };
		return true;
	default:
		return false;
	}
}

// One aligned word of the NVIC or the SCB: written when write, then read back
static inline bool cortexm_reg(uint32_t addr, uint32_t *val, bool write)
{
	if ((addr & 3u) != 0 || !((addr >= CORTEXM_NVIC_LO && addr <= CORTEXM_NVIC_HI - 3u) ||
				  (addr >= CORTEXM_SCB_LO && addr <= CORTEXM_SCB_HI - 3u))) {
		return false;
	}
	volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)addr;

	if (write) {
		*r = *val;
	}
	*val = *r;
	return true;
}

#endif
