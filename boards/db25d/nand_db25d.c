// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D PRO SPI NAND: Kioxia TC58CVG1S3HRAIJ (2 Gbit) on SPI0, chip select PTA18 by GPIO, set up as the stock firmware
// does (docs/spec/hardware-db25d.md 3.2, 6.5). The factory calibration copy is read at boot; with CONFIG_NAND_STORAGE
// the core's storage lives in ODR's own block range (6.6, flash_db25d.c), the only blocks the driver will program or
// erase. One recursive mutex serializes every NAND transaction once the scheduler runs: the storage layer holds it
// across a whole read-modify-program sequence, which uses the chip's page buffer.
#include "db25d.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "flash/spi_nand/spi_nand.h"
#include "hal_time.h"
#include "app/watchdog.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define DIPLEX1_SPI0		(0x3Fu << 3)	// SPI0 SCLK / MOSI / MISO to the SPI master (0 = peripheral)
#define SPI0_BAUDR		4u		// APB / 4: 21 MHz at ODR's 84 MHz, 9 MHz at the loader's 36 (the chip takes 133)

static void chip_select(bool active)
{
	hrc_ssi_select(SPI0_BASE, active);
	hrc_pin_write(PIN_NAND_CS, !active);
}

static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	return hrc_ssi_xfer(SPI0_BASE, tx, rx, len);
}

static const spi_bus_t bus = { .select = chip_select, .xfer = xfer };
// 2 KB pages with 64 spare bytes while the on-die ECC is on (its default), 64 pages per block, 2048 blocks
static spi_nand_t nand = {
	.bus = &bus,
	.page_size = 2048,
	.spare_size = 64,
	.pages_per_block = 64,
	.blocks = 2048,
#if CONFIG_NAND_STORAGE
	// Feature 0xB0: ECC on, high speed read off (the internal data move needs it off, and random page reads are
	// faster without it), block protection commands off. Every block locked (0xA0 = 0x38) except during a program
	// or erase. Writes only in ODR's range
	.config = 0x10,
	.lock_all = 0x38,
	.write_first = DB25D_NAND_ODR_FIRST,
	.write_blocks = DB25D_NAND_ODR_BLOCKS,
#endif
};
static bool ready;
static SemaphoreHandle_t lock;

void db25d_nand_lock(void)
{
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreTakeRecursive(lock, portMAX_DELAY);
	}
}

void db25d_nand_unlock(void)
{
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreGiveRecursive(lock);
	}
}

bool db25d_nand_init(void)
{
	if (lock == NULL) {
		lock = xSemaphoreCreateRecursiveMutex();
	}
	hrc_pin_out(PIN_NAND_CS, true);
	IO_DIPLEX1 &= ~DIPLEX1_SPI0;
	hrc_ssi_init(SPI0_BASE, SPI0_BAUDR);
	// The IAP loader has run long past the chip's power-up; the probe resets it and waits until it's ready
	db25d_nand_lock();
	ready = spi_nand_probe(&nand);
	uint8_t id[SPI_NAND_UID_LEN];
	if (ready && spi_nand_read_uid(&nand, id)) {
		hrc_chip_set_uid(id, sizeof(id));	// the radio's UID (INFO_CHIP): the SoC has none we can read
	}
	db25d_nand_unlock();
	return ready;
}

bool db25d_nand_ready(void)
{
	return ready;
}

uint16_t db25d_nand_id(void)
{
	return (uint16_t)(nand.manufacturer << 8 | nand.device);
}

uint32_t db25d_nand_corrected(void)
{
	return nand.corrected;
}

bool db25d_nand_read(uint32_t addr, void *buf, uint32_t len)
{
	db25d_nand_lock();
	bool ok = ready && spi_nand_read(&nand, addr, buf, len);
	db25d_nand_unlock();
	return ok;
}

#if CONFIG_NAND_STORAGE
// ODR's range as the sector layer sees it (nand_dev_t): blocks and pages from DB25D_NAND_ODR_FIRST. The driver's
// write window refuses anything outside it as well
#define PPB		64u
#define PAGES		(DB25D_NAND_ODR_BLOCKS * PPB)
#define FIRST_PAGE	((uint32_t)DB25D_NAND_ODR_FIRST * PPB)
#define MAX_CHUNKS	2u

static bool dev_read(void *ctx, uint32_t page, uint16_t column, void *buf, uint32_t len)
{
	(void)ctx;
	db25d_nand_lock();
	bool ok = ready && page < PAGES && spi_nand_read_page(&nand, FIRST_PAGE + page, column, buf, len);
	db25d_nand_unlock();
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
	db25d_nand_lock();
	spi_nand_result_t r = spi_nand_program(&nand, FIRST_PAGE + page,
					       src == NAND_SECTORS_NO_PAGE ? SPI_NAND_NO_PAGE : FIRST_PAGE + src, c, n);
	db25d_nand_unlock();
	return result(r);
}

static nand_result_t dev_erase(void *ctx, uint16_t block)
{
	(void)ctx;
	if (!ready || block >= DB25D_NAND_ODR_BLOCKS) {
		return NAND_ERROR;
	}
	db25d_nand_lock();
	spi_nand_result_t r = spi_nand_erase(&nand, DB25D_NAND_ODR_FIRST + block);
	db25d_nand_unlock();
	return result(r);
}

static bool dev_block_bad(void *ctx, uint16_t block, bool *bad)
{
	(void)ctx;
	db25d_nand_lock();
	bool ok = ready && block < DB25D_NAND_ODR_BLOCKS && spi_nand_block_bad(&nand, DB25D_NAND_ODR_FIRST + block, bad);
	db25d_nand_unlock();
	return ok;
}

static void dev_mark_bad(void *ctx, uint16_t block)
{
	(void)ctx;
	if (ready && block < DB25D_NAND_ODR_BLOCKS) {
		db25d_nand_lock();
		spi_nand_mark_bad(&nand, DB25D_NAND_ODR_FIRST + block);
		db25d_nand_unlock();
	}
}

// The sector layer's long loops (the mount at start-up reads every page in use; garbage collection) keep the CPU with
// busy waits: the watchdog is fed per block or sector there instead of by the idle task, and the hang check counts
// from that feed (watchdog_kick)
static void dev_busy(void *ctx)
{
	(void)ctx;
#if CONFIG_WATCHDOG
	watchdog_kick(hal_millis());
#endif
}

const nand_dev_t db25d_nand_dev = {
	.ctx = NULL,
	.pages_per_block = PPB,
	.blocks = DB25D_NAND_ODR_BLOCKS,
	.spare_size = 64,
	.read = dev_read,
	.program = dev_program,
	.erase = dev_erase,
	.block_bad = dev_block_bad,
	.mark_bad = dev_mark_bad,
	.busy = dev_busy,
};

uint32_t db25d_nand_program_fails(void)
{
	return nand.program_fails;
}
#endif
