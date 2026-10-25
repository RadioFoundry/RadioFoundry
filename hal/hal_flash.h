// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Data flash (external SPI NOR on most radios)
#ifndef HAL_FLASH_H
#define HAL_FLASH_H

#include <stdint.h>
#include <stdbool.h>

// Where the core keeps its data in the board's data flash. Every area is whole erase units. A size of 0 means the
// board has no such area (the feature then runs from RAM only, or is refused)
typedef struct {
	// The settings store (core/settings): a ring of at least 2 erase units, written record by record
	uint32_t settings_addr;
	uint32_t settings_size;
	// The sector journal (core/storage): 2 erase units, a copy of the sector being rewritten and its header, so a
	// power cut during a codeplug sector rewrite can be finished at the next boot
	uint32_t journal_addr;
	// The open-firmware codeplug (docs/spec/open-firmware-cps.md 7): the 64 KB "EEPROM" image and the 1 MB "flash"
	// image as physical addresses (the STM32 radios: 0 and 0x20000), and the end of the area an open-firmware CPS may
	// write
	uint32_t openfw_eeprom;
	uint32_t openfw_flash;
	uint32_t openfw_end;
	// The calibration window the open-firmware CPS reads and writes (open-firmware-cps.md 2.2), 0 size if none
	uint32_t calibration_addr;
	uint32_t calibration_size;
	// Reserved for the native codeplug (docs/spec/codeplug-native.md); never written by the open-firmware CPS
	uint32_t native_addr;
	uint32_t native_size;
	// Scratch for a whole new codeplug image before it replaces the stored one (the TFTP codeplug write,
	// docs/spec/tftp.md 4): outside every area above, never written by the open-firmware CPS. 0 size: no staged writes
	uint32_t staging_addr;
	uint32_t staging_size;
} hal_flash_map_t;

typedef struct {
	bool (*init)(void);
	uint32_t size;		// bytes, filled in by init on boards that probe it
	uint32_t erase_size;	// smallest erase unit
	bool (*read)(uint32_t addr, void *buf, uint32_t len);
	// Program already erased bytes; any length and alignment
	bool (*write)(uint32_t addr, const void *buf, uint32_t len);
	bool (*erase)(uint32_t addr);	// one erase_size unit
	// Optional: replace one erase unit with new content of any bits in a single power-cut safe step (the old unit or
	// the new one survives, never a mix). Flash that is copy on write underneath (NAND behind a sector layer) offers
	// it; core/storage then rewrites a codeplug sector with it directly, with no journal and no erase first. NULL: the
	// journaled erase and program
	bool (*replace)(uint32_t addr, const void *data);
	const hal_flash_map_t *map;	// NULL: no areas for the core (nothing is stored)
} hal_flash_t;

#endif
