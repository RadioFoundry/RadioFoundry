// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_CHIP_REG (core/debuglink/debuglink_chipreg.c, docs/spec/debuglink.md 3.24): an RF chip register through
// board.chip_reg, a SoC register through board.system->reg, the request lengths, a SoC write refused on the air, and
// boards without the hooks; chip DL_CHIP_MEM handed to dl_mem with its writes and pin changes refused on the air and
// logged. The board, the transmit check, the log and dl_mem are faked (dlMemTest has dl_mem itself)
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
// board's hooks change per test: defined writable, the code under test sees it const
#define board board_as_declared
#include "board.h"
#undef board
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

static int failures, checks;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                  \
	} while (0)

// --- fakes ---

static bool fake_tx;	// a call or the hotspot on the air
static unsigned logs;

bool debuglink_transmitting(void)
{
	return fake_tx;
}

void dlog(const char *fmt, ...)
{
	(void)fmt;
	logs++;
}

// Chip 0 page 0..1, registers 0..0x3F of 16 bits; chip 1 write-only
static uint16_t chip0[2][0x40];

static bool chip_reg(uint8_t chip, uint8_t page, uint8_t reg, uint32_t *val, bool write)
{
	if (chip == 1 && write && page == 0 && reg < 0x10 && *val <= 0xFFu) {
		return true;
	}
	if (chip != 0 || page > 1 || reg >= 0x40 || *val > 0xFFFFu) {
		return false;
	}
	if (write) {
		chip0[page][reg] = (uint16_t)*val;
	}
	*val = chip0[page][reg];
	return true;
}

// One SoC register at 0x17000008, which reads back with bit 31 forced (a read-only bit)
static uint32_t soc_word;
static unsigned soc_calls;

static bool soc_reg(uint32_t addr, uint32_t *val, bool write)
{
	soc_calls++;
	if (addr != 0x17000008u) {
		return false;
	}
	if (write) {
		soc_word = *val;
	}
	*val = soc_word | 0x80000000u;
	return true;
}

static hal_system_t sys = { .reg = soc_reg };

// dl_mem: what it was handed; a write or a bit op reads back 0x11223344, a pin read state 0x03
static uint8_t mem_req[16];
static uint16_t mem_len;
static unsigned mem_calls;

size_t debuglink_cmd_mem(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)cap;
	mem_calls++;
	mem_len = len;
	memcpy(mem_req, req, len < sizeof mem_req ? len : sizeof mem_req);
	out[0] = DL_ST_OK;
	if (len >= 1u && req[0] >= DL_MEM_OP_WRITE && req[0] <= DL_MEM_OP_CLEAR) {
		out[1] = 0x44, out[2] = 0x33, out[3] = 0x22, out[4] = 0x11;
		return 5;
	}
	if (len >= 1u && req[0] == DL_MEM_OP_PIN) {
		out[1] = 0x03;
		return 2;
	}
	return 1;
}
board_t board;

// --- helpers ---

static uint8_t out[16];

static size_t cmd(const uint8_t *req, uint16_t len)
{
	memset(out, 0xEE, sizeof out);
	return debuglink_cmd_chip_reg(req, len, out, sizeof out);
}

static uint32_t reply32(void)
{
	return (uint32_t)out[1] | ((uint32_t)out[2] << 8) | ((uint32_t)out[3] << 16) | ((uint32_t)out[4] << 24);
}

static void reset(void)
{
	board.chip_reg = chip_reg;
	board.system = &sys;
	sys.reg = soc_reg;
	fake_tx = false;
	memset(chip0, 0, sizeof chip0);
	soc_word = 0;
	soc_calls = logs = mem_calls = 0;
}

static void test_chip(void)
{
	reset();
	chip0[1][0x12] = 0xBEEF;
	uint8_t rd[3] = { 0, 1, 0x12 };
	CHECK(cmd(rd, 3) == 5 && out[0] == DL_ST_OK && reply32() == 0xBEEFu);
	uint8_t wr[7] = { 0, 0, 0x05, 0x34, 0x12, 0, 0 };
	CHECK(cmd(wr, 7) == 5 && out[0] == DL_ST_OK && reply32() == 0x1234u && chip0[0][5] == 0x1234u && logs == 1);
	// Write-only: the value written comes back
	uint8_t wo[7] = { 1, 0, 0x03, 0x7F, 0, 0, 0 };
	CHECK(cmd(wo, 7) == 5 && out[0] == DL_ST_OK && reply32() == 0x7Fu);
	// What the board doesn't offer
	uint8_t page[3] = { 0, 2, 0 }, chip[3] = { 5, 0, 0 }, big[7] = { 0, 0, 0, 0, 0, 1, 0 };
	CHECK(cmd(page, 3) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(chip, 3) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(big, 7) == 1 && out[0] == DL_ST_BAD_ARG);
	// Lengths
	CHECK(cmd(rd, 2) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(wr, 5) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(NULL, 0) == 1 && out[0] == DL_ST_BAD_ARG);
	// An RF chip write on the air goes through (trying a value on the air is what it is for)
	fake_tx = true;
	CHECK(cmd(wr, 7) == 5 && out[0] == DL_ST_OK);
	board.chip_reg = NULL;
	CHECK(cmd(rd, 3) == 1 && out[0] == DL_ST_UNAVAILABLE);
}

static void test_soc(void)
{
	reset();
	uint8_t rd[5] = { DL_CHIP_SOC, 0x08, 0x00, 0x00, 0x17 };
	CHECK(cmd(rd, 5) == 5 && out[0] == DL_ST_OK && reply32() == 0x80000000u);
	uint8_t wr[9] = { DL_CHIP_SOC, 0x08, 0x00, 0x00, 0x17, 0x40, 0x00, 0x00, 0x00 };
	CHECK(cmd(wr, 9) == 5 && out[0] == DL_ST_OK && reply32() == 0x80000040u && soc_word == 0x40u && logs == 1);
	// Outside the platform's windows: refused, as READ_MEM
	uint8_t off[5] = { DL_CHIP_SOC, 0x00, 0x00, 0x01, 0x00 };
	CHECK(cmd(off, 5) == 1 && out[0] == DL_ST_REFUSED);
	// A write on the air: refused before the platform sees it; a read still answers
	fake_tx = true;
	soc_calls = 0;
	wr[5] = 0x99;
	CHECK(cmd(wr, 9) == 1 && out[0] == DL_ST_REFUSED && soc_calls == 0 && soc_word == 0x40u);
	CHECK(cmd(rd, 5) == 5 && out[0] == DL_ST_OK);
	fake_tx = false;
	// Lengths: the SoC form is 5 or 9 bytes, not the RF chips' 3 or 7
	CHECK(cmd(rd, 3) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(wr, 7) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(rd, 1) == 1 && out[0] == DL_ST_BAD_ARG);
	// No RF chip hook doesn't hide the SoC, and the reverse
	board.chip_reg = NULL;
	CHECK(cmd(rd, 5) == 5 && out[0] == DL_ST_OK);
	board.chip_reg = chip_reg;
	sys.reg = NULL;
	CHECK(cmd(rd, 5) == 1 && out[0] == DL_ST_UNAVAILABLE);
	uint8_t chip_rd[3] = { 0, 0, 0 };
	CHECK(cmd(chip_rd, 3) == 5 && out[0] == DL_ST_OK);
	board.system = NULL;
	CHECK(cmd(rd, 5) == 1 && out[0] == DL_ST_UNAVAILABLE);
}

static void test_mem(void)
{
	reset();
	// The operation on is dl_mem's request
	uint8_t wr[11] = { DL_CHIP_MEM, DL_MEM_OP_WRITE, 4, 0x00, 0x00, 0x00, 0x20, 0x01, 0x00, 0x00, 0x00 };
	CHECK(cmd(wr, 11) == 5 && out[0] == DL_ST_OK && reply32() == 0x11223344u);
	CHECK(mem_calls == 1 && mem_len == 10 && memcmp(mem_req, wr + 1, 10) == 0 && logs == 1);
	uint8_t arm[6] = { DL_CHIP_MEM, DL_MEM_OP_ARM, 0x50, 0x4F, 0x4B, 0x45 };
	CHECK(cmd(arm, 6) == 1 && out[0] == DL_ST_OK && mem_calls == 2 && logs == 1);
	uint8_t pin[4] = { DL_CHIP_MEM, DL_MEM_OP_PIN, 7, DL_MEM_PIN_HIGH };
	CHECK(cmd(pin, 4) == 2 && out[0] == DL_ST_OK && out[1] == 0x03 && logs == 2);
	// On the air: writes, bit ops and pin changes refused before dl_mem; reads, a pin read and the key still go
	fake_tx = true;
	mem_calls = 0;
	CHECK(cmd(wr, 11) == 1 && out[0] == DL_ST_REFUSED);
	wr[1] = DL_MEM_OP_SET;
	CHECK(cmd(wr, 11) == 1 && out[0] == DL_ST_REFUSED);
	wr[1] = DL_MEM_OP_CLEAR;
	CHECK(cmd(wr, 11) == 1 && out[0] == DL_ST_REFUSED);
	CHECK(cmd(pin, 4) == 1 && out[0] == DL_ST_REFUSED && mem_calls == 0 && logs == 2);
	pin[3] = DL_MEM_PIN_READ;
	CHECK(cmd(pin, 4) == 2 && out[0] == DL_ST_OK && logs == 2);
	uint8_t rd[8] = { DL_CHIP_MEM, DL_MEM_OP_READ, 4, 0x00, 0x00, 0x00, 0x20, 1 };
	CHECK(cmd(rd, 8) == 1 && cmd(arm, 6) == 1 && mem_calls == 3 && logs == 2);
	// No board hooks needed: dl_mem has its own
	fake_tx = false;
	board.chip_reg = NULL;
	board.system = NULL;
	CHECK(cmd(rd, 8) == 1 && mem_calls == 4);
	// The operation byte missing: dl_mem's to refuse
	CHECK(cmd(rd, 1) == 1 && mem_calls == 5 && mem_len == 0);
}

static void test_short_buffer(void)
{
	reset();
	uint8_t rd[3] = { 0, 0, 0 };
	uint8_t small[4];
	CHECK(debuglink_cmd_chip_reg(rd, 3, small, sizeof small) == 1 && small[0] == DL_ST_BAD_ARG);
}

int main(void)
{
	test_chip();
	test_soc();
	test_mem();
	test_short_buffer();
	printf("debuglinkChipRegTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
