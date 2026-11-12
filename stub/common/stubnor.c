// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The external SPI NOR flash for the flash stubs (stubnor.h). Every wait polls USB, so the host's control requests
// keep being answered while the chip is busy.
#include <stddef.h>
#include "stubnor.h"
#include "stubrt.h"
#include "stubusb.h"

#define PAGE		256u
#define ERASE_MS	1000u		// a 4 KB sector: 400 ms maximum on the W25Q128 data sheet
#define PROGRAM_MS	10u		// a page: 3 ms maximum

static bool up;

// One command: tx bytes out, then n bytes in (or n bytes of data out when out is set), chip select around it
static bool cmd(const uint8_t *tx, uint32_t ntx, uint8_t *in, const uint8_t *out, uint32_t n)
{
	nor_hw_select(true);
	bool ok = nor_hw_xfer(tx, NULL, ntx) && (n == 0 || nor_hw_xfer(out, in, n));
	nor_hw_select(false);
	return ok;
}

static bool cmd_addr(uint8_t op, uint32_t addr, uint8_t *in, const uint8_t *out, uint32_t n)
{
	uint8_t tx[4] = { op, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
	return cmd(tx, 4, in, out, n);
}

static bool wait_ready(uint32_t ms)
{
	uint32_t t0 = stub_ms();
	for (;;) {
		uint8_t op = 0x05, st = 0xFF;
		if (!cmd(&op, 1, &st, NULL, 1)) {
			return false;
		}
		if ((st & 1u) == 0) {
			return true;
		}
		if (stub_ms() - t0 > ms) {
			return false;
		}
		usb_poll();
	}
}

static bool write_enable(void)
{
	uint8_t op = 0x06, st = 0;
	if (!cmd(&op, 1, NULL, NULL, 0)) {
		return false;
	}
	op = 0x05;
	return cmd(&op, 1, &st, NULL, 1) && (st & 2u) != 0;
}

uint32_t stubnor_probe(uint8_t id[3])
{
	id[0] = id[1] = id[2] = 0;
	up = false;
	uint8_t wake = 0xAB, jedec = 0x9F;
	(void)cmd(&wake, 1, NULL, NULL, 0);
	stub_delay_ms(1);		// tRES1: 3 us
	if (!cmd(&jedec, 1, id, NULL, 3)) {
		return 0;
	}
	if ((id[0] == 0x00 && id[1] == 0x00) || (id[0] == 0xFF && id[1] == 0xFF) || id[2] < 0x10u) {
		return 0;
	}
	up = true;
	(void)wait_ready(ERASE_MS);	// RadioFoundry may have handed over in the middle of an erase
	return id[2] >= 0x18u ? 0x1000000u : 1u << id[2];	// 3 address bytes reach 16 MB
}

bool stubnor_read(uint32_t addr, uint8_t *dst, uint32_t n)
{
	return up && cmd_addr(0x03, addr, dst, NULL, n);
}

bool stubnor_erase(uint32_t addr)
{
	return up && wait_ready(ERASE_MS) && write_enable() && cmd_addr(0x20, addr, NULL, NULL, 0) && wait_ready(ERASE_MS);
}

bool stubnor_program(uint32_t addr, const uint8_t *data, uint32_t n)
{
	if (!up) {
		return false;
	}
	while (n > 0) {
		uint32_t k = PAGE - addr % PAGE;
		if (k > n) {
			k = n;
		}
		if (!wait_ready(PROGRAM_MS) || !write_enable() || !cmd_addr(0x02, addr, NULL, data, k) ||
		    !wait_ready(PROGRAM_MS)) {
			return false;
		}
		addr += k;
		data += k;
		n -= k;
	}
	return true;
}
