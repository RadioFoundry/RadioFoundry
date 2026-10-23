// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The input gesture engine (core/input/input_engine.c): press, long, repeat, raw edges, encoder, held keys
#include <stdio.h>
#include <string.h>
#include "input/input_engine.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

enum { K_OK, K_UP, K_PTT, K_MON, K_SIDE, K_TOP, K_5, K_STAR };

static const keymap_entry_t keymap[] = {
	{ K_OK, ACT_SELECT, ACT_QUICK, false, false, 0 },
	{ K_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ K_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
	{ K_MON, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ K_SIDE, ACT_USER_1, ACT_NONE, false, false, 0 },
	{ K_TOP, ACT_USER_3, ACT_EMERGENCY, false, false, 0 },
	{ K_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false, 0 },
	{ K_STAR, ACT_QUICK, ACT_QUICK, false, false, '*' },
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

// Emergency is hold to activate: its key's long action needs INPUT_HOLD_MS, not the long press time
static void test_emergency_hold(void)
{
	reset(0);
	keys = 1u << K_TOP;
	run(INPUT_HOLD_MS - 20, 0);
	CHECK(nev == 0);
	run(40, 0);
	CHECK(nev == 1 && evs[0].action == ACT_EMERGENCY && evs[0].gesture == GESTURE_LONG);
	keys = 0;
	run(30, 0);
	CHECK(nev == 1);

	// Let go between the long press time and the hold: the short action, never emergency
	reset(0);
	keys = 1u << K_TOP;
	run(INPUT_LONG_MS + 100, 0);
	keys = 0;
	run(30, 0);
	CHECK(count(ACT_EMERGENCY, GESTURE_LONG) == 0);
	CHECK(input_long_threshold(ACT_EMERGENCY, 2000) == 2000 && input_long_threshold(ACT_QUICK, 700) == 700);
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

// DTMF keys (radio-features.md 3.7): their edges go to the callback; a hold it takes sends no gestures
static bool dtmf_take;
static char dtmf_log[16];
static int dtmf_n;
static bool dtmf_cb(char digit, bool down, void *ctx)
{
	(void)ctx;
	if (dtmf_n < 15) {
		dtmf_log[dtmf_n++] = down ? digit : (char)(digit | 0x80);	// bit 7: an up
	}
	return dtmf_take;
}

static void test_dtmf(void)
{
	CHECK(input_dtmf_digit(&keymap[K_5]) == '5' && input_dtmf_digit(&keymap[K_STAR]) == '*');
	CHECK(input_dtmf_digit(&keymap[K_OK]) == 0 && input_dtmf_digit(&keymap[K_PTT]) == 0);
	// No callback: a digit key is a digit key
	reset(0);
	keys = 1u << K_5;
	run(100, 0);
	keys = 0;
	run(30, 0);
	CHECK(nev == 1 && evs[0].action == ACT_DIGIT_5 && evs[0].gesture == GESTURE_PRESS);
	// Taken: down and up to the callback, no PRESS, no LONG however long it is held
	reset(0);
	input_engine_set_dtmf(&e, dtmf_cb, NULL);
	dtmf_take = true;
	dtmf_n = 0;
	keys = 1u << K_5;
	run(30, 0);
	CHECK(dtmf_n == 1 && dtmf_log[0] == '5');
	run(INPUT_LONG_MS + 200, 0);
	keys = 0;
	run(30, 0);
	CHECK(nev == 0 && dtmf_n == 2 && dtmf_log[1] == (char)('5' | 0x80));
	// * with its own digit, while OK beside it keeps its gestures and never asks
	keys = (1u << K_STAR) | (1u << K_OK);
	run(100, 0);
	keys = 0;
	run(30, 0);
	CHECK(dtmf_n == 4 && dtmf_log[2] == '*' && dtmf_log[3] == (char)('*' | 0x80));
	CHECK(nev == 1 && evs[0].action == ACT_SELECT && evs[0].gesture == GESTURE_PRESS);
	// Raw-edge keys never ask
	keys = 1u << K_PTT;
	run(30, 0);
	keys = 0;
	run(30, 0);
	CHECK(dtmf_n == 4 && count(ACT_PTT, GESTURE_DOWN) == 1);
	// Refused: the key's gestures as usual, and its up isn't sent
	dtmf_take = false;
	nev = 0;
	keys = 1u << K_STAR;
	run(INPUT_LONG_MS + 100, 0);
	keys = 0;
	run(30, 0);
	CHECK(dtmf_n == 5 && dtmf_log[4] == '*' && count(ACT_QUICK, GESTURE_LONG) == 1);
	// Taken, then the callback goes away mid-hold: the hold still ends quietly
	dtmf_take = true;
	nev = 0;
	keys = 1u << K_5;
	run(30, 0);
	input_engine_set_dtmf(&e, NULL, NULL);
	run(INPUT_LONG_MS, 0);
	keys = 0;
	run(30, 0);
	CHECK(nev == 0 && dtmf_n == 6);
}

// One input task pass over injected keys: the scan runs at now, late if it waited for a shared bus
static void inj_pass(input_inj_state_t *s)
{
	input_engine_step(&e, keys | input_inj_scan(s, now), 0, now, emit, NULL);
}

// An injected PRESS (80 ms) whose scans stall behind a 150 ms LCD redraw still gives its press: it is held until the
// debounce has seen it, not only until its time. And the next one waits for scans that see it released
static void test_inject_late_scans(void)
{
	input_inj_state_t s = { .idle = INPUT_INJECT_MIN_SCANS };
	reset(0);
	CHECK(input_inj_ready(&s, now, INPUT_INJECT_GAP_MS));
	input_inj_hold(&s, 1u << K_OK, now + INPUT_INJECT_PRESS_MS);
	CHECK(!input_inj_ready(&s, now, INPUT_INJECT_GAP_MS));
	now += 150;		// the first scan after the hold started, already past its end
	inj_pass(&s);
	CHECK(s.active);	// one scan saw it: not released yet
	int passes = 0;
	while (s.active && passes < 10) {
		now += INPUT_SCAN_MS;
		inj_pass(&s);
		passes++;
	}
	CHECK(passes == INPUT_INJECT_MIN_SCANS);	// two more scans saw it held, the third released it
	CHECK(s.idle == 1);

	// Released, then the scans stall again: the gap has passed in ms, but the debounce hasn't seen the key up twice
	now = s.released_ms + 2 * INPUT_INJECT_GAP_MS;
	CHECK(!input_inj_ready(&s, now, INPUT_INJECT_GAP_MS));
	inj_pass(&s);
	CHECK(!input_inj_ready(&s, now, INPUT_INJECT_GAP_MS));
	inj_pass(&s);
	CHECK(input_inj_ready(&s, now, INPUT_INJECT_GAP_MS));
	CHECK(count(ACT_SELECT, GESTURE_PRESS) == 1);
	// The usual case: scans on time, released at its end
	input_inj_hold(&s, 1u << K_OK, now + INPUT_INJECT_PRESS_MS);
	uint32_t until = s.until;
	while (s.active) {
		now += INPUT_SCAN_MS;
		inj_pass(&s);
	}
	CHECK(s.released_ms == until);
	run(50, 0);
	CHECK(count(ACT_SELECT, GESTURE_PRESS) == 2);
}

int main(void)
{
	test_press();
	test_bounce();
	test_long();
	test_emergency_hold();
	test_repeat();
	test_timing();
	test_raw_edges();
	test_chord();
	test_held_at_start();
	test_encoder_idle();
	test_dtmf();
	test_inject_late_scans();
	printf("# inputTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
