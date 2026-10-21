// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 data flash: a 25Q128-class SPI NOR (16 MB) on SPI2 with the LCD, chip select PB12 (docs/spec/hardware-md9600.md
// 12). The map and the board's record sector are in store_md9600.c. The security registers hold the factory
// calibration (1-2) and the hardware options (3) and are only read.
#include "md9600.h"
#include "pins.h"
#include "store_md9600.h"
#include "flash/spi_nor/spi_nor.h"

#define SECURITY_REG_SIZE	256u

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

static bool init(void)
{
	md9600_spi2_init();
	md9600_spi2_lock();
	// No assumption about the maker: any part that answers with a size (hardware-md9600.md 12.1)
	bool ok = spi_nor_probe(&nor) && nor.size != 0;
	md9600_spi2_unlock();
	ready = ok;
	md9600_flash.size = ok ? nor.size : 0;
	md9600_flash.map = ok && nor.size >= MD9600_STORE_FULL_SIZE ? &md9600_flash_map : NULL;
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
	return ready ? md9600_store_rev_load(&md9600_flash) : MD9600_REV_UNKNOWN;
}

bool md9600_flash_rev_store(md9600_rev_t rev)
{
	return ready && md9600_store_rev_save(&md9600_flash, rev);
}
