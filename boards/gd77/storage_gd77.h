// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 storage map (docs/spec/hardware-gd77.md 6.2, 6.3): where RadioFoundry keeps its data on the SPI NOR, which ranges of
// both chips RadioFoundry never writes, and the radio's own logical codeplug space across the EEPROM and the NOR (read only,
// for diagnostics and a future import). No platform code here, so the host tests run it.
#ifndef STORAGE_GD77_H
#define STORAGE_GD77_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_flash.h"

// --- The EEPROM (24C512 class on I2C bus A) ---
#define GD77_EEPROM_ADDR7	0x50u		// 0xA0 / 0xA1 on the wire
#define GD77_EEPROM_SIZE	0x10000u
#define GD77_EEPROM_PAGE	64u
#define GD77_EEPROM_WRITE_MS	5u

// --- The SPI NOR ---
// The first megabyte holds the factory header and the radio's codeplug as it came: RadioFoundry never erases or programs it
#define GD77_NOR_FACTORY_END	0x100000u
#define GD77_NOR_MIN_SIZE	0x200000u	// smaller parts: no room for RadioFoundry, nothing is stored

// RadioFoundry's areas, all in the second megabyte and above (the layout of the STM32 radios, moved up by 1 MB)
#define GD77_RFOUNDRY_EEPROM_IMAGE	0x100000u	// the open-firmware 64 KB EEPROM image
#define GD77_RFOUNDRY_SETTINGS	0x111000u	// settings ring, 8 sectors
#define GD77_RFOUNDRY_SETTINGS_SIZE	0x008000u
#define GD77_RFOUNDRY_JOURNAL	0x119000u	// sector journal, 2 sectors
#define GD77_RFOUNDRY_FLASH_IMAGE	0x120000u	// the open-firmware flash image (image offset 0 here)
#define GD77_RFOUNDRY_FLASH_END	0x200000u	// 896 KB of flash image: the codeplug and the DMR ID database
#define GD77_RFOUNDRY_NATIVE		0x200000u	// native codeplug, 1 MB, on parts of 4 MB and up
#define GD77_RFOUNDRY_NATIVE_SIZE	0x100000u
#define GD77_RFOUNDRY_STAGING	0x300000u	// codeplug staging, 256 KB, on parts of 4 MB and up
#define GD77_RFOUNDRY_STAGING_SIZE	0x040000u

// The map the core gets for a NOR part of nor_size bytes (as probed), NULL when the part is too small or unknown
const hal_flash_map_t *gd77_storage_map(uint32_t nor_size);
// Whether RadioFoundry may erase or program [addr, addr + len) of a part of nor_size bytes: only inside RadioFoundry's areas, never in
// the first megabyte
bool gd77_storage_writable(uint32_t addr, uint32_t len, uint32_t nor_size);

// --- The radio's own logical codeplug space (read only to RadioFoundry) ---
// Logical addresses 0..0xB1AF are the EEPROM at the same address; 0xB1B0..0x1FFFF are the NOR at logical + 0x70000.
// The four EEPROM bytes at 0xB1B0 are not logical: they hold the target of an interrupted NOR sector rewrite
#define GD77_LOGICAL_SIZE	0x20000u
#define GD77_LOGICAL_EEPROM_END	0xB1B0u
#define GD77_LOGICAL_NOR_OFFSET	0x70000u
#define GD77_REWRITE_PTR	0xB1B0u		// EEPROM: u32 target of a pending rewrite, 0 or 0xFFFFFFFF when none
#define GD77_REWRITE_SCRATCH	0x00D000u	// NOR: the 4 KB copy that rewrite is made from

typedef struct {
	bool eeprom;		// the piece is in the EEPROM (else the NOR)
	uint32_t addr;		// chip address
	uint32_t len;		// bytes in this piece
} gd77_logical_piece_t;

// The first piece of a logical read [logical, logical + len): a read that straddles 0xB1B0 comes back as two pieces,
// call again with the rest. False when the range is outside the logical space or len is 0
bool gd77_logical_piece(uint32_t logical, uint32_t len, gd77_logical_piece_t *out);

#endif
