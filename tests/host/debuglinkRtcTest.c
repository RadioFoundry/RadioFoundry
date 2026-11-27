// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_RTC (core/debuglink/debuglink_rtc.c, docs/spec/debuglink.md 3.31): read the board's hardware clock and set
// it, the request lengths, a clock that holds no time, a bus that does not answer, a board without a clock. The board
// and the log are faked
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
// board.rtc changes per test: defined writable, the code under test sees it const
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

static unsigned logs;

void dlog(const char *fmt, ...)
{
	(void)fmt;
	logs++;
}

// A clock that holds a time once set, refuses a time past 2100 and can lose its bus
static struct {
	bool valid, bus_ok;
	int64_t t;
} fake;

static bool fake_read(int64_t *t)
{
	if (!fake.bus_ok || !fake.valid) {
		return false;
	}
	*t = fake.t;
	return true;
}

static bool fake_set(int64_t t)
{
	if (!fake.bus_ok || t > 4102444800LL) {
		return false;
	}
	fake.valid = true;
	fake.t = t;
	return true;
}

static const hal_rtc_t rtc = { .read = fake_read, .set = fake_set };
board_t board;

static uint8_t out[16];

static size_t cmd(const uint8_t *req, uint16_t len)
{
	memset(out, 0xEE, sizeof out);
	return debuglink_cmd_rtc(req, len, out, sizeof out);
}

static uint32_t reply_unix(void)
{
	return (uint32_t)out[2] | ((uint32_t)out[3] << 8) | ((uint32_t)out[4] << 16) | ((uint32_t)out[5] << 24);
}

static void reset(void)
{
	memset(&fake, 0, sizeof fake);
	fake.bus_ok = true;
	board.rtc = &rtc;
	logs = 0;
}

int main(void)
{
	const uint8_t get[1] = { DL_RTC_GET };

	// A clock that holds no time
	reset();
	CHECK(cmd(get, 1) == 6 && out[0] == DL_ST_OK && out[1] == 0 && reply_unix() == 0);

	// Set, then read back (1795639230 = 2026-11-25 20:40:30 UTC)
	uint32_t t = 1795639230u;
	uint8_t set[5] = { DL_RTC_SET, (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24) };
	CHECK(cmd(set, 5) == 1 && out[0] == DL_ST_OK && logs == 1);
	CHECK(cmd(get, 1) == 6 && out[0] == DL_ST_OK && out[1] == 1 && reply_unix() == t);

	// A time the clock cannot hold (past 2100 here), and a bus that does not answer: refused, nothing changed
	uint8_t big[5] = { DL_RTC_SET, 0xFF, 0xFF, 0xFF, 0xFF };
	CHECK(cmd(big, 5) == 1 && out[0] == DL_ST_REFUSED);
	CHECK(cmd(get, 1) == 6 && reply_unix() == t);
	fake.bus_ok = false;
	CHECK(cmd(set, 5) == 1 && out[0] == DL_ST_REFUSED);
	CHECK(cmd(get, 1) == 6 && out[0] == DL_ST_OK && out[1] == 0);	// a read the bus did not answer: no time
	fake.bus_ok = true;

	// Lengths and operations
	uint8_t bad_op[1] = { 9 };
	CHECK(cmd(bad_op, 1) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(set, 4) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(get, 2) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(NULL, 0) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(debuglink_cmd_rtc(get, 1, out, 5) == 1 && out[0] == DL_ST_BAD_ARG);	// a reply buffer too small

	// A board without a clock, or a driver with no read or no set
	board.rtc = NULL;
	CHECK(cmd(get, 1) == 1 && out[0] == DL_ST_UNAVAILABLE);
	CHECK(cmd(set, 5) == 1 && out[0] == DL_ST_UNAVAILABLE);
	hal_rtc_t read_only = { .read = fake_read };
	board.rtc = &read_only;
	CHECK(cmd(set, 5) == 1 && out[0] == DL_ST_BAD_ARG);

	printf("# debuglinkRtcTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
