// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Data flash on SPI0 (a 16 MB SPI NOR, GD25Q128 or Puya PY25Q128HA class), chip select on a GPIO the board names and
// drives by hand (docs/spec/hardware-dm32.md 3.2, 10; hardware-dm1801pro.md 6.1). One mutex serializes every
// transaction once the scheduler runs. Everything below the board's write floor belongs to the radio's own firmware:
// reads only, writes and erases there are refused here, whatever the caller asks.
#include "platform.h"
#include "flash/spi_nor/spi_nor.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

static const hrc_flash_cfg_t *cfg;
static SemaphoreHandle_t lock;
static bool ready;
static uint32_t jedec;
static TaskHandle_t holder;		// who has the lock, for the diagnostics page
static uint32_t held_since;

static void chip_select(bool active)
{
	hrc_ssi_select(SPI0_BASE, active);
	hrc_pin_write(cfg->cs_pin, !active);
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
		held_since = hal_millis();
		holder = xTaskGetCurrentTaskHandle();
	}
}

static void give(void)
{
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		holder = NULL;
		xSemaphoreGive(lock);
	}
}

static bool init(void)
{
	if (cfg == NULL) {
		return false;
	}
	if (lock == NULL) {
		lock = xSemaphoreCreateMutex();
	}
	hrc_pin_out(cfg->cs_pin, true);
	hrc_ssi_init(SPI0_BASE, 4u);	// APB / 4 = 21 MHz (42 MHz is within the chip's rating; slower is safer)
	hal_delay_us(200);		// 160 us from power-up to the first command
	take();
	bool ok = spi_nor_probe(&nor) && nor.size != 0;
	uint8_t id[SPI_NOR_UID_LEN];
	if (ok && spi_nor_read_uid(&nor, id)) {
		hrc_chip_set_uid(id, sizeof(id));
	}
	give();
	jedec = (uint32_t)nor.manufacturer << 16 | nor.device;
	// Any vendor whose capacity code the driver knows: GD25Q128 C8 40 18, Puya 85 20 18
	ready = ok;
	hrc_flash_spi0.size = ok ? nor.size : 0;
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
	if (!in_range(addr, len) || addr < cfg->write_floor) {
		return false;
	}
	take();
	bool ok = spi_nor_program(&nor, addr, buf, len);
	give();
	return ok;
}

static bool er(uint32_t addr)
{
	if (!in_range(addr, 1) || addr < cfg->write_floor) {
		return false;
	}
	take();
	bool ok = spi_nor_erase_sector(&nor, addr);
	give();
	return ok;
}

hal_flash_t hrc_flash_spi0 = {
	.init = init,
	.size = 0,
	.erase_size = SPI_NOR_SECTOR_SIZE,
	.read = rd,
	.write = wr,
	.erase = er,
};

void hrc_flash_spi0_configure(const hrc_flash_cfg_t *c)
{
	cfg = c;
	hrc_flash_spi0.map = c->map;
}

uint32_t hrc_flash_spi0_jedec(void)
{
	return jedec;
}

const char *hrc_flash_spi0_holder(uint32_t *held_ms)
{
	TaskHandle_t h = holder;
	*held_ms = h != NULL ? hal_millis() - held_since : 0;
	return h != NULL ? pcTaskGetName(h) : "-";
}
