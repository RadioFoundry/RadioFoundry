// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Generic SPI NOR flash: command set from the Winbond W25Q / GigaDevice GD25Q datasheets
// (docs/spec/hardware-dm1701.md 13.1)
#include <stddef.h>
#include "spi_nor.h"
#include "hal_time.h"

enum {
	CMD_READ		= 0x03,
	CMD_PAGE_PROGRAM	= 0x02,
	CMD_SECTOR_ERASE	= 0x20,
	CMD_WRITE_ENABLE	= 0x06,
	CMD_STATUS1		= 0x05,
	CMD_STATUS2		= 0x35,
	CMD_STATUS3		= 0x15,
	CMD_JEDEC_ID		= 0x9F,
	CMD_READ_SECURITY	= 0x48,
};

#define STATUS1_BUSY	0x01u
#define STATUS1_WEL	0x02u

// Busy limits in 1 ms polls (datasheet maximums: page program 3 ms, 4 KB erase 400 ms)
#define PROGRAM_POLLS	20u
#define ERASE_POLLS	1000u

static void addr_bytes(uint8_t *p, uint8_t cmd, uint32_t addr)
{
	p[0] = cmd;
	p[1] = (uint8_t)(addr >> 16);
	p[2] = (uint8_t)(addr >> 8);
	p[3] = (uint8_t)addr;
}

// One command: a header clocked out, then len bytes read (rx) or written (tx) in the same chip select
static bool command(const spi_nor_t *nor, const uint8_t *hdr, uint32_t hdr_len,
		    const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	const spi_bus_t *bus = nor->bus;
	bus->select(true);
	bool ok = bus->xfer(hdr, NULL, hdr_len);
	if (ok && len > 0) {
		ok = bus->xfer(tx, rx, len);
	}
	bus->select(false);
	return ok;
}

bool spi_nor_status(const spi_nor_t *nor, uint8_t which, uint8_t *out)
{
	static const uint8_t cmds[] = { CMD_STATUS1, CMD_STATUS2, CMD_STATUS3 };
	if (which < 1 || which > 3) {
		return false;
	}
	return command(nor, &cmds[which - 1], 1, NULL, out, 1);
}

static bool wait_ready(const spi_nor_t *nor, uint32_t polls)
{
	for (uint32_t i = 0; i <= polls; i++) {
		uint8_t st;
		if (!spi_nor_status(nor, 1, &st)) {
			return false;
		}
		if ((st & STATUS1_BUSY) == 0) {
			return true;
		}
		hal_delay_ms(1);
	}
	return false;
}

static bool write_enable(const spi_nor_t *nor)
{
	static const uint8_t cmd = CMD_WRITE_ENABLE;
	uint8_t st;
	return command(nor, &cmd, 1, NULL, NULL, 0) && spi_nor_status(nor, 1, &st) && (st & STATUS1_WEL) != 0;
}

bool spi_nor_probe(spi_nor_t *nor)
{
	static const uint8_t cmd = CMD_JEDEC_ID;
	uint8_t id[3];
	nor->manufacturer = 0;
	nor->device = 0;
	nor->size = 0;
	if (!command(nor, &cmd, 1, NULL, id, 3)) {
		return false;
	}
	if ((id[0] == 0x00 && id[1] == 0x00) || (id[0] == 0xFF && id[1] == 0xFF)) {
		return false;	// bus stuck low or nothing driving MISO
	}
	nor->manufacturer = id[0];
	nor->device = (uint16_t)((id[1] << 8) | id[2]);
	if (id[2] >= 0x10 && id[2] <= 0x18) {
		nor->size = 1u << id[2];	// W25Q / GD25Q capacity code: 2^n bytes (0x18 = 16 MB)
	} else if (id[2] >= 0x19 && id[2] <= 0x22) {
		// Larger parts (0x19 32 MB; Winbond goes on 0x20 64 MB, 0x21 128 MB, others 0x1A...): 3-byte addresses
		// reach only the first 16 MB, and an address above that would wrap to the bottom of the chip
		nor->size = SPI_NOR_MAX_SIZE;
	}
	return true;
}

bool spi_nor_read(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len)
{
	uint8_t hdr[4];
	if (len == 0) {
		return true;
	}
	addr_bytes(hdr, CMD_READ, addr);
	return command(nor, hdr, sizeof(hdr), NULL, buf, len);
}

bool spi_nor_read_security(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len)
{
	uint8_t hdr[5];
	if (len == 0) {
		return true;
	}
	addr_bytes(hdr, CMD_READ_SECURITY, addr);
	hdr[4] = 0xFF;	// dummy byte
	return command(nor, hdr, sizeof(hdr), NULL, buf, len);
}

bool spi_nor_program(const spi_nor_t *nor, uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		uint32_t room = SPI_NOR_PAGE_SIZE - (addr % SPI_NOR_PAGE_SIZE);
		uint32_t n = len < room ? len : room;
		uint8_t hdr[4];
		addr_bytes(hdr, CMD_PAGE_PROGRAM, addr);
		if (!write_enable(nor) || !command(nor, hdr, sizeof(hdr), p, NULL, n) || !wait_ready(nor, PROGRAM_POLLS)) {
			return false;
		}
		addr += n;
		p += n;
		len -= n;
	}
	return true;
}

bool spi_nor_erase_sector(const spi_nor_t *nor, uint32_t addr)
{
	uint8_t hdr[4];
	addr_bytes(hdr, CMD_SECTOR_ERASE, addr & ~(SPI_NOR_SECTOR_SIZE - 1u));
	return write_enable(nor) && command(nor, hdr, sizeof(hdr), NULL, NULL, 0) && wait_ready(nor, ERASE_POLLS);
}
