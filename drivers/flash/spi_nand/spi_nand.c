// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Generic SPI NAND flash, read side (docs/spec/hardware-db25d.md 6.5: the Kioxia TC58CVG1S3HRAIJ data sheet and the
// sequence the DB25-D stock firmware uses on it)
#include <stddef.h>
#include "spi_nand.h"
#include "hal_time.h"

enum {
	CMD_RESET		= 0xFF,
	CMD_READ_ID		= 0x9F,
	CMD_GET_FEATURE		= 0x0F,
	CMD_PAGE_READ		= 0x13,
	CMD_READ_CACHE		= 0x03,
};

// Busy limits in POLL_US polls, with room for slower parts (Kioxia maximums: page read 180 us, reset 550 us)
#define POLL_US			10u
#define PAGE_READ_POLLS		200u
#define RESET_POLLS		500u

// One command: a header clocked out, then len bytes read in the same chip select
static bool command(const spi_nand_t *nand, const uint8_t *hdr, uint32_t hdr_len, uint8_t *rx, uint32_t len)
{
	const spi_bus_t *bus = nand->bus;
	bus->select(true);
	bool ok = bus->xfer(hdr, NULL, hdr_len);
	if (ok && len > 0) {
		ok = bus->xfer(NULL, rx, len);
	}
	bus->select(false);
	return ok;
}

bool spi_nand_get_feature(spi_nand_t *nand, uint8_t addr, uint8_t *out)
{
	const uint8_t hdr[2] = { CMD_GET_FEATURE, addr };
	return command(nand, hdr, sizeof(hdr), out, 1);
}

// Poll until the operation in progress ends; the last status in *status
static bool wait_ready(spi_nand_t *nand, uint32_t polls, uint8_t *status)
{
	for (uint32_t i = 0; i <= polls; i++) {
		if (!spi_nand_get_feature(nand, SPI_NAND_FEATURE_STATUS, status)) {
			return false;
		}
		if ((*status & SPI_NAND_STATUS_OIP) == 0) {
			return true;
		}
		hal_delay_us(POLL_US);
	}
	return false;
}

bool spi_nand_probe(spi_nand_t *nand)
{
	static const uint8_t reset = CMD_RESET;
	static const uint8_t read_id[2] = { CMD_READ_ID, 0x00 };
	uint8_t id[2], status;
	nand->manufacturer = 0;
	nand->device = 0;
	nand->cached = SPI_NAND_NO_PAGE;
	nand->ecc = SPI_NAND_ECC_OK;
	nand->corrected = 0;
	if (!command(nand, &reset, 1, NULL, 0) || !wait_ready(nand, RESET_POLLS, &status) ||
	    !command(nand, read_id, sizeof(read_id), id, sizeof(id))) {
		return false;
	}
	if ((id[0] == 0x00 && id[1] == 0x00) || (id[0] == 0xFF && id[1] == 0xFF)) {
		return false;	// bus stuck low or nothing driving MISO
	}
	nand->manufacturer = id[0];
	nand->device = id[1];
	return true;
}

uint32_t spi_nand_size(const spi_nand_t *nand)
{
	return (uint32_t)nand->page_size * nand->pages_per_block * nand->blocks;
}

// Bring page into the chip's cache unless it's there already. With any_ecc a page ECC couldn't correct still counts as
// loaded (its raw data is in the cache), but isn't remembered as cached
static bool load(spi_nand_t *nand, uint32_t page, bool any_ecc)
{
	if (page == nand->cached) {
		return true;
	}
	const uint8_t hdr[4] = { CMD_PAGE_READ, (uint8_t)(page >> 16), (uint8_t)(page >> 8), (uint8_t)page };
	uint8_t status;
	nand->cached = SPI_NAND_NO_PAGE;
	nand->ecc = SPI_NAND_ECC_OK;
	if (!command(nand, hdr, sizeof(hdr), NULL, 0) || !wait_ready(nand, PAGE_READ_POLLS, &status)) {
		return false;
	}
	nand->ecc = (uint8_t)((status >> SPI_NAND_STATUS_ECC_SHIFT) & 3u);
	if (nand->ecc == SPI_NAND_ECC_FAILED) {
		return any_ecc;	// left uncached, so a retry reads the array again
	}
	if (nand->ecc != SPI_NAND_ECC_OK) {
		nand->corrected++;
	}
	nand->cached = page;
	return true;
}

static bool read_cache(const spi_nand_t *nand, uint16_t column, void *buf, uint32_t len)
{
	const uint8_t hdr[4] = { CMD_READ_CACHE, (uint8_t)(column >> 8), (uint8_t)column, 0x00 };	// dummy byte
	return command(nand, hdr, sizeof(hdr), buf, len);
}

bool spi_nand_read_page(spi_nand_t *nand, uint32_t page, uint16_t column, void *buf, uint32_t len)
{
	uint32_t total = (uint32_t)nand->page_size + nand->spare_size;
	if (page >= (uint32_t)nand->pages_per_block * nand->blocks || column > total || len > total - column) {
		return false;
	}
	if (len == 0) {
		return true;
	}
	return load(nand, page, false) && read_cache(nand, column, buf, len);
}

bool spi_nand_read(spi_nand_t *nand, uint32_t addr, void *buf, uint32_t len)
{
	uint8_t *p = buf;
	if (nand->page_size == 0 || addr > spi_nand_size(nand) || len > spi_nand_size(nand) - addr) {
		return false;
	}
	while (len > 0) {
		uint32_t page = addr / nand->page_size;
		uint16_t column = (uint16_t)(addr % nand->page_size);
		uint32_t n = nand->page_size - column;
		if (n > len) {
			n = len;
		}
		if (!spi_nand_read_page(nand, page, column, p, n)) {
			return false;
		}
		addr += n;
		p += n;
		len -= n;
	}
	return true;
}

bool spi_nand_block_bad(spi_nand_t *nand, uint32_t block, bool *bad)
{
	// The mark is judged from the data whatever the ECC status says (Kioxia's bad-block test flow): a marked page can
	// fail ECC
	uint8_t mark;
	if (block >= nand->blocks || !load(nand, block * nand->pages_per_block, true) ||
	    !read_cache(nand, nand->page_size, &mark, 1)) {
		return false;
	}
	*bad = mark != 0xFFu;
	return true;
}
