// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Side key assignment (keyassign.h, docs/spec/ui.md 3.1)
#include "config.h"
#if CONFIG_KEY_ASSIGN
#include <string.h>
#include "input/keyassign.h"

// The input action each function sends (MONITOR as raw edges, the rest as a press or a long press)
static const uint8_t fn_action[KEYFN_COUNT] = {
	[KEYFN_DEFAULT] = ACT_NONE, [KEYFN_NONE] = ACT_NONE, [KEYFN_MONITOR] = ACT_MONITOR,
	[KEYFN_TALK_TARGET] = ACT_TALK_TARGET, [KEYFN_POWER] = ACT_POWER, [KEYFN_ZONE_UP] = ACT_ZONE_UP,
	[KEYFN_ZONE_DOWN] = ACT_ZONE_DOWN, [KEYFN_SCAN] = ACT_SCAN, [KEYFN_EMERGENCY] = ACT_EMERGENCY,
	[KEYFN_KEY_LOCK] = ACT_LOCK,
	[KEYFN_CALL_BACK] = ACT_CALL_BACK, [KEYFN_HOME_ZONE] = ACT_HOME_ZONE,
	[KEYFN_COPY_VFO] = ACT_COPY_VFO, [KEYFN_DUAL_WATCH] = ACT_DUAL_WATCH, [KEYFN_AB] = ACT_AB,
};

bool keyassign_allowed(uint8_t fn, bool hold)
{
	if (fn >= KEYFN_COUNT) {
		return false;
	}
	if (fn == KEYFN_MONITOR_LATCH || fn == KEYFN_HEAR_ALL) {
		return false;	// retired
	}
	return hold ? fn != KEYFN_MONITOR : fn != KEYFN_EMERGENCY;
}

uint8_t keyassign_get(const uint16_t slots[KEYASSIGN_SLOTS], uint8_t role, bool hold)
{
	if (slots[0] != KEYASSIGN_MARKER || role >= KEYASSIGN_ROLES) {
		return KEYFN_DEFAULT;
	}
	uint16_t v = slots[1u + 2u * role + (hold ? 1u : 0u)];
	return v < KEYFN_COUNT && keyassign_allowed((uint8_t)v, hold) ? (uint8_t)v : KEYFN_DEFAULT;
}

void keyassign_set(uint16_t slots[KEYASSIGN_SLOTS], uint8_t role, bool hold, uint8_t fn)
{
	if (role >= KEYASSIGN_ROLES) {
		return;
	}
	if (slots[0] != KEYASSIGN_MARKER) {
		memset(slots, 0, KEYASSIGN_SLOTS * sizeof(slots[0]));
		slots[0] = KEYASSIGN_MARKER;
	}
	slots[1u + 2u * role + (hold ? 1u : 0u)] = keyassign_allowed(fn, hold) ? fn : KEYFN_DEFAULT;
}

void keyassign_build(const keymap_entry_t *base, uint8_t base_len, const uint8_t *side_keys, uint8_t side_key_count,
		     const uint16_t slots[KEYASSIGN_SLOTS], keymap_entry_t *out)
{
	memcpy(out, base, base_len * sizeof(*base));
	for (uint8_t role = 0; role < side_key_count && role < KEYASSIGN_ROLES; role++) {
		uint8_t press = keyassign_get(slots, role, false), hold = keyassign_get(slots, role, true);
		if (press == KEYFN_DEFAULT && hold == KEYFN_DEFAULT) {
			continue;
		}
		for (uint8_t i = 0; i < base_len; i++) {
			keymap_entry_t *m = &out[i];
			if (m->bit != side_keys[role]) {
				continue;
			}
			if (press == KEYFN_MONITOR || (press == KEYFN_DEFAULT && m->raw_edges)) {
				m->short_action = press == KEYFN_MONITOR ? ACT_MONITOR : m->short_action;
				// Held for as long as the key is. A hold function of its own also runs (DMR channels: on FM the
				// key is the squelch break and the UI drops it, #19); none: no long press
				m->long_action = hold != KEYFN_DEFAULT ? fn_action[hold] : m->short_action;
				m->raw_edges = true;
				break;
			}
			if (m->raw_edges) {
				// A hold key (monitor) gets a press of its own: its long press is nothing unless set
				m->long_action = ACT_NONE;
				m->raw_edges = false;
			}
			if (press != KEYFN_DEFAULT) {
				m->short_action = fn_action[press];
			}
			if (hold != KEYFN_DEFAULT) {
				m->long_action = fn_action[hold];
			}
			m->repeat = false;
			break;
		}
	}
}
#endif
