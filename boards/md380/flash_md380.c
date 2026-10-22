// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 data flash: a Winbond W25Q on SPI1 (PB3 SCK, PB4 MISO, PB5 MOSI, AF5), mode 0, APB2/16, chip select PD7
// (docs/spec/hardware-md380.md 12). Early radios are reported with 1 MB parts, later ones with 16 MB: the size is
// probed, and the core gets its areas only on a 16 MB part (the DM-1701 layout). On a smaller part nothing is stored
// (settings live in RAM) and the stock codeplug is left alone. One mutex serializes every transaction once the
// scheduler runs.
#include "md380.h"
#include "pins.h"
#include "flash/spi_nor/spi_nor.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define WINBOND_TYPE		0x40u		// JEDEC memory type of the W25Q serial flash family
#define FULL_SIZE		0x1000000u	// 16 MB: room for the open-firmware layout
#define SECURITY_REG_SIZE	256u

static SPI_HandleTypeDef spi;
static SemaphoreHandle_t lock;
static bool ready;

static void chip_select(bool active)
{
	PIN_WRITE(PIN_FLASH_CS, !active);
}

// Byte at a time on the registers (platform/stm32f4/spi.c)
static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	return stm32_spi_xfer(SPI1, tx, rx, len);
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
	stm32_pin_out(PIN_PORT(PIN_FLASH_CS), PIN_MASK(PIN_FLASH_CS), true, false);
	stm32_pin_af(GPIOB, GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5, GPIO_AF5_SPI1, false, GPIO_NOPULL);
	__HAL_RCC_SPI1_CLK_ENABLE();
	spi.Instance = SPI1;
	spi.Init.Mode = SPI_MODE_MASTER;
	spi.Init.Direction = SPI_DIRECTION_2LINES;
	spi.Init.DataSize = SPI_DATASIZE_8BIT;
	spi.Init.CLKPolarity = SPI_POLARITY_LOW;
	spi.Init.CLKPhase = SPI_PHASE_1EDGE;
	spi.Init.NSS = SPI_NSS_SOFT;
	spi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;	// APB2 72 MHz / 16 = 4.5 MHz
	spi.Init.FirstBit = SPI_FIRSTBIT_MSB;
	spi.Init.TIMode = SPI_TIMODE_DISABLE;
	spi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
	spi.Init.CRCPolynomial = 7;
	HAL_SPI_Init(&spi);
	__HAL_SPI_ENABLE(&spi);
}

// The DM-1701's layout (hardware-md380.md 12.2). The open-firmware codeplug stays where the CPS tools expect it; our
// own areas sit below the open-firmware flash image (0x011000..0x01FFFF, which no CPS address maps to) and above the 14
// MB mark that qdmr's DMR ID database may reach. The stock TYT codeplug at 0 is overwritten by the first open-firmware
// write
static const hal_flash_map_t map = {
	.settings_addr = 0x011000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x019000u,	// 2 sectors
	.openfw_eeprom = 0x000000u,
	.openfw_flash = 0x020000u,
	.openfw_end = 0xE00000u,
	.calibration_addr = 0,		// the calibration is in the security registers (calibration_md380.c)
	.calibration_size = 0,
	.native_addr = 0xE00000u,
	.native_size = 0x100000u,
	.staging_addr = 0xF00000u,	// 256 KB of the unused top megabyte (docs/spec/tftp.md 4)
	.staging_size = 0x040000u,
};

static bool init(void)
{
	if (lock == NULL) {
		lock = xSemaphoreCreateMutex();
	}
	spi_init();
	take();
	bool ok = spi_nor_probe(&nor) && (nor.device >> 8) == WINBOND_TYPE && nor.size != 0;
	give();
	ready = ok;
	md380_flash.size = ok ? nor.size : 0;
	md380_flash.map = ok && nor.size >= FULL_SIZE ? &map : NULL;
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
	if (!in_range(addr, len)) {
		return false;
	}
	take();
	bool ok = spi_nor_program(&nor, addr, buf, len);
	give();
	return ok;
}

static bool er(uint32_t addr)
{
	if (!in_range(addr, 1)) {
		return false;
	}
	take();
	bool ok = spi_nor_erase_sector(&nor, addr);
	give();
	return ok;
}


hal_flash_t md380_flash = {
	.init = init,
	.size = 0,
	.erase_size = SPI_NOR_SECTOR_SIZE,
	.read = rd,
	.write = wr,
	.erase = er,
	.map = NULL,	// set by init
};

bool md380_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len)
{
	if (!ready || reg < 1 || reg > 3 || offset >= SECURITY_REG_SIZE || len > SECURITY_REG_SIZE - offset) {
		return false;
	}
	take();
	bool ok = spi_nor_read_security(&nor, ((uint32_t)reg << 12) | offset, buf, len);
	give();
	return ok;
}

// The open-firmware CPS's area 10 (flash security registers), by the chip's register address (reg << 12 | offset)
bool md380_cps_security_read(uint32_t addr, void *buf, uint32_t len)
{
	return md380_flash_read_security((uint8_t)(addr >> 12), addr & 0xFFFu, buf, len);
}

uint16_t md380_flash_device(void)
{
	return ready ? nor.device : 0;
}
