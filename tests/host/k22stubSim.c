// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The K22F flash stub's real protocol engine (stub/common/stubeng.c) and board record check (stub/k22f/k22stub_board.c)
// on a simulated 512 KB flash and a 2 MB SPI flash, speaking its wire protocol on stdin / stdout, so the host tool
// (stub/k22stub.py) can be checked against it end to end (k22stub_wire_check.py). Usage: k22stubSim FLASH_IN FLASH_OUT;
// the flash is written out when stdin closes, the SPI flash (a fixed pattern) to FLASH_OUT.spi. Erases, SPI erases and
// every accepted board record are logged on stderr.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "k22stub.h"
#include "stubeng.h"

#define FLASH_SIZE	0x80000u
#define SPI_SIZE	0x200000u

static uint8_t flash[FLASH_SIZE];
static uint8_t spi[SPI_SIZE];
static stubeng_port_t port;

static void sim_write(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	fwrite(buf, 1, n, stdout);
	fflush(stdout);
}

static void sim_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
	(void)ctx;
	if (addr + n <= FLASH_SIZE) {
		memcpy(dst, flash + addr, n);
	} else {
		memset(dst, 0, n);
	}
}

static bool sim_erase(void *ctx, unsigned sector)
{
	(void)ctx;
	memset(flash + stubeng_sector_addr(&port.flash, sector), 0xFF, stubeng_sector_size(&port.flash, sector));
	fprintf(stderr, "erase %u\n", sector);
	return true;
}

static bool sim_program(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n)
{
	(void)ctx;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t old;
		memcpy(&old, flash + addr + 4u * i, 4);
		old &= words[i];
		memcpy(flash + addr + 4u * i, &old, 4);
	}
	return true;
}

static bool sim_xread(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
	(void)ctx;
	memcpy(dst, spi + addr, n);
	return true;
}

static bool sim_xerase(void *ctx, uint32_t addr)
{
	(void)ctx;
	memset(spi + addr, 0xFF, STUBENG_BLOCK);
	fprintf(stderr, "xerase %u\n", (unsigned)addr);
	return true;
}

static bool sim_xprogram(void *ctx, uint32_t addr, const uint8_t *data, uint32_t n)
{
	(void)ctx;
	for (uint32_t i = 0; i < n; i++) {
		spi[addr + i] &= data[i];
	}
	return true;
}

static bool sim_configure(void *ctx, const uint8_t *rec, uint32_t n, stubeng_config_t *out)
{
	(void)ctx;
	k22_board_t b;
	if (!k22_board_parse(rec, n, &b)) {
		return false;
	}
	fprintf(stderr, "config %u %02X %02X %02X %02X %u\n", b.board, b.sck, b.mosi, b.miso, b.cs, b.cs_released ? 1u : 0u);
	out->app_base = b.app_base;
	out->app_end = b.app_end;
	out->xfloor = b.xfloor;
	if (b.xflash != 0) {
		out->xsize = SPI_SIZE;
		out->xid[0] = 0xEF;
		out->xid[1] = 0x40;
		out->xid[2] = 0x15;
	}
	return true;
}

static void sim_reset(void *ctx)
{
	(void)ctx;
	fprintf(stderr, "reset\n");
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: k22stubSim FLASH_IN FLASH_OUT\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL || fread(flash, 1, FLASH_SIZE, f) != FLASH_SIZE) {
		fprintf(stderr, "k22stubSim: cannot read %s\n", argv[1]);
		return 2;
	}
	fclose(f);
	port = (stubeng_port_t){
		.name = K22STUB_NAME,
		.version = K22STUB_VERSION,
		.magic = K22STUB_MAGIC,
		.flash = k22_flash(512),
		.peek = k22_peek_ranges,
		.npeek = sizeof(k22_peek_ranges) / sizeof(k22_peek_ranges[0]),
		.configure = sim_configure,
		.write = sim_write,
		.read = sim_read,
		.erase = sim_erase,
		.program = sim_program,
		.reset = sim_reset,
		.security = 0xFE,
		.flags = K22_HELLO_USB_ADOPTED,
		.xread = sim_xread,
		.xerase = sim_xerase,
		.xprogram = sim_xprogram,
	};
	for (uint32_t i = 0; i < SPI_SIZE; i++) {
		spi[i] = (uint8_t)(i * 13u + (i >> 12));
	}
	static stubeng_t eng;
	stubeng_init(&eng, &port);
	uint8_t buf[4096];
	ssize_t n;
	while ((n = read(0, buf, sizeof(buf))) > 0) {
		stubeng_feed(&eng, buf, (uint32_t)n);
	}
	f = fopen(argv[2], "wb");
	if (f == NULL || fwrite(flash, 1, FLASH_SIZE, f) != FLASH_SIZE) {
		return 2;
	}
	fclose(f);
	char name[4096];
	snprintf(name, sizeof(name), "%s.spi", argv[2]);
	f = fopen(name, "wb");
	if (f == NULL || fwrite(spi, 1, SPI_SIZE, f) != SPI_SIZE) {
		return 2;
	}
	fclose(f);
	return 0;
}
