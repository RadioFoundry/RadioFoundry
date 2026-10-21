// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 data flash: a 25Q128-class SPI NOR (16 MB) on SPI2 with the LCD, chip select PB12 (docs/spec/hardware-md9600.md
// 12). The core gets the DM-1701's layout; the board keeps one sector of its own for the stored board revision. The
// security registers hold the factory calibration (1-2) and the hardware options (3) and are only read.
#include "md9600.h"
#include "pins.h"
#include "flash/spi_nor/spi_nor.h"

#define FULL_SIZE		0x1000000u	// 16 MB: room for the open-firmware layout
#define SECURITY_REG_SIZE	256u
// The board revision record (rev_md9600.c): a sector of the hole below the open-firmware flash image that no CPS
// address maps to, above the core's settings and journal
#define REV_SECTOR		0x01F000u

static bool ready;

static void chip_select(bool active)
{
	PIN_WRITE(PIN_FLASH_CS, !active);
}

static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	return stm32_spi_xfer(SPI2, tx, rx, len);
}

static const spi_bus_t bus = { .select = chip_select, .xfer = xfer };
static spi_nor_t nor = { .bus = &bus };

// The open-firmware codeplug where the CPS tools expect it; settings and journal at 0x011000-0x01AFFF (the DM-1701's
// map). 0x010000-0x010FFF stays free. The stock TYT codeplug at 0 is overwritten by the first open-firmware write
static const hal_flash_map_t map = {
	.settings_addr = 0x011000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x019000u,	// 2 sectors
	.openfw_eeprom = 0x000000u,
	.openfw_flash = 0x020000u,
	.openfw_end = 0xE00000u,
	.calibration_addr = 0,		// the calibration is in the security registers
	.calibration_size = 0,
	.native_addr = 0xE00000u,
	.native_size = 0x100000u,
	.staging_addr = 0xF00000u,	// 256 KB of the unused top megabyte (docs/spec/tftp.md 4)
	.staging_size = 0x040000u,
};

static bool init(void)
{
	md9600_spi2_init();
	md9600_spi2_lock();
	// No assumption about the maker: any part that answers with a size (hardware-md9600.md 12.1)
	bool ok = spi_nor_probe(&nor) && nor.size != 0;
	md9600_spi2_unlock();
	ready = ok;
	md9600_flash.size = ok ? nor.size : 0;
	md9600_flash.map = ok && nor.size >= FULL_SIZE ? &map : NULL;
	return ok;
}

static bool in_range(uint32_t addr, uint32_t len)
{
	return ready && addr < nor.size && len <= nor.size - addr;
}

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	if (!in_range(addr, len)) {
		return false;
	}
	md9600_spi2_lock();
	bool ok = spi_nor_read(&nor, addr, buf, len);
	md9600_spi2_unlock();
	return ok;
}

static bool wr(uint32_t addr, const void *buf, uint32_t len)
{
	if (!in_range(addr, len)) {
		return false;
	}
	md9600_spi2_lock();
	bool ok = spi_nor_program(&nor, addr, buf, len);
	md9600_spi2_unlock();
	return ok;
}

static bool er(uint32_t addr)
{
	if (!in_range(addr, 1)) {
		return false;
	}
	md9600_spi2_lock();
	bool ok = spi_nor_erase_sector(&nor, addr);
	md9600_spi2_unlock();
	return ok;
}

hal_flash_t md9600_flash = {
	.init = init,
	.size = 0,
	.erase_size = SPI_NOR_SECTOR_SIZE,
	.read = rd,
	.write = wr,
	.erase = er,
	.map = NULL,	// set by init
};

bool md9600_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len)
{
	if (!ready || reg < 1 || reg > 3 || offset >= SECURITY_REG_SIZE || len > SECURITY_REG_SIZE - offset) {
		return false;
	}
	md9600_spi2_lock();
	bool ok = spi_nor_read_security(&nor, ((uint32_t)reg << 12) | offset, buf, len);
	md9600_spi2_unlock();
	return ok;
}

// The open-firmware CPS's area 10 (flash security registers), by the chip's register address (reg << 12 | offset)
bool md9600_cps_security_read(uint32_t addr, void *buf, uint32_t len)
{
	return md9600_flash_read_security((uint8_t)(addr >> 12), addr & 0xFFFu, buf, len);
}

uint16_t md9600_flash_device(void)
{
	return ready ? nor.device : 0;
}

md9600_rev_t md9600_flash_rev_load(void)
{
	uint8_t rec[MD9600_REV_RECORD_SIZE];
	if (!ready || nor.size < FULL_SIZE || !rd(REV_SECTOR, rec, sizeof(rec))) {
		return MD9600_REV_UNKNOWN;
	}
	return md9600_rev_record_decode(rec);
}

bool md9600_flash_rev_store(md9600_rev_t rev)
{
	uint8_t rec[MD9600_REV_RECORD_SIZE];
	if (!ready || nor.size < FULL_SIZE || md9600_rev_desc(rev) == NULL) {
		return false;
	}
	md9600_rev_record_encode(rev, rec);
	return er(REV_SECTOR) && wr(REV_SECTOR, rec, sizeof(rec)) && md9600_flash_rev_load() == rev;
}
