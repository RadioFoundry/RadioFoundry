// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MCU itself: chip identity and option bytes, raw memory reads, reboot (the debug link's INFO_CHIP, READ_MEM and
// REBOOT, docs/spec/debuglink.md). Optional: board.system is NULL where the platform has none.
#ifndef HAL_SYSTEM_H
#define HAL_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Which chip the fields describe. Only the STM32F4 has the ID, flash size and option byte registers; the others fill in
// the UID and leave the rest 0
enum { HAL_CHIP_STM32F4 = 0, HAL_CHIP_HRC7000 = 1 };

typedef struct {
	uint32_t idcode;	// DBGMCU IDCODE (may read 0 without a debugger on some STM32F40x revisions)
	uint16_t flash_kb;	// flash size register
	uint32_t uid[3];	// 96-bit unique ID, uid[0] = the lowest word
	uint32_t optcr;		// FLASH_OPTCR: RDP byte, nWRP bits, BOR level, user bits
	uint8_t family;		// HAL_CHIP_*
} hal_chip_t;

// A memory window READ_MEM may read (docs/spec/debuglink.md 3.10), lo..hi inclusive. A window with words set is
// peripheral registers: only aligned 32-bit words, read one 32-bit load at a time (a byte or unaligned access there can
// fault the bus or read the wrong lane)
typedef struct {
	uint32_t lo, hi;
	bool words;
} hal_mem_window_t;

enum {
	HAL_REBOOT_NORMAL	= 0,	// system reset: the stock bootloader, then the application
	HAL_REBOOT_ROM_DFU	= 1,	// system reset, then straight into the MCU's ROM bootloader (USB DfuSe)
};

typedef struct {
	bool (*chip)(hal_chip_t *c);
	// Copy n bytes from a memory address the caller has already checked (dl_mem_allowed). Inside a words window the
	// caller passes an aligned address and length, and the copy is one 32-bit load per word
	void (*read)(uint32_t addr, void *dst, size_t n);
	// The readable windows, one per call: fills *w with window i and returns true, or false past the last. NULL: READ_MEM
	// is unavailable. Only windows a read can't disturb: memory, and register blocks without read-to-clear bits or FIFOs
	bool (*mem_window)(unsigned i, hal_mem_window_t *w);
	// Does not return (or returns false when the request could not be armed, e.g. the backup register didn't stick)
	bool (*reboot)(uint8_t mode);
	// Runtime RAM-monitor entry (CONFIG_STUB_ENTRY, docs/spec/debuglink.md 3.18), both optional (NULL where the
	// platform has none). stub_region returns a writable RAM span the host image is loaded into and its length in
	// *size, or NULL if unsupported. enter_stub quiesces the SoC (interrupts masked, watchdog off, DMA stopped, flash
	// controller handed back) and jumps to entry; it does not return on success, and returns false only if it could
	// not jump. stub_run_addr is where the image runs: 0 means where it is loaded (the HR_C7000), otherwise
	// enter_stub copies the span there first (the STM32: SRAM base, which RadioFoundry's own data holds until the jump)
	uint8_t *(*stub_region)(uint32_t *size);
	bool (*enter_stub)(uint32_t entry);
	// Optional (NULL: none): interrupts the platform's fallback handler took for a source with no handler (it masks the
	// source so it can't storm), and the last such source. A count above 0 means some interrupt went quiet for good
	void (*irq_unhandled)(uint32_t *count, uint32_t *last_source);
#if defined(CONFIG_STUB_ENTRY) && CONFIG_STUB_ENTRY
	uint32_t stub_run_addr;		// last, so a file built without config.h sees the same layout before it
	// Optional (NULL: any image): the loaded image may run here. The STM32 refuses one that doesn't feed the IWDG
	bool (*stub_image_ok)(const uint8_t *img, uint32_t len);
#endif
} hal_system_t;

#endif
