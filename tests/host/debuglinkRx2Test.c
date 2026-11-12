// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_RX2 (core/debuglink/debuglink_rx2.c, docs/spec/debuglink.md 3.22): INFO's flags, the token and on-air checks,
// the bridge starting only from the service hook after the reply, and a board without a second receiver. The board,
// the clock, the radio's view and the log are faked
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
// board.rx2 changes per test: defined writable, the code under test sees it const
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

static uint32_t now_ms;
static radio_state_t fake_state = RS_RX;
static radio_state_t state_after_delay = RS_RX;
static bool up;
static unsigned bridges, restarts;

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_ms(uint32_t ms) { now_ms += ms; fake_state = state_after_delay; }

void radio_get_view(radio_view_t *v)
{
	memset(v, 0, sizeof *v);
	v->state = fake_state;
}

void dlog(const char *fmt, ...)
{
	(void)fmt;
}

static void st(rx2_status_t *s)
{
	memset(s, 0, sizeof *s);
	s->up = up;
}

static void bridge(void) { bridges++; }
static void restart(void) { restarts++; }

// FORWARD: the request it got, and the reply it gives (0: no answer)
static uint8_t fwd_req[80], fwd_len;
static size_t fwd_reply;
static size_t forward(const uint8_t *req, uint8_t len, uint8_t *o, size_t cap)
{
	memcpy(fwd_req, req, len);
	fwd_len = len;
	size_t n = fwd_reply < cap ? fwd_reply : cap;
	for (size_t i = 0; i < n; i++) {
		o[i] = (uint8_t)(0xB0 + i);
	}
	return n;
}

static hal_rx2_t rx2 = { .status = st, .bridge = bridge, .restart = restart, .forward = forward };
board_t board;

// --- helpers ---

static uint8_t out[16];

static size_t cmd(const uint8_t *req, uint16_t len)
{
	memset(out, 0xEE, sizeof out);
	return debuglink_cmd_rx2(req, len, out, sizeof out);
}

static size_t op_token(uint8_t op, uint32_t token)
{
	uint8_t req[5] = { op, (uint8_t)token, (uint8_t)(token >> 8), (uint8_t)(token >> 16), (uint8_t)(token >> 24) };
	return cmd(req, 5);
}

static void reset(void)
{
	board.rx2 = &rx2;
	rx2.bridge = bridge;
	rx2.restart = restart;
	rx2.forward = forward;
	fwd_reply = 0;
	fake_state = state_after_delay = RS_RX;
	up = false;
	bridges = restarts = 0;
	debuglink_rx2_service();	// nothing pending from an earlier test
	bridges = 0;
}

static void test_info(void)
{
	reset();
	uint8_t req = DL_RX2_OP_INFO;
	CHECK(cmd(&req, 1) == 2 && out[0] == DL_ST_OK);
	CHECK(out[1] == (DL_RX2_F_PRESENT | DL_RX2_F_BRIDGE | DL_RX2_F_RESTART | DL_RX2_F_FORWARD));
	up = true;
	cmd(&req, 1);
	CHECK(out[1] == (DL_RX2_F_PRESENT | DL_RX2_F_UP | DL_RX2_F_BRIDGE | DL_RX2_F_RESTART | DL_RX2_F_FORWARD));
	rx2.bridge = NULL;
	rx2.restart = NULL;
	rx2.forward = NULL;
	cmd(&req, 1);
	CHECK(out[1] == (DL_RX2_F_PRESENT | DL_RX2_F_UP));
	board.rx2 = NULL;
	CHECK(cmd(&req, 1) == 2 && out[0] == DL_ST_OK && out[1] == 0);
}

static void test_bridge(void)
{
	reset();
	CHECK(op_token(DL_RX2_OP_BRIDGE, DL_RX2_TOKEN) == 1 && out[0] == DL_ST_OK);
	CHECK(bridges == 0);			// never from the handler: the reply goes out first
	debuglink_rx2_service();
	CHECK(bridges == 1 && now_ms >= 100);
	debuglink_rx2_service();
	CHECK(bridges == 1);			// once

	reset();
	CHECK(op_token(DL_RX2_OP_BRIDGE, 0x12345678u) == 1 && out[0] == DL_ST_REFUSED);
	debuglink_rx2_service();
	CHECK(bridges == 0);

	reset();
	fake_state = RS_TX;
	CHECK(op_token(DL_RX2_OP_BRIDGE, DL_RX2_TOKEN) == 1 && out[0] == DL_ST_REFUSED);

	// PTT between the reply and the bridge: dropped
	reset();
	state_after_delay = RS_TX;
	CHECK(op_token(DL_RX2_OP_BRIDGE, DL_RX2_TOKEN) == 1 && out[0] == DL_ST_OK);
	debuglink_rx2_service();
	CHECK(bridges == 0);
}

static void test_restart(void)
{
	reset();
	CHECK(op_token(DL_RX2_OP_RESTART, DL_RX2_TOKEN) == 1 && out[0] == DL_ST_OK && restarts == 1);
	CHECK(op_token(DL_RX2_OP_RESTART, 0) == 1 && out[0] == DL_ST_REFUSED && restarts == 1);
}

static void test_unavailable(void)
{
	reset();
	board.rx2 = NULL;
	CHECK(op_token(DL_RX2_OP_BRIDGE, DL_RX2_TOKEN) == 1 && out[0] == DL_ST_UNAVAILABLE);
	CHECK(op_token(DL_RX2_OP_RESTART, DL_RX2_TOKEN) == 1 && out[0] == DL_ST_UNAVAILABLE);
	reset();
	rx2.bridge = NULL;
	CHECK(op_token(DL_RX2_OP_BRIDGE, DL_RX2_TOKEN) == 1 && out[0] == DL_ST_UNAVAILABLE);
	// Malformed
	CHECK(cmd(NULL, 0) == 1 && out[0] == DL_ST_BAD_ARG);
	uint8_t bad[2] = { DL_RX2_OP_BRIDGE, 0 };
	CHECK(cmd(bad, 2) == 1 && out[0] == DL_ST_BAD_ARG);
	uint8_t unk = 9;
	CHECK(cmd(&unk, 1) == 1 && out[0] == DL_ST_BAD_ARG);
}

static void test_forward(void)
{
	reset();
	up = true;
	uint8_t req[4] = { DL_RX2_OP_FORWARD, 0x05, 0x01, 0x02 };	// GET_VIEW-like: command and payload
	fwd_reply = 3;
	CHECK(cmd(req, 4) == 4 && out[0] == DL_ST_OK && out[1] == 0xB0 && out[3] == 0xB2);
	CHECK(fwd_len == 3 && fwd_req[0] == 0x05 && fwd_req[2] == 0x02);
	CHECK(cmd(req, 2) == 4 && fwd_len == 1);		// a command without payload
	// The reply cut to the caller's room, A's status counted
	fwd_reply = 100;
	CHECK(cmd(req, 2) == sizeof out && out[0] == DL_ST_OK);
	// No answer, no command, no second SoC or no FORWARD
	fwd_reply = 0;
	CHECK(cmd(req, 2) == 1 && out[0] == DL_ST_UNAVAILABLE);
	CHECK(cmd(req, 1) == 1 && out[0] == DL_ST_BAD_ARG);
	rx2.forward = NULL;
	CHECK(cmd(req, 2) == 1 && out[0] == DL_ST_UNAVAILABLE);
	board.rx2 = NULL;
	CHECK(cmd(req, 2) == 1 && out[0] == DL_ST_UNAVAILABLE);
}

int main(void)
{
	test_info();
	test_bridge();
	test_restart();
	test_unavailable();
	test_forward();
	printf("debuglinkRx2Test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
