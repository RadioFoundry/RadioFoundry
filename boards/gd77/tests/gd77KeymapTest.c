// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The GD-77 key map (boards/gd77/keymap_gd77.h) against the rules every board's map follows
// (docs/ux/information-architecture.md 2, 7.3): every physical key mapped once, the required actions there, raw edges
// only for PTT and monitor, repeat only on the navigation keys, the reassignable roles on mapped keys. Built with and
// without DMR emergency (the top key's long press)
#include <stdio.h>
#include <string.h>
#include "keymap_gd77.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const keymap_entry_t *entry(uint8_t key)
{
	for (unsigned i = 0; i < GD77_KEYMAP_LEN; i++) {
		if (gd77_keymap[i].bit == key) {
			return &gd77_keymap[i];
		}
	}
	return NULL;
}

static bool has_short(uint8_t action)
{
	for (unsigned i = 0; i < GD77_KEYMAP_LEN; i++) {
		if (gd77_keymap[i].short_action == action) {
			return true;
		}
	}
	return false;
}

int main(void)
{
	unsigned seen[KEY_COUNT] = { 0 };
	for (unsigned i = 0; i < GD77_KEYMAP_LEN; i++) {
		const keymap_entry_t *e = &gd77_keymap[i];
		CHECK(e->bit < KEY_COUNT);
		if (e->bit < KEY_COUNT) {
			seen[e->bit]++;
		}
		bool edges = e->short_action == ACT_PTT || e->short_action == ACT_MONITOR;
		CHECK(e->raw_edges == edges);
		bool nav = e->short_action >= ACT_NAV_UP && e->short_action <= ACT_NAV_RIGHT;
		CHECK(!e->repeat || nav);
	}
	for (unsigned k = 0; k < KEY_COUNT; k++) {
		CHECK(seen[k] == 1);
	}

	static const uint8_t required[] = { ACT_NAV_UP, ACT_NAV_DOWN, ACT_SELECT, ACT_BACK, ACT_PTT, ACT_QUICK,
					    ACT_CONTEXT, ACT_MONITOR, ACT_TALK_TARGET };
	for (unsigned i = 0; i < sizeof required; i++) {
		CHECK(has_short(required[i]));
	}
	for (uint8_t d = ACT_DIGIT_0; d <= ACT_DIGIT_9; d++) {
		CHECK(has_short(d));
	}
	CHECK(entry(GD77_KEY_UP)->repeat && entry(GD77_KEY_DOWN)->repeat);
	CHECK(entry(KEY_STAR)->short_action == ACT_QUICK && entry(KEY_HASH)->short_action == ACT_CONTEXT);

	// The top key: the reassignable role, held for emergency only where it is built
	const keymap_entry_t *top = entry(KEY_TOP);
	CHECK(top->short_action == ACT_USER_3);
#if CONFIG_DMR_EMERGENCY
	CHECK(top->long_action == ACT_EMERGENCY);
#else
	CHECK(top->long_action == ACT_USER_3);
	CHECK(!has_short(ACT_EMERGENCY));
	for (unsigned i = 0; i < GD77_KEYMAP_LEN; i++) {
		CHECK(gd77_keymap[i].long_action != ACT_EMERGENCY);
	}
#endif

	// Settings > Keys roles: side key 1, side key 2, top key, three mapped keys
	CHECK(sizeof gd77_side_keys == 3);
	for (unsigned i = 0; i < sizeof gd77_side_keys; i++) {
		CHECK(entry(gd77_side_keys[i]) != NULL);
		for (unsigned j = i + 1; j < sizeof gd77_side_keys; j++) {
			CHECK(gd77_side_keys[i] != gd77_side_keys[j]);
		}
	}
	if (failures) {
		printf("gd77KeymapTest: %d failures\n", failures);
		return 1;
	}
	printf("gd77KeymapTest (emergency %d): all passed\n", CONFIG_DMR_EMERGENCY);
	return 0;
}
