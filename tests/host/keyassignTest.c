// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Side key assignment (core/input/keyassign.c, docs/spec/ui.md 3.1): the quick-key slots with and without our marker,
// what each slot may hold, and the key map built from them, run through the gesture engine
#include <stdio.h>
#include <string.h>
#include "input/keyassign.h"
#include "input/input_engine.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

enum { K_OK, K_SK1, K_SK2, K_TOP, K_PTT };

// The DM-1701's side keys: SK1 monitor (held), SK2 talk target / scan, top power / emergency
static const keymap_entry_t base[] = {
	{ K_OK, ACT_SELECT, ACT_SELECT, false, false },
	{ K_SK1, ACT_MONITOR, ACT_MONITOR, true, false },
	{ K_SK2, ACT_TALK_TARGET, ACT_USER_2, false, false },
	{ K_TOP, ACT_USER_3, ACT_EMERGENCY, false, false },
	{ K_PTT, ACT_PTT, ACT_PTT, true, false },
};
#define N	(sizeof(base) / sizeof(base[0]))
static const uint8_t side[] = { K_SK1, K_SK2, K_TOP };

static keymap_entry_t map[N];

static const keymap_entry_t *entry(uint8_t bit)
{
	for (unsigned i = 0; i < N; i++) {
		if (map[i].bit == bit) {
			return &map[i];
		}
	}
	return NULL;
}

static void test_marker(void)
{
	uint16_t s[KEYASSIGN_SLOTS];
	// Another firmware's slots (no marker): every key as the board has it
	for (unsigned i = 0; i < KEYASSIGN_SLOTS; i++) {
		s[i] = (uint16_t)(0x0100u + i);
	}
	s[1] = KEYFN_POWER;
	CHECK(keyassign_get(s, 0, false) == KEYFN_DEFAULT);
	keyassign_build(base, N, side, 3, s, map);
	CHECK(memcmp(map, base, sizeof(map)) == 0);
	// The first change writes the marker and clears the foreign slots
	keyassign_set(s, 1, true, KEYFN_SCAN);
	CHECK(s[0] == KEYASSIGN_MARKER && s[4] == KEYFN_SCAN);
	for (unsigned i = 1; i < KEYASSIGN_SLOTS; i++) {
		CHECK(i == 4 || s[i] == 0);
	}
	CHECK(keyassign_get(s, 1, true) == KEYFN_SCAN && keyassign_get(s, 0, false) == KEYFN_DEFAULT);
	// An unknown value reads as the default; a role past the last one is ignored
	s[1] = 999;
	CHECK(keyassign_get(s, 0, false) == KEYFN_DEFAULT);
	keyassign_set(s, KEYASSIGN_ROLES, false, KEYFN_POWER);
	CHECK(s[7] == 0 && keyassign_get(s, KEYASSIGN_ROLES, false) == KEYFN_DEFAULT);
}

static void test_allowed(void)
{
	uint16_t s[KEYASSIGN_SLOTS] = { 0 };
	CHECK(keyassign_allowed(KEYFN_MONITOR, false) && !keyassign_allowed(KEYFN_MONITOR, true));
	CHECK(!keyassign_allowed(KEYFN_EMERGENCY, false) && keyassign_allowed(KEYFN_EMERGENCY, true));
	CHECK(!keyassign_allowed(KEYFN_COUNT, false));
	// Emergency on a press would go off by accident; monitor on a long press can't be held
	keyassign_set(s, 0, false, KEYFN_EMERGENCY);
	keyassign_set(s, 0, true, KEYFN_MONITOR);
	CHECK(s[1] == KEYFN_DEFAULT && s[2] == KEYFN_DEFAULT);
	s[1] = KEYFN_EMERGENCY;			// written by a tool: still refused
	CHECK(keyassign_get(s, 0, false) == KEYFN_DEFAULT);
}

static void test_build(void)
{
	uint16_t s[KEYASSIGN_SLOTS] = { 0 };
	// SK1 (a monitor key) gets a press: no longer held, its long press is nothing until set
	keyassign_set(s, 0, false, KEYFN_POWER);
	keyassign_build(base, N, side, 3, s, map);
	CHECK(!entry(K_SK1)->raw_edges && entry(K_SK1)->short_action == ACT_POWER &&
	      entry(K_SK1)->long_action == ACT_NONE);
	keyassign_set(s, 0, true, KEYFN_ZONE_UP);
	keyassign_build(base, N, side, 3, s, map);
	CHECK(entry(K_SK1)->long_action == ACT_ZONE_UP);
	// The monitor's hold ignores a long press set for it
	keyassign_set(s, 0, false, KEYFN_DEFAULT);
	keyassign_build(base, N, side, 3, s, map);
	CHECK(entry(K_SK1)->raw_edges && entry(K_SK1)->short_action == ACT_MONITOR);
	// SK2: only the long press changes, its press stays the board's
	keyassign_set(s, 1, true, KEYFN_ZONE_DOWN);
	keyassign_build(base, N, side, 3, s, map);
	CHECK(entry(K_SK2)->short_action == ACT_TALK_TARGET && entry(K_SK2)->long_action == ACT_ZONE_DOWN);
	// The top key becomes a monitor key; its long press goes with it
	keyassign_set(s, 2, false, KEYFN_MONITOR);
	keyassign_build(base, N, side, 3, s, map);
	CHECK(entry(K_TOP)->raw_edges && entry(K_TOP)->short_action == ACT_MONITOR &&
	      entry(K_TOP)->long_action == ACT_MONITOR);
	// Nothing: the key is silent
	keyassign_set(s, 2, false, KEYFN_NONE);
	keyassign_set(s, 2, true, KEYFN_NONE);
	keyassign_build(base, N, side, 3, s, map);
	CHECK(!entry(K_TOP)->raw_edges && entry(K_TOP)->short_action == ACT_NONE &&
	      entry(K_TOP)->long_action == ACT_NONE);
	// A board with two side keys: the top key's slots don't touch anything
	keyassign_build(base, N, side, 2, s, map);
	CHECK(memcmp(entry(K_TOP), &base[3], sizeof(base[3])) == 0);
	// The other keys are never changed
	CHECK(memcmp(entry(K_OK), &base[0], sizeof(base[0])) == 0 && memcmp(entry(K_PTT), &base[4], sizeof(base[4])) == 0);
}

// The functions of side key actions 11 (hear all, monitor latch, key lock, call back, home zone)
static void test_toggles(void)
{
	static const struct { uint8_t fn; uint8_t value; uint8_t action; } t[] = {
		{ KEYFN_KEY_LOCK, 11, ACT_LOCK }, { KEYFN_CALL_BACK, 12, ACT_CALL_BACK }, { KEYFN_HOME_ZONE, 13, ACT_HOME_ZONE },
		{ KEYFN_COPY_VFO, 14, ACT_COPY_VFO }, { KEYFN_DUAL_WATCH, 15, ACT_DUAL_WATCH }, { KEYFN_AB, 16, ACT_AB },
	};
	// Stored in the codeplug's slots: append only, so every value that shipped keeps its number
	CHECK(KEYFN_DEFAULT == 0 && KEYFN_NONE == 1 && KEYFN_MONITOR == 2 && KEYFN_TALK_TARGET == 3 &&
	      KEYFN_POWER == 4 && KEYFN_ZONE_UP == 5 && KEYFN_ZONE_DOWN == 6 && KEYFN_SCAN == 7 && KEYFN_EMERGENCY == 8);
	CHECK(KEYFN_HEAR_ALL == 9 && KEYFN_MONITOR_LATCH == 10 && KEYFN_COUNT == 17);
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
		uint16_t s[KEYASSIGN_SLOTS] = { 0 };
		CHECK(t[i].fn == t[i].value);
		// Allowed in a press and in a long press (only monitor is press only, only emergency long only)
		CHECK(keyassign_allowed(t[i].fn, false) && keyassign_allowed(t[i].fn, true));
		for (uint8_t role = 0; role < KEYASSIGN_ROLES; role++) {
			keyassign_set(s, role, false, t[i].fn);
			keyassign_set(s, role, true, t[i].fn);
			CHECK(keyassign_get(s, role, false) == t[i].fn && keyassign_get(s, role, true) == t[i].fn);
		}
		keyassign_build(base, N, side, 3, s, map);
		// Each role's key sends the function's own action on both gestures, and is not held, so a long press exists
		for (uint8_t role = 0; role < 3; role++) {
			const keymap_entry_t *m = entry(side[role]);
			CHECK(m->short_action == t[i].action && m->long_action == t[i].action && !m->raw_edges &&
			      !m->repeat);
		}
	}
	// A value one past the last is not a function: it reads as the default
	uint16_t s[KEYASSIGN_SLOTS] = { KEYASSIGN_MARKER, KEYFN_COUNT, KEYFN_COUNT };
	CHECK(keyassign_get(s, 0, false) == KEYFN_DEFAULT && keyassign_get(s, 0, true) == KEYFN_DEFAULT);
	// The retired monitor latch reads as the default: the monitor key stays held
	uint16_t m[KEYASSIGN_SLOTS] = { 0 };
	keyassign_set(m, 0, false, KEYFN_MONITOR_LATCH);
	keyassign_build(base, N, side, 3, m, map);
	CHECK(entry(K_SK1)->raw_edges && entry(K_SK1)->short_action == ACT_MONITOR);
}

// The built map in the gesture engine
static input_event_t evs[32];
static int nev;
static void emit(const input_event_t *ev, void *ctx)
{
	(void)ctx;
	if (nev < 32) {
		evs[nev++] = *ev;
	}
}

static void hold(input_engine_t *e, uint32_t *now, uint32_t mask, uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += INPUT_SCAN_MS) {
		*now += INPUT_SCAN_MS;
		input_engine_step(e, mask, 0, *now, emit, NULL);
	}
	for (uint32_t t = 0; t < 30; t += INPUT_SCAN_MS) {
		*now += INPUT_SCAN_MS;
		input_engine_step(e, 0, 0, *now, emit, NULL);
	}
}

static void test_engine(void)
{
	uint16_t s[KEYASSIGN_SLOTS] = { 0 };
	input_engine_t e;
	uint32_t now = 1000;
	keyassign_set(s, 0, false, KEYFN_SCAN);
	keyassign_set(s, 0, true, KEYFN_EMERGENCY);
	keyassign_set(s, 2, false, KEYFN_MONITOR);
	keyassign_build(base, N, side, 3, s, map);
	input_engine_init(&e, map, N, now);
	input_engine_step(&e, 0, 0, now, emit, NULL);
	nev = 0;
	hold(&e, &now, 1u << K_SK1, 100);
	CHECK(nev == 1 && evs[0].action == ACT_SCAN && evs[0].gesture == GESTURE_PRESS);
	// Emergency on a long press still needs the hold time, whatever the long press time
	nev = 0;
	hold(&e, &now, 1u << K_SK1, 1000);
	CHECK(nev == 1 && evs[0].action == ACT_SCAN);
	nev = 0;
	hold(&e, &now, 1u << K_SK1, INPUT_HOLD_MS + 50);
	CHECK(nev == 1 && evs[0].action == ACT_EMERGENCY && evs[0].gesture == GESTURE_LONG);
	// The top key as monitor: down and up edges
	nev = 0;
	hold(&e, &now, 1u << K_TOP, 2000);
	CHECK(nev == 2 && evs[0].action == ACT_MONITOR && evs[0].gesture == GESTURE_DOWN &&
	      evs[1].gesture == GESTURE_UP);
	// The toggles (key lock, call back) go in either slot and send their own action
	keyassign_set(s, 1, false, KEYFN_KEY_LOCK);
	keyassign_set(s, 1, true, KEYFN_CALL_BACK);
	CHECK(keyassign_get(s, 1, false) == KEYFN_KEY_LOCK && keyassign_get(s, 1, true) == KEYFN_CALL_BACK);
	keyassign_build(base, N, side, 3, s, map);
	input_engine_init(&e, map, N, now);
	input_engine_step(&e, 0, 0, now, emit, NULL);
	nev = 0;
	hold(&e, &now, 1u << K_SK2, 100);
	CHECK(nev == 1 && evs[0].action == ACT_LOCK && evs[0].gesture == GESTURE_PRESS);
	nev = 0;
	hold(&e, &now, 1u << K_SK2, 2000);
	CHECK(nev == 1 && evs[0].action == ACT_CALL_BACK && evs[0].gesture == GESTURE_LONG);
	CHECK(!keyassign_allowed(KEYFN_MONITOR_LATCH, true) && !keyassign_allowed(KEYFN_HEAR_ALL, false) &&
	      keyassign_allowed(KEYFN_KEY_LOCK, false));
	// Monitor with a hold function (#19): DOWN and UP as before, plus the long press (value 1), and the UP says so
	keyassign_set(s, 2, false, KEYFN_MONITOR);
	keyassign_set(s, 2, true, KEYFN_POWER);
	keyassign_build(base, N, side, 3, s, map);
	input_engine_init(&e, map, N, now);
	input_engine_step(&e, 0, 0, now, emit, NULL);
	nev = 0;
	hold(&e, &now, 1u << K_TOP, 100);
	CHECK(nev == 2 && evs[0].action == ACT_MONITOR && evs[0].gesture == GESTURE_DOWN &&
	      evs[1].gesture == GESTURE_UP && evs[1].value == 0);
	nev = 0;
	hold(&e, &now, 1u << K_TOP, 2000);
	CHECK(nev == 3 && evs[0].gesture == GESTURE_DOWN && evs[1].action == ACT_POWER &&
	      evs[1].gesture == GESTURE_LONG && evs[1].value == 1 && evs[2].action == ACT_MONITOR &&
	      evs[2].gesture == GESTURE_UP && evs[2].value == 1);
}

// A board key map entry whose short press is the VFO / channel toggle (ACT_VFO_TOGGLE) has Copy to VFO on its long
// press (docs/ux/information-architecture.md 2). Read from the boards' sources: they don't build on the host
static void test_board_defaults(void)
{
	static const char *const boards[] = { BOARD_IDS };	// the Makefile lists boards/*/board.c
	int toggles = 0;
	for (unsigned b = 0; b < sizeof boards / sizeof boards[0]; b++) {
		char path[64];
		snprintf(path, sizeof path, "../../boards/%s/board.c", boards[b]);
		FILE *f = fopen(path, "r");
		CHECK(f != NULL);
		if (f == NULL) {
			continue;
		}
		char line[256];
		while (fgets(line, sizeof line, f) != NULL) {
			if (strstr(line, "{ KEY_") == NULL || strstr(line, "ACT_") == NULL) {
				continue;
			}
			const char *comma = strchr(line, ',');	// after the key; the short action follows
			if (comma != NULL && strncmp(comma + 2, "ACT_VFO_TOGGLE", 14) == 0) {
				toggles++;
				CHECK(strstr(comma + 2, ", ACT_COPY_VFO,") != NULL);
			}
			CHECK(strstr(line, "ACT_COPY_VFO") == NULL || strstr(line, "ACT_VFO_TOGGLE") != NULL);
		}
		fclose(f);
	}
	CHECK(toggles >= 1);	// at least one board has the key, so the check reads something
}

int main(void)
{
	test_marker();
	test_allowed();
	test_build();
	test_toggles();
	test_engine();
	test_board_defaults();
	printf("# keyassignTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
