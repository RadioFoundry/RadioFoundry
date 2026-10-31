// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 data flash: an SPI NOR chip on PTE4 (MOSI), PTE5 (SCK), PTE6 (MISO) with its chip select on PTA19, reached
// through gd77_nor_bus (docs/spec/hardware-gd77.md 6.2). The core gets the chip with the map of storage_gd77.c: RadioFoundry's
// settings, journal and codeplug images from the second megabyte up. Every erase and program below 1 MB is refused
// here, whatever asks for it, so the radio's own data and factory header stay as they are. One mutex serializes every
// transaction once the scheduler runs.
#include "gd77.h"
#include "storage_gd77.h"
#include "flash/spi_nor/spi_nor.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

static spi_nor_t nor = { .bus = &gd77_nor_bus, .fast_read = true };
static SemaphoreHandle_t lock;
static bool probed;
static uint32_t refusals;

static void take(void)
{
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreTake(lock, portMAX_DELAY);
	}
}

static void give(void)
{
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreGive(lock);
	}
}

// A part too small for RadioFoundry's areas (or of unknown size) is still read for the diagnostics, but the core sees no flash
static bool init(void)
{
	if (lock == NULL) {
		lock = xSemaphoreCreateMutex();
	}
	take();
	probed = spi_nor_probe(&nor);
	give();
	const hal_flash_map_t *m = probed ? gd77_storage_map(nor.size) : NULL;
	gd77_flash.size = m != NULL ? nor.size : 0;
	gd77_flash.map = m;
	return m != NULL;
}

uint32_t gd77_flash_jedec(void)
{
	return probed ? ((uint32_t)nor.manufacturer << 16) | nor.device : 0;
}

uint32_t gd77_flash_refusals(void)
{
	return refusals;
}

bool gd77_flash_raw_read(uint32_t addr, void *buf, uint32_t len)
{
	if (!probed || nor.size == 0 || addr >= nor.size || len > nor.size - addr) {
		return false;
	}
	take();
	bool ok = spi_nor_read(&nor, addr, buf, len);
	give();
	return ok;
}

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	return gd77_flash.size != 0 && gd77_flash_raw_read(addr, buf, len);
}

static bool writable(uint32_t addr, uint32_t len)
{
	if (gd77_flash.size == 0 || !gd77_storage_writable(addr, len, nor.size)) {
		refusals++;
		return false;
	}
	return true;
}

static bool wr(uint32_t addr, const void *buf, uint32_t len)
{
	if (!writable(addr, len)) {
		return false;
	}
	take();
	bool ok = spi_nor_program(&nor, addr, buf, len);
	give();
	return ok;
}

static bool er(uint32_t addr)
{
	if (!writable(addr & ~(SPI_NOR_SECTOR_SIZE - 1u), SPI_NOR_SECTOR_SIZE)) {
		return false;
	}
	take();
	bool ok = spi_nor_erase_sector(&nor, addr);
	give();
	return ok;
}

hal_flash_t gd77_flash = {
	.init = init,
	.size = 0,		// set by init: the part's size when RadioFoundry's areas fit, else 0
	.erase_size = SPI_NOR_SECTOR_SIZE,
	.read = rd,
	.write = wr,
	.erase = er,
	.map = NULL,		// set by init (storage_gd77.c)
};
