// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV data flash for the core (hal_flash_t): one device over RadioFoundry's two NAND ranges (hardware-d578uv.md
// 6.3). 4 KB sectors with NOR semantics over the sectors' range (drivers/flash/nand_sectors, docs/spec/nand-sectors.md),
// with the ID database's range (drivers/flash/nand_blocks, docs/spec/nand-blocks.md) mapped in right after the
// open-firmware flash image's first 1 MB, so the image, and the ID database's room in it, runs on into it. Every call
// holds the NAND's mutex for the whole operation: a rewrite reads the old pages, then moves them inside the chip.
#include "d578uv.h"
#include "config.h"
#include "flash/nand_blocks/nand_blocks.h"

#if CONFIG_NAND_STORAGE

// The device (hardware-d578uv.md 6.3), laid out as the SPI NOR radios' so the open-firmware codeplug images sit at the
// addresses the core's defaults use: the 64 KB "EEPROM" image at 0, the settings ring and the sector journal in the gap
// after it, the 1 MB "flash" image at 0x20000 and the ID database's 24 MB right after it (the image's end), then the
// native codeplug and the staging area. The sector layer holds all but the 24 MB: its addresses from 0x120000 show
// above the ID database's (EXT_END). The factory calibration stays in the factory NAND blocks, read only, so there is
// no calibration window
#define SECTORS		(0x260000u / NAND_SECTORS_SECTOR)
#define EXT		0x120000u					// where the ID database's range starts
#define EXT_SIZE	((uint32_t)D578UV_NAND_IDDB_LOGICAL * 64u * NAND_BLOCKS_PAGE)
#define EXT_END		(EXT + EXT_SIZE)

// Not const: without the ID database's range (it didn't mount) the image ends at EXT
static hal_flash_map_t map = {
	.settings_addr = 0x011000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x019000u,	// 2 sectors
	.openfw_eeprom = 0x000000u,
	.openfw_flash = 0x020000u,
	.openfw_end = 0x1920000u,	// EXT_END
	.calibration_addr = 0,
	.calibration_size = 0,
	.native_addr = 0x1920000u,	// the sector layer's 0x120000
	.native_size = 0x100000u,
	.staging_addr = 0x1A20000u,	// its 0x220000
	.staging_size = 0x040000u,
};
_Static_assert(EXT_END == 0x1920000u, "the map's literals (tools/boards.py reads them) follow EXT_END");

// The sector layer's RAM: 2 bytes per sector, 6 per block (1408 bytes)
static uint16_t sector_map[SECTORS];
static uint8_t block_live[D578UV_NAND_RFOUNDRY_BLOCKS];
static uint8_t block_flags[D578UV_NAND_RFOUNDRY_BLOCKS];
static uint32_t block_order[D578UV_NAND_RFOUNDRY_BLOCKS];
static nand_sectors_t layer = {
	.dev = &d578uv_nand_dev,
	.sectors = SECTORS,
	.map = sector_map,
	.live = block_live,
	.flags = block_flags,
	.order = block_order,
};

// The ID database's: 2 bytes per logical block, 1 per physical (416 bytes)
static uint16_t ext_map[D578UV_NAND_IDDB_LOGICAL];
static uint8_t ext_flags[D578UV_NAND_IDDB_BLOCKS];
static nand_blocks_t ext = {
	.dev = &d578uv_nand_iddb_dev,
	.blocks = D578UV_NAND_IDDB_LOGICAL,
	.map = ext_map,
	.flags = ext_flags,
};
static bool ext_up;

// A device range in one of the two layers: true for the ID database's range (*a made relative to it), false for the
// sector layer (*a its address there). Ranges don't straddle the boundaries: the core's sectors and the callers'
// reads are split by span()
static bool in_ext(uint32_t *a)
{
	if (*a >= EXT && *a < EXT_END) {
		*a -= EXT;
		return true;
	}
	if (*a >= EXT_END) {
		*a -= EXT_SIZE;
	}
	return false;
}

// Bytes from addr to the next boundary between the layers
static uint32_t span(uint32_t addr, uint32_t len)
{
	uint32_t edge = addr < EXT ? EXT : addr < EXT_END ? EXT_END : 0xFFFFFFFFu;
	return edge - addr < len ? edge - addr : len;
}

static bool init(void)
{
	return d578uv_flash_init();
}

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	uint8_t *p = buf;
	bool ok = true;
	d578uv_nand_lock();
	while (ok && len > 0) {
		uint32_t n = span(addr, len), a = addr;
		ok = in_ext(&a) ? ext_up && nand_blocks_read(&ext, a, p, n) : nand_sectors_read(&layer, a, p, n);
		addr += n;
		p += n;
		len -= n;
	}
	d578uv_nand_unlock();
	return ok;
}

// A change in the sector layer first finishes the ID database's open copy: whatever the core writes there next (an
// ID database header, a staging record, a setting) may count on what it wrote before
static bool sync_ext(void)
{
	return !ext_up || nand_blocks_sync(&ext);
}

static bool wr(uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	bool ok = true;
	d578uv_nand_lock();
	while (ok && len > 0) {
		uint32_t n = span(addr, len), a = addr;
		ok = in_ext(&a) ? ext_up && nand_blocks_write(&ext, a, p, n) : sync_ext() && nand_sectors_write(&layer, a, p, n);
		addr += n;
		p += n;
		len -= n;
	}
	d578uv_nand_unlock();
	return ok;
}

static bool er(uint32_t addr)
{
	d578uv_nand_lock();
	bool ok = in_ext(&addr) ? ext_up && nand_blocks_erase(&ext, addr) : sync_ext() && nand_sectors_erase(&layer, addr);
	d578uv_nand_unlock();
	return ok;
}

// One sector's new content in one step (nand_sectors_replace, nand_blocks_replace): the core's codeplug sector
// rewrite uses it, with no journal and no erase first
static bool rep(uint32_t addr, const void *data)
{
	d578uv_nand_lock();
	bool ok = in_ext(&addr) ? ext_up && nand_blocks_replace(&ext, addr, data) :
				  sync_ext() && nand_sectors_replace(&layer, addr, data);
	d578uv_nand_unlock();
	return ok;
}

static bool syn(void)
{
	d578uv_nand_lock();
	bool ok = sync_ext();
	d578uv_nand_unlock();
	return ok;
}

hal_flash_t d578uv_flash = {
	.init = init,
	.size = 0,
	.erase_size = NAND_SECTORS_SECTOR,
	.read = rd,
	.write = wr,
	.erase = er,
	.replace = rep,
	.sync = syn,
	.map = &map,
};

bool d578uv_flash_init(void)
{
	d578uv_nand_lock();
	bool ok = d578uv_nand_ready() && nand_sectors_mount(&layer);
	// The ID database's range: its own mount. Without it (foreign data in the range, a bad setup) the image ends where
	// the sector layer's first part does, as on the NOR radios, and the database keeps that room
	ext_up = ok && nand_blocks_mount(&ext);
	if (!ext_up) {
		map.openfw_end = EXT;
	}
	d578uv_nand_unlock();
	d578uv_flash.size = ok ? nand_sectors_size(&layer) + EXT_SIZE : 0;
	return ok;
}

void d578uv_flash_ext_stats(nand_blocks_stats_t *st)
{
	d578uv_nand_lock();
	nand_blocks_get_stats(&ext, st);
	d578uv_nand_unlock();
}

void d578uv_flash_stats(nand_sectors_stats_t *st)
{
	d578uv_nand_lock();
	nand_sectors_get_stats(&layer, st);
	d578uv_nand_unlock();
}

#endif
