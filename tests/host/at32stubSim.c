// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT32F43x flash stub's real protocol engine (stub/common/stubeng.c) and board record check
// (stub/at32f43x/at32stub_board.c) on a simulated 1 MB flash, speaking its wire protocol on stdin / stdout, so the host
// tool (stub/f4stub.py) can be checked against it end to end (f4stub_wire_check.py --at32). Usage: at32stubSim FLASH_IN
// FLASH_OUT; the flash is written out when stdin closes. Each accepted board record is logged on stderr
// ("config BOARD APP_BASE"), each erase ("erase SECTOR") and the reset. No SPI flash: the target has none.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "at32stub.h"
#include "stubeng.h"

#define FLASH_SIZE	AT32_STUB_FLASH_MAX

static uint8_t flash[FLASH_SIZE];
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
	if (addr >= AT32_FLASH_START && addr - AT32_FLASH_START + n <= FLASH_SIZE) {
		memcpy(dst, flash + (addr - AT32_FLASH_START), n);
	} else {
		memset(dst, 0, n);
	}
}

static bool sim_erase(void *ctx, unsigned sector)
{
	(void)ctx;
	memset(flash + (stubeng_sector_addr(&port.flash, sector) - AT32_FLASH_START), 0xFF,
	       stubeng_sector_size(&port.flash, sector));
	fprintf(stderr, "erase %u\n", sector);
	return true;
}

static bool sim_program(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n)
{
	(void)ctx;
	for (uint32_t i = 0; i < n; i++) {
		uint8_t *p = flash + (addr + 4u * i - AT32_FLASH_START);
		uint32_t old;
		memcpy(&old, p, 4);
		old &= words[i];
		memcpy(p, &old, 4);
	}
	return true;
}

static bool sim_configure(void *ctx, const uint8_t *rec, uint32_t n, stubeng_config_t *out)
{
	(void)ctx;
	at32_board_t b;
	if (!at32_board_parse(rec, n, &b)) {
		return false;
	}
	fprintf(stderr, "config %u %08X\n", b.board, (unsigned)b.app_base);
	out->app_base = b.app_base;
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
		fprintf(stderr, "usage: at32stubSim FLASH_IN FLASH_OUT\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL || fread(flash, 1, FLASH_SIZE, f) != FLASH_SIZE) {
		fprintf(stderr, "at32stubSim: cannot read %s\n", argv[1]);
		return 2;
	}
	fclose(f);
	port = (stubeng_port_t){
		.name = AT32STUB_NAME,
		.version = AT32STUB_VERSION,
		.magic = AT32STUB_MAGIC,
		.flash = at32_flash(1024),
		.peek = at32_peek_ranges,
		.npeek = sizeof(at32_peek_ranges) / sizeof(at32_peek_ranges[0]),
		.configure = sim_configure,
		.write = sim_write,
		.read = sim_read,
		.erase = sim_erase,
		.program = sim_program,
		.reset = sim_reset,
		.security = 0xAA,
		.flags = AT32_HELLO_USB_ADOPTED,
	};
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
	return 0;
}
