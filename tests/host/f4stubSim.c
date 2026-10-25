// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4 flash stub's real protocol engine (stub/common/stubeng.c) and board record check
// (stub/stm32f4/f4stub_board.c) on a simulated 1 MB flash, speaking its wire protocol on stdin / stdout, so the host
// tool (stub/f4stub.py) can be checked against it end to end (f4stub_wire_check.py). Usage: f4stubSim FLASH_IN
// FLASH_OUT; the flash is written out when stdin closes, and a 1 MB SPI flash (a fixed pattern at the start) to
// FLASH_OUT.spi. Every accepted board record is logged on stderr ("config BOARD BUS MODE CS"), so the check sees which
// wiring the stub would have used.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "f4stub.h"
#include "stubeng.h"

#define FLASH_SIZE	0x100000u

static uint8_t flash[FLASH_SIZE];
static stubeng_port_t port;
#define SPI_SIZE	0x100000u
static uint8_t spi[SPI_SIZE];

static void sim_write(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	fwrite(buf, 1, n, stdout);
	fflush(stdout);
}

static void sim_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
	(void)ctx;
	if (addr >= F4_FLASH_BASE && addr - F4_FLASH_BASE + n <= FLASH_SIZE) {
		memcpy(dst, flash + (addr - F4_FLASH_BASE), n);
	} else {
		memset(dst, 0, n);
	}
}

static bool sim_erase(void *ctx, unsigned sector)
{
	(void)ctx;
	memset(flash + (stubeng_sector_addr(&port.flash, sector) - F4_FLASH_BASE), 0xFF, stubeng_sector_size(&port.flash, sector));
	fprintf(stderr, "erase %u\n", sector);
	return true;
}

static bool sim_program(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n)
{
	(void)ctx;
	for (uint32_t i = 0; i < n; i++) {
		uint8_t *p = flash + (addr + 4u * i - F4_FLASH_BASE);
		uint32_t old;
		memcpy(&old, p, 4);
		old &= words[i];
		memcpy(p, &old, 4);
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
	f4_board_t b;
	if (!f4_board_parse(rec, n, &b)) {
		return false;
	}
	fprintf(stderr, "config %u %u %u %02X\n", b.board, b.spi_bus, b.spi_mode, b.cs);
	out->app_base = b.app_base;
	if (b.spi_bus != 0) {
		out->xsize = SPI_SIZE;
		out->xid[0] = 0xEF;
		out->xid[1] = 0x40;
		out->xid[2] = 0x14;
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
		fprintf(stderr, "usage: f4stubSim FLASH_IN FLASH_OUT\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL || fread(flash, 1, FLASH_SIZE, f) != FLASH_SIZE) {
		fprintf(stderr, "f4stubSim: cannot read %s\n", argv[1]);
		return 2;
	}
	fclose(f);
	port = (stubeng_port_t){
		.name = F4STUB_NAME,
		.version = F4STUB_VERSION,
		.magic = F4STUB_MAGIC,
		.flash = f4_flash(1024),
		.peek = f4_peek_ranges,
		.npeek = 4,
		.configure = sim_configure,
		.write = sim_write,
		.read = sim_read,
		.erase = sim_erase,
		.program = sim_program,
		.reset = sim_reset,
		.security = 0x55,
		.flags = F4_HELLO_USB_ADOPTED,
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
