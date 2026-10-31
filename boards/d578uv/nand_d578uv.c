// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV SPI NAND: Kioxia TC58CVG1S3HRAIG-class (2 Gbit) or the GigaDevice part the stock firmware also accepts
// (manufacturer 0x98 or 0xC8), on GPIO pins the stock firmware bit-bangs as well (docs/spec/hardware-d578uv.md 6). The
// factory calibration is read from it; with CONFIG_NAND_STORAGE the core's storage lives in RadioFoundry's own block
// range (6.3, flash_d578uv.c), the only blocks the driver will program or erase. One recursive mutex serializes every
// NAND transaction once the scheduler runs: the storage layer holds it across a whole read-modify-program sequence,
// which uses the chip's page buffer.
#include "d578uv.h"
#include "config.h"
#include "pins.h"
#include "mcu_io.h"
#include "flash/spi_nand/spi_nand.h"
#include "hal_time.h"
#include "app/watchdog.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

static void chip_select(bool active)
{
	mcu_pin_write(PIN_NAND_CS, !active);
}

// Mode 0, MSB first: data out while the clock is low, sampled on the rising edge. GPIO stores at 120 MHz keep each
// half period well above the chip's 7.5 ns minimum
static uint8_t xfer_byte(uint8_t out)
{
	uint8_t in = 0;
	for (uint8_t m = 0x80; m != 0; m >>= 1) {
		mcu_pin_write(PIN_NAND_MOSI, (out & m) != 0);
		mcu_pin_write(PIN_NAND_CLK, true);
		if (mcu_pin_read(PIN_NAND_MISO)) {
			in |= m;
		}
		mcu_pin_write(PIN_NAND_CLK, false);
	}
	return in;
}

static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		uint8_t v = xfer_byte(tx != NULL ? tx[i] : 0xFF);
		if (rx != NULL) {
			rx[i] = v;
		}
	}
	return true;
}

static const spi_bus_t bus = { .select = chip_select, .xfer = xfer };
// 2 KB pages with 64 spare bytes (on-die ECC on, its default), 64 pages per block, 2048 blocks
static spi_nand_t nand = {
	.bus = &bus,
	.page_size = 2048,
	.spare_size = 64,
	.pages_per_block = 64,
	.blocks = 2048,
#if CONFIG_NAND_STORAGE
	// Feature 0xB0: ECC on (both parts' default; the stock firmware never writes it), Kioxia's high speed read off
	// (the internal data move needs it off). Every block locked (0xA0 = 0x38) except during a program or erase.
	// Writes only in RadioFoundry's range
	.config = 0x10,
	.lock_all = 0x38,
	.write_first = D578UV_NAND_RFOUNDRY_FIRST,
	.write_blocks = D578UV_NAND_RFOUNDRY_BLOCKS,
#endif
};
static bool ready;
static SemaphoreHandle_t lock;

void d578uv_nand_lock(void)
{
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreTakeRecursive(lock, portMAX_DELAY);
	}
}

void d578uv_nand_unlock(void)
{
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreGiveRecursive(lock);
	}
}

bool d578uv_nand_init(void)
{
	if (lock == NULL) {
		lock = xSemaphoreCreateRecursiveMutex();
	}
	mcu_pin_out(PIN_NAND_CS, true, false);
	mcu_pin_out(PIN_NAND_CLK, false, false);
	mcu_pin_out(PIN_NAND_MOSI, false, false);
	mcu_pin_in(PIN_NAND_MISO, MCU_PULL_UP);
	d578uv_nand_lock();
	ready = spi_nand_probe(&nand);
	d578uv_nand_unlock();
	return ready;
}

bool d578uv_nand_ready(void)
{
	return ready;
}

uint16_t d578uv_nand_id(void)
{
	return (uint16_t)(nand.manufacturer << 8 | nand.device);
}

bool d578uv_nand_read(uint32_t addr, void *buf, uint32_t len)
{
	d578uv_nand_lock();
	bool ok = ready && spi_nand_read(&nand, addr, buf, len);
	d578uv_nand_unlock();
	return ok;
}

#if CONFIG_NAND_STORAGE
// RadioFoundry's range as the sector layer sees it (nand_dev_t): blocks and pages from D578UV_NAND_RFOUNDRY_FIRST. The
// driver's write window refuses anything outside it as well
#define PPB		64u
#define PAGES		(D578UV_NAND_RFOUNDRY_BLOCKS * PPB)
#define FIRST_PAGE	((uint32_t)D578UV_NAND_RFOUNDRY_FIRST * PPB)
#define MAX_CHUNKS	2u

static bool dev_read(void *ctx, uint32_t page, uint16_t column, void *buf, uint32_t len)
{
	(void)ctx;
	d578uv_nand_lock();
	bool ok = ready && page < PAGES && spi_nand_read_page(&nand, FIRST_PAGE + page, column, buf, len);
	d578uv_nand_unlock();
	return ok;
}

static nand_result_t result(spi_nand_result_t r)
{
	return r == SPI_NAND_DONE ? NAND_OK : r == SPI_NAND_FAILED ? NAND_FAILED : NAND_ERROR;
}

static nand_result_t dev_program(void *ctx, uint32_t page, uint32_t src, const nand_chunk_t *chunks, unsigned n)
{
	(void)ctx;
	spi_nand_chunk_t c[MAX_CHUNKS];
	if (!ready || page >= PAGES || (src != NAND_SECTORS_NO_PAGE && src >= PAGES) || n > MAX_CHUNKS) {
		return NAND_ERROR;
	}
	for (unsigned i = 0; i < n; i++) {
		c[i].column = chunks[i].column;
		c[i].len = chunks[i].len;
		c[i].data = chunks[i].data;
	}
	d578uv_nand_lock();
	spi_nand_result_t r = spi_nand_program(&nand, FIRST_PAGE + page,
					       src == NAND_SECTORS_NO_PAGE ? SPI_NAND_NO_PAGE : FIRST_PAGE + src, c, n);
	d578uv_nand_unlock();
	return result(r);
}

static nand_result_t dev_erase(void *ctx, uint16_t block)
{
	(void)ctx;
	if (!ready || block >= D578UV_NAND_RFOUNDRY_BLOCKS) {
		return NAND_ERROR;
	}
	d578uv_nand_lock();
	spi_nand_result_t r = spi_nand_erase(&nand, D578UV_NAND_RFOUNDRY_FIRST + block);
	d578uv_nand_unlock();
	return result(r);
}

static bool dev_block_bad(void *ctx, uint16_t block, bool *bad)
{
	(void)ctx;
	d578uv_nand_lock();
	bool ok = ready && block < D578UV_NAND_RFOUNDRY_BLOCKS && spi_nand_block_bad(&nand, D578UV_NAND_RFOUNDRY_FIRST + block, bad);
	d578uv_nand_unlock();
	return ok;
}

static void dev_mark_bad(void *ctx, uint16_t block)
{
	(void)ctx;
	if (ready && block < D578UV_NAND_RFOUNDRY_BLOCKS) {
		d578uv_nand_lock();
		spi_nand_mark_bad(&nand, D578UV_NAND_RFOUNDRY_FIRST + block);
		d578uv_nand_unlock();
	}
}

// The sector layer's long loops keep the CPU with busy waits: the watchdog is fed per block or sector, and every
// DEV_BUSY_YIELD_MS of such work gives the other tasks one tick (docs/spec/watchdog.md 4)
#define DEV_BUSY_YIELD_MS	20u

static void dev_busy(void *ctx)
{
	static uint32_t last_yield;
	(void)ctx;
	uint32_t now = hal_millis();
#if CONFIG_WATCHDOG
	watchdog_kick(now);
#endif
	if (now - last_yield >= DEV_BUSY_YIELD_MS && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		vTaskDelay(1);
		last_yield = hal_millis();
	}
}

const nand_dev_t d578uv_nand_dev = {
	.ctx = NULL,
	.pages_per_block = PPB,
	.blocks = D578UV_NAND_RFOUNDRY_BLOCKS,
	.spare_size = 64,
	.read = dev_read,
	.program = dev_program,
	.erase = dev_erase,
	.block_bad = dev_block_bad,
	.mark_bad = dev_mark_bad,
	.busy = dev_busy,
};

#endif
