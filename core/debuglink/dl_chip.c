// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's chip commands, the portable parts (dl_chip.h)
#include "debuglink/dl_chip.h"

static bool inside(uint32_t addr, uint32_t len, uint32_t lo, uint32_t hi)
{
	// len >= 1; no overflow: addr <= hi and len - 1 <= hi - addr
	return addr >= lo && addr <= hi && (len - 1u) <= (hi - addr);
}

bool dl_mem_allowed(uint32_t addr, uint32_t len, bool (*window)(unsigned i, hal_mem_window_t *w))
{
	hal_mem_window_t w;

	if (len == 0 || window == NULL) {
		return false;
	}
	for (unsigned i = 0; window(i, &w); i++) {
		if (inside(addr, len, w.lo, w.hi)) {
			return !w.words || ((addr | len) & 3u) == 0;
		}
	}
	return false;
}

uint8_t dl_reboot_check(uint8_t mode, uint32_t token, const hal_chip_t *chip, bool transmitting)
{
	if (chip == NULL) {
		return DL_REBOOT_NO_CHIP;
	}
	if (token != chip->uid[0]) {
		return DL_REBOOT_BAD_TOKEN;
	}
	if (transmitting) {
		return DL_REBOOT_TX;
	}
	// With RDP level 1 the ROM bootloader refuses flash access, and its read-unprotect command mass-erases all of
	// flash, the stock bootloader included: never offer the ROM path then. Level 2 disables the ROM bootloader. A chip
	// without option bytes has no such ROM path to offer
	if (mode == HAL_REBOOT_ROM_DFU && (chip->family != HAL_CHIP_STM32F4 || dl_rdp_level(chip->optcr) != 0)) {
		return DL_REBOOT_RDP;
	}
	return DL_REBOOT_OK;
}
