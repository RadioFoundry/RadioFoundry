// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
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
	// The legacy-layout codeplug (docs/spec/legacy-cps-interop.md 7): the 64 KB "EEPROM" image and the 1 MB "flash"
	// image as physical addresses (the STM32 radios: 0 and 0x20000), and the end of the area a legacy CPS may write
	uint32_t legacy_eeprom;
	uint32_t legacy_flash;
	uint32_t legacy_end;
	// The calibration window the legacy CPS reads and writes (legacy-cps-interop.md 2.2), 0 size if none
	uint32_t calibration_addr;
	uint32_t calibration_size;
	// Reserved for the native codeplug (docs/spec/codeplug-native.md); never written by the legacy CPS
	uint32_t native_addr;
	uint32_t native_size;
	// Scratch for a whole new codeplug image before it replaces the stored one (the TFTP codeplug write,
	// docs/spec/tftp.md 4): outside every area above, never written by the legacy CPS. 0 size: no staged writes
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
	const hal_flash_map_t *map;	// NULL: no areas for the core (nothing is stored)
} hal_flash_t;

#endif
