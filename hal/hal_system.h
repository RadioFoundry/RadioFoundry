// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The MCU itself: chip identity and option bytes, raw memory reads, reboot (the debug link's INFO_CHIP, READ_MEM and
// REBOOT, docs/spec/debuglink.md). Optional: board.system is NULL where the platform has none.
#ifndef HAL_SYSTEM_H
#define HAL_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t idcode;	// DBGMCU IDCODE (may read 0 without a debugger on some STM32F40x revisions)
	uint16_t flash_kb;	// flash size register
	uint32_t uid[3];	// 96-bit unique ID, uid[0] = the lowest word
	uint32_t optcr;		// FLASH_OPTCR: RDP byte, nWRP bits, BOR level, user bits
} hal_chip_t;

enum {
	HAL_REBOOT_NORMAL	= 0,	// system reset: the stock bootloader, then the application
	HAL_REBOOT_ROM_DFU	= 1,	// system reset, then straight into the MCU's ROM bootloader (USB DfuSe)
};

typedef struct {
	bool (*chip)(hal_chip_t *c);
	// Copy n bytes from a memory address the caller has already checked (dl_mem_allowed)
	void (*read)(uint32_t addr, void *dst, size_t n);
	// Does not return (or returns false when the request could not be armed, e.g. the backup register didn't stick)
	bool (*reboot)(uint8_t mode);
	// Runtime RAM-monitor entry (CONFIG_STUB_ENTRY, docs/spec/debuglink.md 3.18), both optional (NULL where the
	// platform has none). stub_region returns a writable RAM span the host image is loaded into and its length in
	// *size, or NULL if unsupported. enter_stub quiesces the SoC (interrupts masked, watchdog off, DMA stopped, flash
	// controller handed back) and jumps to entry; it does not return on success, and returns false only if it could
	// not jump. stub_run_addr is where the image runs: 0 means where it is loaded (the HR_C7000), otherwise
	// enter_stub copies the span there first (the STM32: SRAM base, which ODR's own data holds until the jump)
	uint8_t *(*stub_region)(uint32_t *size);
	bool (*enter_stub)(uint32_t entry);
	uint32_t stub_run_addr;
} hal_system_t;

#endif
