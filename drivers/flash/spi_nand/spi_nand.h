// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Generic SPI NAND flash with on-die ECC (the common command set: FF reset, 9F ID, 0F get feature, 13 page read to the
// chip's cache, 03 read from cache). Read side only so far. The board supplies the bus, any locking and the geometry
// from the chip's data sheet; the driver builds the command sequences, checks ECC and keeps track of the page in the
// cache. Single-plane parts: a two-plane part would need the plane bit in the column address.
#ifndef SPI_NAND_H
#define SPI_NAND_H

#include <stdint.h>
#include <stdbool.h>
#include "bus/spi_bus.h"

#define SPI_NAND_FEATURE_LOCK		0xA0u	// block protection
#define SPI_NAND_FEATURE_CONFIG		0xB0u	// ECC enable, OTP access
#define SPI_NAND_FEATURE_STATUS		0xC0u
#define SPI_NAND_STATUS_OIP		0x01u	// operation in progress
#define SPI_NAND_STATUS_WEL		0x02u
#define SPI_NAND_STATUS_E_FAIL		0x04u
#define SPI_NAND_STATUS_P_FAIL		0x08u
#define SPI_NAND_STATUS_ECC_SHIFT	4u	// 2 bits: SPI_NAND_ECC_*

// ECC result of a page read. 3: corrected, at or above the chip's bit-flip threshold (Kioxia; feature 0x10)
enum { SPI_NAND_ECC_OK, SPI_NAND_ECC_CORRECTED, SPI_NAND_ECC_FAILED, SPI_NAND_ECC_CORRECTED_MANY };

#define SPI_NAND_NO_PAGE		0xFFFFFFFFu

typedef struct {
	const spi_bus_t *bus;
	// Geometry, from the board
	uint16_t page_size;		// main area bytes
	uint16_t spare_size;		// spare bytes with ECC on (what reads past page_size reach)
	uint16_t pages_per_block;
	uint16_t blocks;
	// Filled in by spi_nand_probe and the reads
	uint8_t manufacturer;
	uint8_t device;
	uint8_t ecc;			// SPI_NAND_ECC_* of the last page read
	uint32_t cached;		// the page in the chip's cache, SPI_NAND_NO_PAGE if none
	uint32_t corrected;		// pages read with corrected bit errors since the probe
} spi_nand_t;

// Reset the chip and read its ID (9F and a dummy byte, then manufacturer and device). False if the bus fails, the chip
// stays busy or nothing answers
bool spi_nand_probe(spi_nand_t *nand);
// Main-area bytes: page_size x pages_per_block x blocks
uint32_t spi_nand_size(const spi_nand_t *nand);
bool spi_nand_get_feature(spi_nand_t *nand, uint8_t addr, uint8_t *out);
// len bytes of one page from column (0 .. page_size + spare_size). False if the bus fails, the chip stays busy or ECC
// can't correct the page
bool spi_nand_read_page(spi_nand_t *nand, uint32_t page, uint16_t column, void *buf, uint32_t len);
// The main areas as one linear space (addr = page x page_size + column); spare bytes are skipped
bool spi_nand_read(spi_nand_t *nand, uint32_t addr, void *buf, uint32_t len);
// The factory bad-block mark: the first spare byte of the block's first page is not 0xFF, read whatever the page's ECC
// status (a marked page is all 0x00 on the Kioxia parts and can fail ECC)
bool spi_nand_block_bad(spi_nand_t *nand, uint32_t block, bool *bad);

#endif
