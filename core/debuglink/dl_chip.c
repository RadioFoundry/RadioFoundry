// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The debug link's chip commands, the portable parts (dl_chip.h)
#include "debuglink/dl_chip.h"

typedef struct {
	uint32_t lo, hi;	// inclusive
} range_t;

static bool inside(uint32_t addr, uint32_t len, uint32_t lo, uint32_t hi)
{
	// len >= 1; no overflow: addr <= hi and len - 1 <= hi - addr
	return addr >= lo && addr <= hi && (len - 1u) <= (hi - addr);
}

bool dl_mem_allowed(uint32_t addr, uint32_t len, uint16_t flash_kb)
{
	static const range_t fixed[] = {
		{ 0x1FFF0000u, 0x1FFF77FFu },	// system memory (ROM bootloader)
		{ 0x1FFF7800u, 0x1FFF7A0Fu },	// OTP and its lock bytes
		{ 0x1FFFC000u, 0x1FFFC00Fu },	// option bytes
	};
	uint32_t flash_bytes = (flash_kb >= 16u && flash_kb <= 1024u) ? (uint32_t)flash_kb * 1024u : 0x100000u;

	if (len == 0) {
		return false;
	}
	if (inside(addr, len, 0x08000000u, 0x08000000u + flash_bytes - 1u)) {
		return true;
	}
	for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
		if (inside(addr, len, fixed[i].lo, fixed[i].hi)) {
			return true;
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
	// flash, the stock bootloader included: never offer the ROM path then. Level 2 disables the ROM bootloader
	if (mode == HAL_REBOOT_ROM_DFU && dl_rdp_level(chip->optcr) != 0) {
		return DL_REBOOT_RDP;
	}
	return DL_REBOOT_OK;
}
