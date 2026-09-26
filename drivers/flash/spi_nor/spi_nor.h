// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Generic SPI NOR flash (W25Q / GD25Q class, 3-byte addresses). The board supplies the bus (chip select and a
// full-duplex transfer) and any locking; the driver only builds the command sequences.
#ifndef SPI_NOR_H
#define SPI_NOR_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	void (*select)(bool active);	// true = chip select asserted (low on the wire)
	// Clock len bytes. tx NULL sends 0xFF, rx NULL discards what comes back
	bool (*xfer)(const uint8_t *tx, uint8_t *rx, uint32_t len);
} spi_nor_bus_t;

typedef struct {
	const spi_nor_bus_t *bus;
	uint8_t manufacturer;	// JEDEC manufacturer ID (0xEF Winbond, 0xC8 GigaDevice, ...)
	uint16_t device;	// memory type << 8 | capacity code
	uint32_t size;		// bytes, from the capacity code (0 if unknown)
} spi_nor_t;

#define SPI_NOR_PAGE_SIZE	256u
#define SPI_NOR_SECTOR_SIZE	4096u
#define SPI_NOR_MAX_SIZE	0x1000000u	// what 3-byte addresses reach: a larger part is used as 16 MB

// Read the JEDEC ID (0x9F) and fill in manufacturer, device and size (at most SPI_NOR_MAX_SIZE). False if the bus fails
// or nothing answers
bool spi_nor_probe(spi_nor_t *nor);
bool spi_nor_read(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len);
// Program already erased bytes; split at page boundaries, waits for each page
bool spi_nor_program(const spi_nor_t *nor, uint32_t addr, const void *buf, uint32_t len);
// Erase the 4 KB sector holding addr; waits until done
bool spi_nor_erase_sector(const spi_nor_t *nor, uint32_t addr);
// Read security register bytes (0x48). addr is the register address as the chip defines it (W25Q: reg << 12 | offset)
bool spi_nor_read_security(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len);
// Status register 1..3 (0x05 / 0x35 / 0x15)
bool spi_nor_status(const spi_nor_t *nor, uint8_t which, uint8_t *out);

#endif
