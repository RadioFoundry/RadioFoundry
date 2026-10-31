// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's chip commands, the portable parts (host tested): option byte decoding, the READ_MEM address
// window check, the REBOOT safety gate (docs/spec/debuglink.md 3.9-3.11). The windows themselves are the platform's
// (hal_system_t.mem_window).
#ifndef DL_CHIP_H
#define DL_CHIP_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_system.h"

#define DL_MEM_READ_MAX		1000u	// READ_MEM bytes per reply

// FLASH_OPTCR fields (STM32F405/407: RDP 15:8, nWRP 27:16 one bit per sector 0..11, 0 = protected, BOR_LEV 3:2,
// WDG_SW 5, nRST_STOP 6, nRST_STDBY 7)
#define DL_RDP_LEVEL0_BYTE	0xAAu
#define DL_RDP_LEVEL2_BYTE	0xCCu
static inline uint8_t dl_rdp_byte(uint32_t optcr)
{
	return (uint8_t)(optcr >> 8);
}
// 0, 1 or 2
static inline uint8_t dl_rdp_level(uint32_t optcr)
{
	uint8_t b = dl_rdp_byte(optcr);
	return b == DL_RDP_LEVEL0_BYTE ? 0 : (b == DL_RDP_LEVEL2_BYTE ? 2 : 1);
}
// Write-protected sectors, bit n = sector n protected
static inline uint16_t dl_wrp_mask(uint32_t optcr)
{
	return (uint16_t)(~(optcr >> 16) & 0x0FFFu);
}
static inline uint8_t dl_bor_level(uint32_t optcr)
{
	return (uint8_t)((optcr >> 2) & 3u);
}
static inline uint8_t dl_user_bits(uint32_t optcr)
{
	return (uint8_t)((optcr >> 5) & 7u);	// bit 0 WDG_SW, 1 nRST_STOP, 2 nRST_STDBY
}

// READ_MEM: the whole range [addr, addr + len) inside one of the platform's windows (window(i, &w) for i = 0, 1, ...
// until it returns false); in a words window (peripheral registers) only whole aligned words. len 0 or no windows:
// refused
bool dl_mem_allowed(uint32_t addr, uint32_t len, bool (*window)(unsigned i, hal_mem_window_t *w));

// REBOOT refusal reasons (the reply's second byte when the status is DL_ST_REFUSED)
enum {
	DL_REBOOT_OK		= 0,
	DL_REBOOT_BAD_TOKEN	= 1,	// the token isn't the chip UID's low word
	DL_REBOOT_RDP		= 2,	// ROM DFU asked with RDP not level 0, or on a chip without it
	DL_REBOOT_TX		= 3,	// transmitting (radio or hotspot)
	DL_REBOOT_NO_CHIP	= 4,	// the chip info could not be read
	DL_REBOOT_ARM_FAILED	= 5,	// the ROM DFU flag could not be stored
};

// The gate: mode is HAL_REBOOT_*, chip NULL when unreadable. Returns DL_REBOOT_*
uint8_t dl_reboot_check(uint8_t mode, uint32_t token, const hal_chip_t *chip, bool transmitting);

#endif
