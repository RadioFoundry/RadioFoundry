// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV data flash: 16 MB SPI NOR (GD25Q128 or Puya PY25Q128HA) on the DesignWare SSI master SPI0
// (platform/hrc7000/ssi.c), chip select on GPIO 0x12 driven by hand (docs/spec/hardware-dm32.md 3.2, 10). One mutex
// serializes every transaction once the scheduler runs. Everything below 0x700000 belongs to the stock firmware (10.1):
// reads only, writes and erases there are refused here, whatever the caller asks.
#include "dm32.h"
#include "pins.h"
#include "platform.h"
#include "flash/spi_nor/spi_nor.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define WRITE_FLOOR		0x700000u	// first byte this firmware may write (hardware-dm32.md 10.1)
#define SECURITY_REG_SIZE	256u

static SemaphoreHandle_t lock;
static bool ready;
static uint32_t jedec;

static void chip_select(bool active)
{
	hrc_ssi_select(SPI0_BASE, active);
	hrc_pin_write(PIN_FLASH_CS, !active);
}

static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	return hrc_ssi_xfer(SPI0_BASE, tx, rx, len);
}

static const spi_bus_t bus = { .select = chip_select, .xfer = xfer };
static spi_nor_t nor = { .bus = &bus };

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

static void spi_init(void)
{
	hrc_pin_out(PIN_FLASH_CS, true);
	hrc_ssi_init(SPI0_BASE, 4u);	// APB / 4 = 21 MHz (the stock-style 42 MHz is within the chip's rating; slower is safer)
	hal_delay_us(200);		// 160 us from power-up to the first command (10)
}

static bool init(void)
{
	if (lock == NULL) {
		lock = xSemaphoreCreateMutex();
	}
	spi_init();
	take();
	bool ok = spi_nor_probe(&nor) && nor.size != 0;
	give();
	jedec = (uint32_t)nor.manufacturer << 16 | nor.device;
	// Any vendor whose capacity code the driver knows: GD25Q128 C8 40 18, Puya 85 20 18 (the old port took only 0x4018)
	ready = ok;
	dm32_flash.size = ok ? nor.size : 0;
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
	take();
	bool ok = spi_nor_read(&nor, addr, buf, len);
	give();
	return ok;
}

static bool wr(uint32_t addr, const void *buf, uint32_t len)
{
	if (!in_range(addr, len) || addr < WRITE_FLOOR) {
		return false;
	}
	take();
	bool ok = spi_nor_program(&nor, addr, buf, len);
	give();
	return ok;
}

static bool er(uint32_t addr)
{
	if (!in_range(addr, 1) || addr < WRITE_FLOOR) {
		return false;
	}
	take();
	bool ok = spi_nor_erase_sector(&nor, addr);
	give();
	return ok;
}

// Everything the core stores lives above the write floor (hardware-dm32.md 10.1): the legacy-layout codeplug's 64 KB
// "EEPROM" image at 0x700000 (CODEPLUG_LEGACY_EEPROM_BASE in board.mk), OpenDM32's calibration copy left alone at
// 0x710000, the settings ring and the sector journal after it, the 1 MB legacy "flash" image at 0x720000
// (CODEPLUG_LEGACY_FLASH_BASE) with room above it for the DMR ID database, the native codeplug in the last 1 MB. The
// factory calibration is read only, so the legacy CPS gets no calibration window
static const hal_flash_map_t map = {
	.settings_addr = 0x711000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x719000u,	// 2 sectors
	.legacy_eeprom = 0x700000u,
	.legacy_flash = 0x720000u,
	.legacy_end = 0xF00000u,
	.calibration_addr = 0,
	.calibration_size = 0,
	.native_addr = 0xF00000u,
	.native_size = 0x100000u,
};
_Static_assert(0x700000u >= WRITE_FLOOR, "the core's areas start at the write floor");

hal_flash_t dm32_flash = {
	.init = init,
	.size = 0,
	.erase_size = SPI_NOR_SECTOR_SIZE,
	.read = rd,
	.write = wr,
	.erase = er,
	.map = &map,
};

uint32_t dm32_flash_jedec(void)
{
	return jedec;
}
