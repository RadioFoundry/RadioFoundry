// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The debug link's HWOPT command (core/debuglink/debuglink_hwopt.c, docs/spec/debuglink.md 3.23) against a fake board
// with two hardware options: the list text, a stored choice, the refusals (unknown option or choice, the board
// refusing, the radio transmitting); built again with HWOPT_NONE_BOARD for a board without options (hwoptTestNone).
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "radio/radio.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint8_t radio_state = RS_IDLE;

void radio_get_view(radio_view_t *v)
{
	memset(v, 0, sizeof *v);
	v->state = radio_state;
}

static int8_t rev = HWOPT_NONE;
static bool arm, arm_ok = true;

static int8_t rev_get(void) { return rev; }
static bool rev_set(uint8_t c) { rev = (int8_t)c; return true; }
static int8_t arm_get(void) { return arm; }
static bool arm_set(uint8_t c) { if (!arm_ok) return false; arm = c != 0; return true; }

static const char *const revs[] = { "V1-V3", "V4", "V4A/V5" };
static const char *const arms[] = { "Off", "Armed" };
static const hal_hwopt_t opts[] = {
	{ "Board revision", 3, revs, rev_get, rev_set, true },
	{ "Transmit", 2, arms, arm_get, arm_set, false },
};

#ifdef HWOPT_NONE_BOARD
const board_t board = { .name = "test" };
#else
const board_t board = { .name = "test", .hwopts = opts, .hwopt_count = 2 };
#endif

static uint8_t out[1100];

static size_t cmd(const uint8_t *req, uint16_t len)
{
	memset(out, 0, sizeof out);
	return debuglink_cmd_hwopt(req, len, out, sizeof out);
}

static bool has(const char *s)
{
	return strstr((const char *)out + 1, s) != NULL;
}

int main(void)
{
	const uint8_t list[] = { DL_HWOPT_OP_LIST };
	const uint8_t set_v5[] = { DL_HWOPT_OP_SET, 0, 2 };
#ifdef HWOPT_NONE_BOARD
	(void)opts;
	cmd(list, 1);
	CHECK(out[0] == DL_ST_OK && has("hwopt.count=0\n") && !has("hwopt.0."));
	CHECK(cmd(set_v5, 3) == 1 && out[0] == DL_ST_BAD_ARG);
#else
	size_t n = cmd(list, 1);
	CHECK(n > 1 && out[0] == DL_ST_OK);
	CHECK(has("hwopt.count=2\n") && has("hwopt.0.name=Board revision\n") && has("hwopt.0.value=-1\n"));
	CHECK(has("hwopt.0.restart=1\n") && has("hwopt.0.choices=V1-V3|V4|V4A/V5\n") && has("hwopt.1.restart=0\n"));
	CHECK(has("hwopt.1.choices=Off|Armed\n"));

	CHECK(cmd(set_v5, 3) == 1 && out[0] == DL_ST_OK && rev == 2);
	cmd(list, 1);
	CHECK(has("hwopt.0.value=2\n"));

	// Unknown option or choice, a bad length, an unknown operation
	const uint8_t bad_opt[] = { DL_HWOPT_OP_SET, 2, 0 }, bad_choice[] = { DL_HWOPT_OP_SET, 1, 2 };
	CHECK(cmd(bad_opt, 3) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(bad_choice, 3) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(set_v5, 2) == 1 && out[0] == DL_ST_BAD_ARG);
	const uint8_t op9[] = { 9 };
	CHECK(cmd(op9, 1) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(list, 0) == 1 && out[0] == DL_ST_BAD_ARG);

	// The board refuses; the radio transmits
	const uint8_t arm_on[] = { DL_HWOPT_OP_SET, 1, 1 };
	arm_ok = false;
	CHECK(cmd(arm_on, 3) == 1 && out[0] == DL_ST_REFUSED && !arm);
	arm_ok = true;
	radio_state = RS_TX;
	CHECK(cmd(arm_on, 3) == 1 && out[0] == DL_ST_REFUSED && !arm);
	radio_state = RS_IDLE;
	CHECK(cmd(arm_on, 3) == 1 && out[0] == DL_ST_OK && arm);
#endif

	printf("hwoptTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
