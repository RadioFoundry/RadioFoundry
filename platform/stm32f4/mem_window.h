// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4's READ_MEM windows (docs/spec/debuglink.md 3.10), no hardware access (host tested): internal flash up to
// its size, system memory, OTP and option bytes. Never RAM, CCM or peripherals: the debug link has no use for them
// here, and a peripheral read can clear a flag or pop a FIFO.
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

#endif
