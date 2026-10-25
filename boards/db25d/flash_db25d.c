// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D data flash for the core (hal_flash_t): 4 KB sectors with NOR semantics over RadioFoundry's own NAND block range
// (drivers/flash/nand_sectors, docs/spec/nand-sectors.md; hardware-db25d.md 6.6). Every call holds the NAND's mutex
// for the whole operation: a rewrite reads the old pages, then moves them inside the chip.
#include "db25d.h"
#include "config.h"

#if CONFIG_NAND_STORAGE

// The logical device (hardware-db25d.md 6.6), laid out as the STM32 radios' SPI NOR so the open-firmware codeplug
// images sit at the addresses the core's defaults use: the 64 KB "EEPROM" image at 0, the settings ring and the
// sector journal in the gap after it, the 1 MB "flash" image at 0x20000, then the native codeplug and the staging
// area. The factory calibration is read only (it stays in the stock NAND page), so there is no calibration window
#define SECTORS		(0x260000u / NAND_SECTORS_SECTOR)

static const hal_flash_map_t map = {
	.settings_addr = 0x011000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x019000u,	// 2 sectors
	.openfw_eeprom = 0x000000u,
	.openfw_flash = 0x020000u,
	.openfw_end = 0x120000u,
	.calibration_addr = 0,
	.calibration_size = 0,
	.native_addr = 0x120000u,
	.native_size = 0x100000u,
	.staging_addr = 0x220000u,
	.staging_size = 0x040000u,
};

// The layer's RAM: 2 bytes per sector, 6 per block (1408 bytes)
static uint16_t sector_map[SECTORS];
static uint8_t block_live[DB25D_NAND_RFOUNDRY_BLOCKS];
static uint8_t block_flags[DB25D_NAND_RFOUNDRY_BLOCKS];
static uint32_t block_order[DB25D_NAND_RFOUNDRY_BLOCKS];
static nand_sectors_t layer = {
	.dev = &db25d_nand_dev,
	.sectors = SECTORS,
	.map = sector_map,
	.live = block_live,
	.flags = block_flags,
	.order = block_order,
};

static bool init(void)
{
	return db25d_flash_init();
}

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	db25d_nand_lock();
	bool ok = nand_sectors_read(&layer, addr, buf, len);
	db25d_nand_unlock();
	return ok;
}

static bool wr(uint32_t addr, const void *buf, uint32_t len)
{
	db25d_nand_lock();
	bool ok = nand_sectors_write(&layer, addr, buf, len);
	db25d_nand_unlock();
	return ok;
}

static bool er(uint32_t addr)
{
	db25d_nand_lock();
	bool ok = nand_sectors_erase(&layer, addr);
	db25d_nand_unlock();
	return ok;
}

// One sector's new content in one record (nand_sectors_replace): the core's codeplug sector rewrite uses it, with no
// journal and no erase first
static bool rep(uint32_t addr, const void *data)
{
	db25d_nand_lock();
	bool ok = nand_sectors_replace(&layer, addr, data);
	db25d_nand_unlock();
	return ok;
}

hal_flash_t db25d_flash = {
	.init = init,
	.size = 0,
	.erase_size = NAND_SECTORS_SECTOR,
	.read = rd,
	.write = wr,
	.erase = er,
	.replace = rep,
	.map = &map,
};

bool db25d_flash_init(void)
{
	db25d_nand_lock();
	bool ok = db25d_nand_ready() && nand_sectors_mount(&layer);
	db25d_nand_unlock();
	db25d_flash.size = ok ? nand_sectors_size(&layer) : 0;
	return ok;
}

void db25d_flash_stats(nand_sectors_stats_t *st)
{
	db25d_nand_lock();
	nand_sectors_get_stats(&layer, st);
	db25d_nand_unlock();
}

#endif
