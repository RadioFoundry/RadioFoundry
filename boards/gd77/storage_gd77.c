// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 storage map (storage_gd77.h, docs/spec/hardware-gd77.md 6.2, 6.3)
#include <stddef.h>
#include "storage_gd77.h"

// A 2 MB part: the open-firmware images, the settings ring and the journal. No native codeplug and no staging area:
// the second megabyte is full
static const hal_flash_map_t map_2mb = {
	.settings_addr = GD77_RFOUNDRY_SETTINGS,
	.settings_size = GD77_RFOUNDRY_SETTINGS_SIZE,
	.journal_addr = GD77_RFOUNDRY_JOURNAL,
	.openfw_eeprom = GD77_RFOUNDRY_EEPROM_IMAGE,
	.openfw_flash = GD77_RFOUNDRY_FLASH_IMAGE,
	.openfw_end = GD77_RFOUNDRY_FLASH_END,
	.calibration_addr = 0,		// the factory calibration is in the EEPROM, read only (6.4)
	.calibration_size = 0,
	.native_addr = 0,
	.native_size = 0,
	.staging_addr = 0,
	.staging_size = 0,
};

// 4 MB and up: the same, plus the native codeplug and the staging area in the third and fourth megabytes
static const hal_flash_map_t map_4mb = {
	.settings_addr = GD77_RFOUNDRY_SETTINGS,
	.settings_size = GD77_RFOUNDRY_SETTINGS_SIZE,
	.journal_addr = GD77_RFOUNDRY_JOURNAL,
	.openfw_eeprom = GD77_RFOUNDRY_EEPROM_IMAGE,
	.openfw_flash = GD77_RFOUNDRY_FLASH_IMAGE,
	.openfw_end = GD77_RFOUNDRY_FLASH_END,
	.calibration_addr = 0,
	.calibration_size = 0,
	.native_addr = GD77_RFOUNDRY_NATIVE,
	.native_size = GD77_RFOUNDRY_NATIVE_SIZE,
	.staging_addr = GD77_RFOUNDRY_STAGING,
	.staging_size = GD77_RFOUNDRY_STAGING_SIZE,
};

const hal_flash_map_t *gd77_storage_map(uint32_t nor_size)
{
	if (nor_size >= GD77_RFOUNDRY_STAGING + GD77_RFOUNDRY_STAGING_SIZE) {
		return &map_4mb;
	}
	return nor_size >= GD77_NOR_MIN_SIZE ? &map_2mb : NULL;
}

bool gd77_storage_writable(uint32_t addr, uint32_t len, uint32_t nor_size)
{
	return nor_size >= GD77_NOR_MIN_SIZE && addr >= GD77_NOR_FACTORY_END && addr < nor_size &&
	       len <= nor_size - addr;
}

bool gd77_logical_piece(uint32_t logical, uint32_t len, gd77_logical_piece_t *out)
{
	if (len == 0 || logical >= GD77_LOGICAL_SIZE || len > GD77_LOGICAL_SIZE - logical) {
		return false;
	}
	if (logical < GD77_LOGICAL_EEPROM_END) {
		uint32_t room = GD77_LOGICAL_EEPROM_END - logical;
		out->eeprom = true;
		out->addr = logical;
		out->len = len < room ? len : room;
	} else {
		out->eeprom = false;
		out->addr = logical + GD77_LOGICAL_NOR_OFFSET;
		out->len = len;
	}
	return true;
}
