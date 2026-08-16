// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 data flash: W25Q128 on SPI1 (PB3 SCK, PB4 MISO, PB5 MOSI, AF5), mode 0, APB2/16, chip select PD7
// (docs/spec/hardware-dm1701.md 13). One mutex serializes every transaction once the scheduler runs.
#include "dm1701.h"
#include "pins.h"
#include "flash/spi_nor/spi_nor.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define EXPECTED_DEVICE		0x4018u		// W25Q128: memory type 0x40, capacity 0x18
#define SECURITY_REG_SIZE	256u
#define XFER_SPINS		100000u		// per byte; a byte takes 1.8 us at 4.5 MHz

static SPI_HandleTypeDef spi;
static SemaphoreHandle_t lock;
static bool ready;

static void chip_select(bool active)
{
	PIN_WRITE(PIN_FLASH_CS, !active);
}

// Byte at a time on the registers: no HAL time base or DMA needed, and tx / rx may be NULL
static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	SPI_TypeDef *r = SPI1;
	for (uint32_t i = 0; i < len; i++) {
		uint32_t spins = XFER_SPINS;
		while ((r->SR & SPI_SR_TXE) == 0) {
			if (--spins == 0) {
				return false;
			}
		}
		*(volatile uint8_t *)&r->DR = tx != NULL ? tx[i] : 0xFF;
		spins = XFER_SPINS;
		while ((r->SR & SPI_SR_RXNE) == 0) {
			if (--spins == 0) {
				return false;
			}
		}
		uint8_t b = *(volatile uint8_t *)&r->DR;
		if (rx != NULL) {
			rx[i] = b;
		}
	}
	uint32_t spins = XFER_SPINS;
	while ((r->SR & SPI_SR_BSY) != 0) {
		if (--spins == 0) {
			return false;
		}
	}
	return true;
}

static const spi_nor_bus_t bus = { .select = chip_select, .xfer = xfer };
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

static bool init(void)
{
	if (lock == NULL) {
		lock = xSemaphoreCreateMutex();
	}
	spi_init();
	take();
	bool ok = spi_nor_probe(&nor) && nor.device == EXPECTED_DEVICE && nor.size != 0;
	give();
	ready = ok;
	dm1701_flash.size = ok ? nor.size : 0;
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

// docs/spec/hardware-dm1701.md 13.2. The legacy codeplug stays where the CPS tools expect it; our own areas sit in
// the gap between the calibration copy and the legacy flash image (0x011000..0x01FFFF, which no CPS address maps to)
// and above the 14 MB mark that qdmr's DMR ID database may reach
static const hal_flash_map_t map = {
	.settings_addr = 0x011000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x019000u,	// 2 sectors
	.legacy_eeprom = 0x000000u,
	.legacy_flash = 0x020000u,
	.legacy_end = 0xE00000u,
	.calibration_addr = 0x010000u,
	.calibration_size = 0x200u,
	.native_addr = 0xE00000u,
	.native_size = 0x100000u,
};

hal_flash_t dm1701_flash = {
	.init = init,
	.size = 0,
	.erase_size = SPI_NOR_SECTOR_SIZE,
	.read = rd,
	.write = wr,
	.erase = er,
	.map = &map,
};

bool dm1701_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len)
{
	if (!ready || reg < 1 || reg > 3 || offset >= SECURITY_REG_SIZE || len > SECURITY_REG_SIZE - offset) {
		return false;
	}
	take();
	bool ok = spi_nor_read_security(&nor, ((uint32_t)reg << 12) | offset, buf, len);
	give();
	return ok;
}

// The legacy CPS's area 10 (flash security registers), by the chip's register address (reg << 12 | offset)
bool dm1701_cps_security_read(uint32_t addr, void *buf, uint32_t len)
{
	return dm1701_flash_read_security((uint8_t)(addr >> 12), addr & 0xFFFu, buf, len);
}
