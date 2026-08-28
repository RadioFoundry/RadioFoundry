// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The input gesture engine (core/input/input_engine.c): press, long, repeat, raw edges, encoder, held keys
#include <stdio.h>
#include <string.h>
#include "input/input_engine.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

enum { K_OK, K_UP, K_PTT, K_MON, K_SIDE };

static const keymap_entry_t keymap[] = {
	{ K_OK, ACT_SELECT, ACT_QUICK, false, false },
	{ K_UP, ACT_NAV_UP, ACT_NAV_UP, false, true },
	{ K_PTT, ACT_PTT, ACT_PTT, true, false },
	{ K_MON, ACT_MONITOR, ACT_MONITOR, true, false },
	{ K_SIDE, ACT_USER_1, ACT_NONE, false, false },
};

#define MAXEV 128
static input_event_t evs[MAXEV];
static int nev;
static input_engine_t e;
static uint32_t now;
static uint32_t keys;

static void emit(const input_event_t *ev, void *ctx)
{
	(void)ctx;
	if (nev < MAXEV) {
		evs[nev++] = *ev;
	}
}

static void run(uint32_t ms, int8_t enc)
{
	for (uint32_t t = 0; t < ms; t += INPUT_SCAN_MS) {
		now += INPUT_SCAN_MS;
		input_engine_step(&e, keys, enc, now, emit, NULL);
		enc = 0;
	}
}

static void reset(uint32_t held_at_start)
{
	nev = 0;
	now = 5000;
	keys = held_at_start;
	input_engine_init(&e, keymap, sizeof(keymap) / sizeof(keymap[0]), now);
	run(INPUT_SCAN_MS, 0);
}

static int count(uint8_t action, uint8_t gesture)
{
	int n = 0;
	for (int i = 0; i < nev; i++) {
		if (evs[i].action == action && evs[i].gesture == gesture) {
			n++;
		}
	}
	return n;
}

static void test_press(void)
{
	reset(0);
	keys = 1u << K_OK;
	run(200, 0);
	CHECK(nev == 0);			// nothing until release
	keys = 0;
	run(30, 0);
	CHECK(nev == 1 && evs[0].action == ACT_SELECT && evs[0].gesture == GESTURE_PRESS);
}

static void test_bounce(void)
{
	reset(0);
	keys = 1u << K_OK;			// one-sample glitch: ignored
	run(INPUT_SCAN_MS, 0);
	keys = 0;
	run(50, 0);
	CHECK(nev == 0);
}

static void test_long(void)
{
	reset(0);
	keys = 1u << K_OK;
	run(INPUT_LONG_MS - 20, 0);
	CHECK(nev == 0);
	run(40, 0);
	CHECK(nev == 1 && evs[0].action == ACT_QUICK && evs[0].gesture == GESTURE_LONG);
	run(1000, 0);
	CHECK(nev == 1);			// once, and no repeat on this key
	keys = 0;
	run(30, 0);
	CHECK(nev == 1);			// no PRESS after a LONG

	// A key with no long action: a long hold gives nothing
	reset(0);
	keys = 1u << K_SIDE;
	run(1000, 0);
	keys = 0;
	run(30, 0);
	CHECK(nev == 0);
}

static void test_repeat(void)
{
	reset(0);
	keys = 1u << K_UP;
	run(INPUT_LONG_MS + 20, 0);	// the debounce adds one scan
	CHECK(count(ACT_NAV_UP, GESTURE_LONG) == 1);
	CHECK(count(ACT_NAV_UP, GESTURE_REPEAT) == 0);
	run(INPUT_REPEAT_MS * 5, 0);
	int r = count(ACT_NAV_UP, GESTURE_REPEAT);
	CHECK(r >= 4 && r <= 5);
	keys = 0;
	run(30, 0);
	CHECK(count(ACT_NAV_UP, GESTURE_PRESS) == 0);
}

// Settings > Keys: a longer long press and a slower repeat (input_engine_set_timing); 0 is the defaults again
static void test_timing(void)
{
	reset(0);
	input_engine_set_timing(&e, 900, 200);
	keys = 1u << K_UP;
	run(INPUT_LONG_MS + 100, 0);
	CHECK(count(ACT_NAV_UP, GESTURE_LONG) == 0);
	run(900 - INPUT_LONG_MS, 0);
	CHECK(count(ACT_NAV_UP, GESTURE_LONG) == 1);
	run(200 * 5, 0);
	int r = count(ACT_NAV_UP, GESTURE_REPEAT);
	CHECK(r >= 4 && r <= 5);
	keys = 0;
	run(30, 0);
	reset(0);
	input_engine_set_timing(&e, 0, 0);
	keys = 1u << K_OK;
	run(INPUT_LONG_MS + 20, 0);
	CHECK(nev == 1 && evs[0].gesture == GESTURE_LONG);
}

static void test_raw_edges(void)
{
	reset(0);
	keys = 1u << K_PTT;
	run(2000, 0);
	CHECK(nev == 1 && evs[0].action == ACT_PTT && evs[0].gesture == GESTURE_DOWN);
	keys = 0;
	run(30, 0);
	CHECK(nev == 2 && evs[1].action == ACT_PTT && evs[1].gesture == GESTURE_UP);
}

static void test_chord(void)
{
	reset(0);
	keys = 1u << K_MON;
	run(50, 0);
	keys |= 1u << K_PTT;
	run(50, 0);
	keys |= 1u << K_UP;
	run(50, 0);
	keys &= ~(1u << K_UP);
	run(50, 0);
	keys = 0;
	run(50, 0);
	CHECK(count(ACT_MONITOR, GESTURE_DOWN) == 1 && count(ACT_MONITOR, GESTURE_UP) == 1);
	CHECK(count(ACT_PTT, GESTURE_DOWN) == 1 && count(ACT_PTT, GESTURE_UP) == 1);
	CHECK(count(ACT_NAV_UP, GESTURE_PRESS) == 1);
	CHECK(nev == 5);
}

static void test_held_at_start(void)
{
	reset(1u << K_PTT);			// PTT held at power-on: ignored until released
	run(1000, 0);
	CHECK(nev == 0);
	keys = 0;
	run(50, 0);
	CHECK(nev == 0);
	keys = 1u << K_PTT;
	run(50, 0);
	CHECK(nev == 1 && evs[0].gesture == GESTURE_DOWN);
}

static void test_encoder_idle(void)
{
	reset(0);
	run(1000, 0);
	CHECK(now - e.last_activity_ms >= 990);
	run(INPUT_SCAN_MS, -2);
	CHECK(nev == 1 && evs[0].action == ACT_ENC_STEP && evs[0].gesture == GESTURE_DOWN && evs[0].value == -2);
	CHECK(e.last_activity_ms == now);
	run(500, 0);
	keys = 1u << K_OK;
	run(3000, 0);
	CHECK(now - e.last_activity_ms == 0);	// holding a key counts as activity
}

int main(void)
{
	test_press();
	test_bounce();
	test_long();
	test_repeat();
	test_timing();
	test_raw_edges();
	test_chord();
	test_held_at_start();
	test_encoder_idle();
	printf("# inputTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
