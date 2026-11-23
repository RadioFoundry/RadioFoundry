// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Generic SPI NOR flash (W25Q / GD25Q class, 3-byte addresses). The board supplies the bus (chip select and a
// full-duplex transfer) and any locking; the driver only builds the command sequences.
#ifndef SPI_NOR_H
#define SPI_NOR_H

#include <stdint.h>
#include <stdbool.h>
#include "bus/spi_bus.h"

typedef struct {
	const spi_bus_t *bus;
	uint8_t manufacturer;	// JEDEC manufacturer ID (0xEF Winbond, 0xC8 GigaDevice, ...)
	uint16_t device;	// memory type << 8 | capacity code
	uint32_t size;		// bytes, from the capacity code (0 if unknown)
	bool fast_read;		// read with 0x0B (one dummy byte) instead of 0x03; set by the board before use
} spi_nor_t;

#define SPI_NOR_PAGE_SIZE	256u
#define SPI_NOR_SECTOR_SIZE	4096u
#define SPI_NOR_BLOCK_SIZE	65536u
#define SPI_NOR_MAX_SIZE	0x1000000u	// what 3-byte addresses reach: a larger part is used as 16 MB

// Read the JEDEC ID (0x9F) and fill in manufacturer, device and size (at most SPI_NOR_MAX_SIZE). False if the bus fails
// or nothing answers
bool spi_nor_probe(spi_nor_t *nor);
bool spi_nor_read(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len);
// Program already erased bytes; split at page boundaries, waits for each page
bool spi_nor_program(const spi_nor_t *nor, uint32_t addr, const void *buf, uint32_t len);
// Erase the 4 KB sector holding addr; waits until done
bool spi_nor_erase_sector(const spi_nor_t *nor, uint32_t addr);
// Erase the 64 KB block holding addr (0xD8); waits until done
bool spi_nor_erase_block(const spi_nor_t *nor, uint32_t addr);
// Erase the whole chip (0xC7); waits until done (up to 200 s on a 16 MB part). Nothing in the core calls it
bool spi_nor_erase_chip(const spi_nor_t *nor);
// Read security register bytes (0x48). addr is the register address as the chip defines it (W25Q: reg << 12 | offset)
bool spi_nor_read_security(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len);
// The chip's unique ID (0x4B, four dummy bytes): 128 bits on the GD25Q and Puya PY25Q parts; a part with a 64-bit ID
// (W25Q) sends it in the first 8 bytes
#define SPI_NOR_UID_LEN		16u
bool spi_nor_read_uid(const spi_nor_t *nor, uint8_t id[SPI_NOR_UID_LEN]);
// Status register 1..3 (0x05 / 0x35 / 0x15)
bool spi_nor_status(const spi_nor_t *nor, uint8_t which, uint8_t *out);

#endif
