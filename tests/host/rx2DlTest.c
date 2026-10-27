// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A second SoC's debug link server (core/rx2/rx2_dl.c, docs/spec/rx2-link.md 2.3): the main SoC's reply layouts,
// READ_MEM's windows from board.system, the restart's token with and without a chip UID, the restart only after the
// reply, the log ring and an unknown command. The board, the clock and the memory are faked
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
// board changes per test: defined writable, the code under test sees it const
#define board board_as_declared
#include "board.h"
#undef board
#include "rx2/rx2_dl.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_chip.h"
#include "debuglink/dlog.h"

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

static uint32_t now_ms = 5000;
static unsigned reboots;
static bool have_chip = true;

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_ms(uint32_t ms) { now_ms += ms; }

static bool chip(hal_chip_t *c)
{
	if (!have_chip) {
		return false;
	}
	memset(c, 0, sizeof *c);
	c->family = HAL_CHIP_HRC7000;
	c->uid[0] = 0x11223344u;
	c->flash_kb = 2048;
	return true;
}

// One window: 0x1000-0x10FF reads as the low byte of its address
static bool readable(uint32_t addr, uint32_t n) { return addr >= 0x1000u && addr + n <= 0x1100u; }
static void readmem(uint32_t addr, void *dst, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		((uint8_t *)dst)[i] = (uint8_t)(addr + i);
	}
}
static bool reboot(uint8_t mode) { (void)mode; reboots++; return true; }

static hal_system_t sys = { .chip = chip, .read = readmem, .readable = readable, .reboot = reboot };

static bool diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (i >= 2) {
		return false;
	}
	snprintf(buf, len, "line %u", i);
	return true;
}

board_t board;
static rx2_recv_t link;

static void own_view(dl_text_t *t) { dl_text(t, "b.test=7\n"); }

// --- helpers ---

static uint8_t out[RX2_DL_REPLY_MAX];

static size_t cmd(uint8_t c, const uint8_t *req, uint16_t len)
{
	memset(out, 0xEE, sizeof out);
	return rx2_dl_handle(c, req, len, out, sizeof out);
}

static bool has(size_t off, size_t n, const char *s)
{
	char buf[sizeof out + 1];
	memcpy(buf, out + off, n - off);
	buf[n - off] = '\0';
	return strstr(buf, s) != NULL;
}

static void setup(void)
{
	memset(&board, 0, sizeof board);
	board.name = "testb";
	board.system = &sys;
	board.diag_line = diag_line;
	sys.readable = readable;
	have_chip = true;
	reboots = 0;
	link.frames = 12;
	link.dl_requests = 3;
	rx2_dl_init(&link, own_view);
}

// --- tests ---

static void test_basic(void)
{
	setup();
	uint8_t p[3] = { 1, 2, 3 };
	CHECK(cmd(DL_CMD_PING, p, 3) == 4 && out[0] == DL_ST_OK && out[3] == 3);
	size_t n = cmd(DL_CMD_VERSION, NULL, 0);
	CHECK(out[0] == DL_ST_OK && has(1, n, "board=testb\n") && has(1, n, "via=forward\n") && has(1, n, "proto="));
	CHECK(cmd(0x7D, NULL, 0) == 1 && out[0] == DL_ST_UNKNOWN_CMD);

	n = cmd(DL_CMD_GET_DIAG, NULL, 0);
	CHECK(out[0] == DL_ST_OK && out[5] == 0xFF && has(6, n, "line 0\nline 1\n"));
	uint8_t from = 1;
	n = cmd(DL_CMD_GET_DIAG, &from, 1);
	CHECK(out[5] == 0xFF && has(6, n, "line 1\n") && !has(6, n, "line 0"));
	board.diag_line = NULL;
	n = cmd(DL_CMD_GET_DIAG, NULL, 0);
	CHECK(n == 6 && out[5] == 0xFF);
}

static void test_view(void)
{
	setup();
	uint8_t sec = 0;
	size_t n = cmd(DL_CMD_GET_VIEW, &sec, 1);
	CHECK(out[0] == DL_ST_OK && out[5] == 1 && has(6, n, "board=testb\n") && has(6, n, "rx2.frames=12\n") &&
	      has(6, n, "rx2.dl_requests=3\n") && has(6, n, "b.test=7\n"));
	sec = 1;
	n = cmd(DL_CMD_GET_VIEW, &sec, 1);
	CHECK(out[0] == DL_ST_OK && out[5] == 2 && has(6, n, "crash.built=0"));
	sec = 2;
	n = cmd(DL_CMD_GET_VIEW, &sec, 1);
	CHECK(out[0] == DL_ST_OK && out[5] == 0xFF && has(6, n, "reset.built=0"));
	sec = 3;
	CHECK(cmd(DL_CMD_GET_VIEW, &sec, 1) == 1 && out[0] == DL_ST_BAD_ARG);
}

static void test_read_mem(void)
{
	setup();
	uint8_t r[6] = { 0x10, 0x10, 0, 0, 8, 0 };	// 0x1010, 8 bytes
	CHECK(cmd(DL_CMD_READ_MEM, r, 6) == 13 && out[0] == DL_ST_OK && out[1] == 0x10 && out[2] == 0x10 &&
	      out[5] == 0x10 && out[12] == 0x17);
	r[1] = 0x20;					// 0x2010: outside
	CHECK(cmd(DL_CMD_READ_MEM, r, 6) == 1 && out[0] == DL_ST_REFUSED);
	r[0] = 0xFC; r[1] = 0x10;			// 0x10FC + 8: runs past the end
	CHECK(cmd(DL_CMD_READ_MEM, r, 6) == 1 && out[0] == DL_ST_REFUSED);
	r[4] = 0;					// none
	CHECK(cmd(DL_CMD_READ_MEM, r, 6) == 1 && out[0] == DL_ST_BAD_ARG);
	r[4] = (uint8_t)(DL_MEM_READ_MAX + 1u); r[5] = (uint8_t)((DL_MEM_READ_MAX + 1u) >> 8);
	CHECK(cmd(DL_CMD_READ_MEM, r, 6) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(DL_CMD_READ_MEM, r, 5) == 1 && out[0] == DL_ST_BAD_ARG);
	sys.readable = NULL;				// no windows: never read
	r[0] = 0x10; r[4] = 4; r[5] = 0;
	CHECK(cmd(DL_CMD_READ_MEM, r, 6) == 1 && out[0] == DL_ST_UNAVAILABLE);
}

static size_t reboot_with(uint8_t mode, uint32_t token)
{
	uint8_t r[5] = { mode, (uint8_t)token, (uint8_t)(token >> 8), (uint8_t)(token >> 16), (uint8_t)(token >> 24) };
	return cmd(DL_CMD_REBOOT, r, 5);
}

static void test_reboot(void)
{
	setup();
	CHECK(reboot_with(HAL_REBOOT_NORMAL, 0x11223344u) == 2 && out[0] == DL_ST_OK);
	CHECK(reboots == 0);				// never from the handler: the reply goes out first
	uint32_t t = now_ms;
	rx2_dl_service();
	CHECK(reboots == 1 && now_ms - t >= 50);
	rx2_dl_service();
	CHECK(reboots == 1);				// once

	setup();
	CHECK(reboot_with(HAL_REBOOT_NORMAL, DL_RX2_TOKEN) == 2 && out[0] == DL_ST_REFUSED &&
	      out[1] == DL_REBOOT_BAD_TOKEN);
	CHECK(reboot_with(HAL_REBOOT_ROM_DFU, 0x11223344u) == 2 && out[0] == DL_ST_REFUSED && out[1] == DL_REBOOT_RDP);
	rx2_dl_service();
	CHECK(reboots == 0);

	// No chip UID: INFO_CHIP is unavailable and the restart takes the bridge's token
	setup();
	have_chip = false;
	CHECK(cmd(DL_CMD_INFO_CHIP, NULL, 0) == 1 && out[0] == DL_ST_UNAVAILABLE);
	CHECK(reboot_with(HAL_REBOOT_NORMAL, 0) == 2 && out[0] == DL_ST_REFUSED);
	CHECK(reboot_with(HAL_REBOOT_NORMAL, DL_RX2_TOKEN) == 2 && out[0] == DL_ST_OK);
	rx2_dl_service();
	CHECK(reboots == 1);

	setup();
	CHECK(cmd(DL_CMD_INFO_CHIP, NULL, 0) == 30 && out[0] == DL_ST_OK && out[7] == 0x44 && out[29] == HAL_CHIP_HRC7000);
	board.system = NULL;
	CHECK(reboot_with(HAL_REBOOT_NORMAL, 0x11223344u) == 1 && out[0] == DL_ST_UNAVAILABLE);
	CHECK(cmd(DL_CMD_INFO_CHIP, NULL, 0) == 1 && out[0] == DL_ST_UNAVAILABLE);
}

static void test_log(void)
{
	setup();
	dlog("b line %d", 42);
	size_t n = cmd(DL_CMD_GET_LOG, NULL, 0);
	CHECK(out[0] == DL_ST_OK && out[5] == 0 && has(6, n, "b line 42"));
	n = cmd(DL_CMD_GET_LOG, NULL, 0);
	CHECK(n == 6);					// read once
	// Without the crash log, CRASH answers unavailable
	uint8_t c = DL_CRASH_OP_CLEAR;
	CHECK(cmd(DL_CMD_CRASH, &c, 1) == 1 && out[0] == DL_ST_UNAVAILABLE);
}

int main(void)
{
	test_basic();
	test_view();
	test_read_mem();
	test_reboot();
	test_log();
	printf("rx2DlTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
