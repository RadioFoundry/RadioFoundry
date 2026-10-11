// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D PRO SPI NAND: Kioxia TC58CVG1S3HRAIJ (2 Gbit) on SPI0, chip select PTA18 by GPIO, set up as the stock firmware
// does (docs/spec/hardware-db25d.md 3.2, 6.5). Read only: the factory calibration copy is the only reader so far, at
// boot before the scheduler starts, so there is no lock yet. Storage on it needs one, and a sector layer.
#include "db25d.h"
#include "pins.h"
#include "platform.h"
#include "flash/spi_nand/spi_nand.h"

#define DIPLEX1_SPI0		(0x3Fu << 3)	// SPI0 SCLK / MOSI / MISO to the SPI master (0 = peripheral)
#define SPI0_BAUDR		4u		// APB / 4 = 21 MHz, as the stock firmware

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
};
static bool ready;

bool db25d_nand_init(void)
{
	hrc_pin_out(PIN_NAND_CS, true);
	IO_DIPLEX1 &= ~DIPLEX1_SPI0;
	hrc_ssi_init(SPI0_BASE, SPI0_BAUDR);
	// The IAP loader has run long past the chip's power-up; the probe resets it and waits until it's ready
	ready = spi_nand_probe(&nand);
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
	return ready && spi_nand_read(&nand, addr, buf, len);
}
