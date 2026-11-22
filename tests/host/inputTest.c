// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The input gesture engine (core/input/input_engine.c): press, long, repeat, raw edges, encoder, held keys
#include <stdio.h>
#include <string.h>
#include "input/input_engine.h"
#include "fuzz.h"

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

// The time from the first scan that sees the key held to the given gesture's first event of action (-1: none)
static int32_t first_at(uint32_t down_at, uint8_t action, uint8_t gesture, const uint32_t *at)
{
	for (int i = 0; i < nev; i++) {
		if (evs[i].action == action && evs[i].gesture == gesture) {
			return (int32_t)(at[i] - down_at);
		}
	}
	return -1;
}

// Emergency's long press comes at INPUT_EMERGENCY_SHOW_MS whatever the long press time, and its release after it is an
// UP of ACT_EMERGENCY (value 1) for the UI's countdown; a shorter press is the key's own press, never emergency.
// Partitions: release before the threshold / at or after it; long press time below / equal / above the threshold;
// plain and raw-edge keys. Boundaries: one scan either side of the threshold. Then a seeded run of random holds
static uint32_t ev_at[MAXEV];
static void emit_at(const input_event_t *ev, void *ctx)
{
	(void)ctx;
	if (nev < MAXEV) {
		ev_at[nev] = now;
		evs[nev++] = *ev;
	}
}

// Hold the top key ms (counted from the scan that debounces it), then let go: the events are in evs
static void hold_top(const keymap_entry_t *map, uint8_t n, uint16_t long_ms, uint32_t ms, uint8_t bit)
{
	nev = 0;
	now = 5000;
	input_engine_init(&e, map, n, now);
	input_engine_set_timing(&e, long_ms, 0);
	input_engine_step(&e, 0, 0, now, emit_at, NULL);
	now += INPUT_SCAN_MS;
	input_engine_step(&e, 1u << bit, 0, now, emit_at, NULL);	// first sample
	for (uint32_t t = 0; t < ms; t += INPUT_SCAN_MS) {		// debounced at the first of these
		now += INPUT_SCAN_MS;
		input_engine_step(&e, 1u << bit, 0, now, emit_at, NULL);
	}
	for (int i = 0; i < 3; i++) {
		now += INPUT_SCAN_MS;
		input_engine_step(&e, 0, 0, now, emit_at, NULL);
	}
}

static int emergency_checks;
#define ECHECK(c) do { emergency_checks++; CHECK(c); } while (0)

static void test_emergency_hold(void)
{
	static const keymap_entry_t map[] = { { K_TOP, ACT_USER_3, ACT_EMERGENCY, false, false, 0 } };
	// A monitor key whose hold is emergency (keyassign.c): raw edges, and the long press marked value 1
	static const keymap_entry_t raw[] = { { K_TOP, ACT_MONITOR, ACT_EMERGENCY, true, false, 0 } };
	static const uint16_t long_ms[] = { 200, INPUT_EMERGENCY_SHOW_MS, INPUT_LONG_MS, 1200 };
	const uint32_t edge = INPUT_EMERGENCY_SHOW_MS - INPUT_SCAN_MS;	// held this long after the debounce: one scan short

	for (size_t l = 0; l < sizeof long_ms / sizeof long_ms[0]; l++) {
		// One scan short of the threshold: the press, nothing of emergency
		hold_top(map, 1, long_ms[l], edge, K_TOP);
		ECHECK(nev == 1 && evs[0].action == ACT_USER_3 && evs[0].gesture == GESTURE_PRESS);
		// At the threshold: LONG there; let go within the press window: UP value 2, then the key's press
		uint32_t win = input_press_window(ACT_EMERGENCY, long_ms[l]);
		hold_top(map, 1, long_ms[l], edge + INPUT_SCAN_MS, K_TOP);
		ECHECK(nev == 3 && evs[0].action == ACT_EMERGENCY && evs[0].gesture == GESTURE_LONG && evs[0].value == 0);
		ECHECK(evs[1].action == ACT_EMERGENCY && evs[1].gesture == GESTURE_UP && evs[1].value == 2);
		ECHECK(evs[2].action == ACT_USER_3 && evs[2].gesture == GESTURE_PRESS);
		// The window's edges (the release is debounced one scan after the last held one): one scan short, the press;
		// at the window, UP value 1 and no press
		hold_top(map, 1, long_ms[l], win - 2 * INPUT_SCAN_MS, K_TOP);
		ECHECK(nev == 3 && evs[1].value == 2 && count(ACT_USER_3, GESTURE_PRESS) == 1);
		hold_top(map, 1, long_ms[l], win - INPUT_SCAN_MS, K_TOP);
		ECHECK(nev == 2 && evs[1].gesture == GESTURE_UP && evs[1].value == 1 && count(ACT_USER_3, GESTURE_PRESS) == 0);
		// The raw-edge key: DOWN, the LONG (value 1), and at the release the monitor's UP and the emergency's UP (value
		// 1 always: its press is the raw edges themselves)
		hold_top(raw, 1, long_ms[l], edge + INPUT_SCAN_MS, K_TOP);
		ECHECK(nev == 4 && evs[0].action == ACT_MONITOR && evs[0].gesture == GESTURE_DOWN);
		ECHECK(evs[1].action == ACT_EMERGENCY && evs[1].gesture == GESTURE_LONG && evs[1].value == 1);
		ECHECK(evs[2].action == ACT_MONITOR && evs[2].gesture == GESTURE_UP && evs[2].value == 1);
		ECHECK(evs[3].action == ACT_EMERGENCY && evs[3].gesture == GESTURE_UP && evs[3].value == 1);
		hold_top(raw, 1, long_ms[l], edge, K_TOP);
		ECHECK(count(ACT_EMERGENCY, GESTURE_LONG) == 0 && count(ACT_EMERGENCY, GESTURE_UP) == 0);
	}
	ECHECK(input_long_threshold(ACT_EMERGENCY, 2000) == INPUT_EMERGENCY_SHOW_MS &&
	       input_long_threshold(ACT_QUICK, 700) == 700);
	ECHECK(input_long_up(ACT_EMERGENCY) && !input_long_up(ACT_QUICK) && !input_long_up(ACT_NONE));
	ECHECK(input_press_window(ACT_EMERGENCY, 600) == INPUT_EMERGENCY_PRESS_MS &&
	       input_press_window(ACT_EMERGENCY, 2000) == 2000);
	// Another long action keeps its silence at the release (the OK key's QUICK)
	static const keymap_entry_t ok[] = { { K_OK, ACT_SELECT, ACT_QUICK, false, false, 0 } };
	hold_top(ok, 1, 0, 1000, K_OK);
	ECHECK(nev == 1 && evs[0].action == ACT_QUICK && evs[0].gesture == GESTURE_LONG);

	// Property: for any hold and long press time, LONG comes exactly once at the threshold (to the scan) when the hold
	// reaches it, followed by one UP at the release: value 2 and the key's PRESS when let go within the press window,
	// value 1 and no PRESS after it; otherwise exactly one PRESS and no emergency event
	fuzz_t f;
	fuzz_init(&f, 0xE3E7u);
	long iters = fuzz_iters(500);
	for (long i = 0; i < iters; i++) {
		uint16_t lm = (uint16_t)(200 + fuzz_below(&f, 2801));
		uint32_t ms = INPUT_SCAN_MS * (1 + fuzz_below(&f, 400));	// at least the scan that debounces it (to 4 s)
		hold_top(map, 1, lm, ms, K_TOP);
		uint32_t down = 5000 + 2 * INPUT_SCAN_MS;	// the scan that debounces the key (the second sample)
		if (ms > edge) {
			bool press = ms + INPUT_SCAN_MS < input_press_window(ACT_EMERGENCY, lm);	// the release, debounced
			ECHECK(nev == (press ? 3 : 2) && count(ACT_EMERGENCY, GESTURE_LONG) == 1 &&
			       count(ACT_EMERGENCY, GESTURE_UP) == 1 && count(ACT_USER_3, GESTURE_PRESS) == (press ? 1 : 0));
			ECHECK(evs[1].value == (press ? 2 : 1));
			ECHECK(first_at(down, ACT_EMERGENCY, GESTURE_LONG, ev_at) == INPUT_EMERGENCY_SHOW_MS);
			ECHECK(first_at(down, ACT_EMERGENCY, GESTURE_UP, ev_at) == (int32_t)(ms + INPUT_SCAN_MS));	// the release debounced
		} else {
			ECHECK(nev == 1 && evs[0].action == ACT_USER_3 && evs[0].gesture == GESTURE_PRESS);
		}
	}
	printf("# inputTest emergency hold: %d checks (%ld random holds)\n", emergency_checks, iters);
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
