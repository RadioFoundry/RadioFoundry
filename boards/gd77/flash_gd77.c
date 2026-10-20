// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 data flash: an SPI NOR chip on PTE4 (MOSI), PTE5 (SCK), PTE6 (MISO) with its chip select on PTA19, reached
// through gd77_nor_bus (docs/spec/hardware-gd77.md 6). Read only in this build: the radio's codeplug and settings live
// partly in the EEPROM on I2C bus A and partly in this chip, and that split isn't wired into the core's storage yet, so
// the board gives the core no flash map (nothing is stored) and refuses every write and erase.
#include "gd77.h"
#include "pins.h"
#include "platform.h"
#include "flash/spi_nor/spi_nor.h"

static spi_nor_t nor = { .bus = &gd77_nor_bus };
static bool probed;

static bool init(void)
{
	probed = spi_nor_probe(&nor);
	return probed;
}

uint32_t gd77_flash_jedec(void)
{
	return probed ? ((uint32_t)nor.manufacturer << 16) | nor.device : 0;
}

static bool read(uint32_t addr, void *buf, uint32_t len)
{
	return probed && spi_nor_read(&nor, addr, buf, len);
}

static bool write(uint32_t addr, const void *buf, uint32_t len)
{
	(void)addr;
	(void)buf;
	(void)len;
	return false;
}

static bool erase(uint32_t addr)
{
	(void)addr;
	return false;
}

const hal_flash_t gd77_flash = {
	.init = init,
	.size = 0,		// the core sees no flash until the storage split is wired (hardware-gd77.md 6)
	.erase_size = SPI_NOR_SECTOR_SIZE,
	.read = read,
	.write = write,
	.erase = erase,
	.map = NULL,
};
