// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Generic SPI NAND flash with on-die ECC (the common command set: FF reset, 9F ID, 0F / 1F get / set feature, 13 page
// read to the chip's cache, 03 read from cache, 06 write enable, 02 / 84 program load, 10 program execute, D8 block
// erase). The board supplies the bus, any locking and the geometry from the chip's data sheet; the driver builds the
// command sequences, checks ECC and keeps track of the page in the cache. Program and erase reach only the blocks of
// the board's write window. Single-plane parts: a two-plane part would need the plane bit in the column address.
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

#define SPI_NAND_MFR_GIGADEVICE		0xC8u	// its ECC status is 3 bits and its read from cache takes a 3-byte address

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
	// Set up by the board. config: written to feature 0xB0 at the probe (0 leaves the chip's own setting). lock_all:
	// the feature 0xA0 value that locks every block; it is put back after each program and erase, so a stray command
	// can't change the array in between (0: the blocks stay unlocked)
	uint8_t config;
	uint8_t lock_all;
	// The only blocks program and erase may touch: write_blocks blocks from write_first (0: the chip is read only)
	uint16_t write_first;
	uint16_t write_blocks;
	// Filled in by spi_nand_probe and the reads
	uint8_t manufacturer;
	uint8_t device;
	uint8_t ecc;			// SPI_NAND_ECC_* of the last page read
	uint32_t cached;		// the page in the chip's cache, SPI_NAND_NO_PAGE if none
	uint32_t corrected;		// pages read with corrected bit errors since the probe
	uint32_t program_fails;		// programs and erases the chip reported failed
} spi_nand_t;

// What a program or erase came to
typedef enum {
	SPI_NAND_DONE,
	SPI_NAND_FAILED,		// the chip reported a program or erase failure (status bit 3 or 2): the block is worn
	SPI_NAND_ERROR,			// refused (outside the write window, bad arguments), the bus failed, the chip stayed
					// busy, or the source page of a move couldn't be read
} spi_nand_result_t;

// Bytes for the chip's buffer at a column of the page (main area from 0, spare from page_size)
typedef struct {
	uint16_t column;
	uint16_t len;
	const void *data;
} spi_nand_chunk_t;

// Reset the chip and read its ID (9F and a dummy byte, then manufacturer and device). False if the bus fails, the chip
// stays busy or nothing answers
bool spi_nand_probe(spi_nand_t *nand);
// Main-area bytes: page_size x pages_per_block x blocks
uint32_t spi_nand_size(const spi_nand_t *nand);
// The chip's 16-byte unique ID (the ID page: Kioxia; other vendors differ). False if no copy checks out or the bus fails
#define SPI_NAND_UID_LEN		16u
bool spi_nand_read_uid(spi_nand_t *nand, uint8_t id[SPI_NAND_UID_LEN]);
bool spi_nand_get_feature(spi_nand_t *nand, uint8_t addr, uint8_t *out);
bool spi_nand_set_feature(spi_nand_t *nand, uint8_t addr, uint8_t value);
// len bytes of one page from column (0 .. page_size + spare_size). False if the bus fails, the chip stays busy or ECC
// can't correct the page
bool spi_nand_read_page(spi_nand_t *nand, uint32_t page, uint16_t column, void *buf, uint32_t len);
// The main areas as one linear space (addr = page x page_size + column); spare bytes are skipped
bool spi_nand_read(spi_nand_t *nand, uint32_t addr, void *buf, uint32_t len);
// The factory bad-block mark: the first spare byte of the block's first page is not 0xFF, read whatever the page's ECC
// status (a marked page is all 0x00 on the Kioxia parts and can fail ECC)
bool spi_nand_block_bad(spi_nand_t *nand, uint32_t block, bool *bad);

// Program one page, once (no partial programs: with on-die ECC each 512 + 16 byte ECC unit takes one program). src
// SPI_NAND_NO_PAGE: the chip's buffer starts all 0xFF (program load 02, then 84 for the further chunks). Otherwise an
// internal data move: page src is read into the buffer (13; its ECC must pass), the chunks change it (84), and it
// is programmed to page with fresh ECC, the data never leaving the chip. Pages of a block must be programmed in
// ascending order. Chunks must lie inside the page and its spare area
spi_nand_result_t spi_nand_program(spi_nand_t *nand, uint32_t page, uint32_t src, const spi_nand_chunk_t *chunks,
				   unsigned n);
// Erase one block (every page to 0xFF)
spi_nand_result_t spi_nand_erase(spi_nand_t *nand, uint32_t block);
// Retire a worn block: erase it (whatever the result) and program its first page to 0x00 throughout, which
// spi_nand_block_bad (and the stock firmware's own scan) then reports as bad
spi_nand_result_t spi_nand_mark_bad(spi_nand_t *nand, uint32_t block);

#endif
