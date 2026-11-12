// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4's READ_MEM windows (docs/spec/debuglink.md 3.10), no hardware access (host tested): internal flash up to
// its size, system memory, OTP and option bytes, and the core's system control block, whose registers have no read side
// effects (the CPU ID, the fault status). Never RAM, CCM or other peripherals: the debug link has no use for them here,
// and a peripheral read can clear a flag or pop a FIFO. CHIP_REG's SoC windows (3.24) are wider: a register tool may
// mean a read's side effects.
#ifndef STM32_MEM_WINDOW_H
#define STM32_MEM_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_system.h"

// flash_kb: the flash size register; 0 or out of range counts as 1 MB, the largest part
static inline bool stm32_mem_window_for(unsigned i, hal_mem_window_t *w, uint16_t flash_kb)
{
	static const hal_mem_window_t fixed[] = {
		{ 0x1FFF0000u, 0x1FFF77FFu, false },	// system memory (ROM bootloader)
		{ 0x1FFF7800u, 0x1FFF7A0Fu, false },	// OTP and its lock bytes
		{ 0x1FFFC000u, 0x1FFFC00Fu, false },	// option bytes
		{ 0xE000ED00u, 0xE000ED3Fu, true },	// SCB: CPUID, ICSR, VTOR, AIRCR, SCR, CCR, SHPR1-3, SHCSR, CFSR, HFSR,
							// DFSR, MMFAR, BFAR, AFSR (Cortex-M4: all plain or write-1-to-clear)
	};
	if (i == 0) {
		uint32_t bytes = (flash_kb >= 16u && flash_kb <= 1024u) ? (uint32_t)flash_kb * 1024u : 0x100000u;
		*w = (hal_mem_window_t){ 0x08000000u, 0x08000000u + bytes - 1u, false };
		return true;
	}
	if (i - 1u < sizeof(fixed) / sizeof(fixed[0])) {
		*w = fixed[i - 1u];
		return true;
	}
	return false;
}

// CHIP_REG's SoC register windows (docs/spec/debuglink.md 3.24): the peripheral buses (APB1, APB2, AHB1, AHB2), the
// FSMC's control registers and the Cortex-M4's private peripheral bus (SCS, NVIC, SysTick, DWT, ITM, DBGMCU). Aligned
// words only. Reserved addresses inside them may fault the bus: the access takes that fault as a refusal (sysboot.c)
static inline bool stm32_reg_allowed(uint32_t addr)
{
	if ((addr & 3u) != 0u) {
		return false;
	}
	return (addr >= 0x40000000u && addr <= 0x5FFFFFFCu) || (addr >= 0xA0000000u && addr <= 0xA0000FFCu) ||
		(addr >= 0xE0000000u && addr <= 0xE00FFFFCu);
}

#endif
