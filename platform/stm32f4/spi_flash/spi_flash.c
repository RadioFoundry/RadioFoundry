// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The data flash of an STM32F4 radio (spi_flash.h)
#include "spi_flash_board.h"
#include "flash/spi_nor/spi_nor.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define SECURITY_REG_SIZE	256u

static const stm32_spi_flash_cfg_t *cfg;
static SemaphoreHandle_t lock;
static bool ready;

static void chip_select(bool active)
{
	stm32_pin_write(cfg->cs_port, cfg->cs_pin, !active);
}

// Byte at a time on the registers (platform/stm32f4/spi.c)
static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	return stm32_spi_xfer(cfg->spi, tx, rx, len);
}

static const spi_bus_t bus = { .select = chip_select, .xfer = xfer };
static spi_nor_t nor = { .bus = &bus };

static void take(void)
{
	if (cfg->bus_take != NULL) {
		cfg->bus_take();
	} else if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreTake(lock, portMAX_DELAY);
	}
}

static void give(void)
{
	if (cfg->bus_give != NULL) {
		cfg->bus_give();
	} else if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreGive(lock);
	}
}

static bool init(void)
{
	if (cfg == NULL) {
		return false;
	}
	if (lock == NULL && cfg->bus_take == NULL) {
		lock = xSemaphoreCreateMutex();
	}
	stm32_pin_out(cfg->cs_port, cfg->cs_pin, true, false);
	stm32_pin_af(cfg->bus_port, cfg->bus_pins, cfg->af, false, GPIO_NOPULL);
	stm32_spi_master_init(cfg->spi, cfg->cpol, cfg->cpha, cfg->br);
	take();
	bool ok = spi_nor_probe(&nor) && (nor.device & cfg->device_mask) == cfg->device && nor.size != 0;
	give();
	ready = ok;
	stm32_spi_flash.size = ok ? nor.size : 0;
	stm32_spi_flash.map = ok && nor.size >= cfg->map_min_size ? cfg->map : NULL;
	return ok;
}

bool stm32_spi_flash_init(const stm32_spi_flash_cfg_t *c)
{
	cfg = c;
	return init();
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

hal_flash_t stm32_spi_flash = {
	.init = init,
	.size = 0,
	.erase_size = SPI_NOR_SECTOR_SIZE,
	.read = rd,
	.write = wr,
	.erase = er,
	.map = NULL,	// set by init
};

bool stm32_spi_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len)
{
	if (!ready || reg < 1 || reg > 3 || offset >= SECURITY_REG_SIZE || len > SECURITY_REG_SIZE - offset) {
		return false;
	}
	take();
	bool ok = spi_nor_read_security(&nor, ((uint32_t)reg << 12) | offset, buf, len);
	give();
	return ok;
}

bool stm32_spi_flash_cps_security_read(uint32_t addr, void *buf, uint32_t len)
{
	return stm32_spi_flash_read_security((uint8_t)(addr >> 12), addr & 0xFFFu, buf, len);
}

uint16_t stm32_spi_flash_device(void)
{
	return ready ? nor.device : 0;
}
