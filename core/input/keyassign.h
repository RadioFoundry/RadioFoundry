// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Side key assignment (docs/spec/ui.md 3.1): what the board's reassignable keys (board.side_keys) do on a press and
// on a long press, kept in the open-firmware codeplug's ten quick-key slots (open-firmware-cps.md 7.2, EEPROM
// 0x7524). Slot 0 holds KEYASSIGN_MARKER; without it the slots are another firmware's and the keys keep the board's
// key map. Slot 1 + 2k is role k's press, slot 2 + 2k its long press; slots 7-9 are spare. No RTOS: the host tests
// run it.
#ifndef KEYASSIGN_H
#define KEYASSIGN_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_input.h"

#define KEYASSIGN_SLOTS		10
#define KEYASSIGN_MARKER	0x4B01u	// "K", layout 1
#define KEYASSIGN_ROLES		3	// side key 1, side key 2, top key

// A slot's value. Stored: append only, never renumbered (gotchas 9.11)
typedef enum {
	KEYFN_DEFAULT,		// what the board's key map gives
	KEYFN_NONE,		// nothing
	KEYFN_MONITOR,		// open the squelch while held (press only: the key then has no long press)
	KEYFN_TALK_TARGET,	// the talk target picker
	KEYFN_POWER,		// the next power level
	KEYFN_ZONE_UP,		// the next zone
	KEYFN_ZONE_DOWN,	// the previous zone
	KEYFN_SCAN,		// scan on / off
	KEYFN_EMERGENCY,	// emergency on / off (long press only: hold to activate, ux/principles.md P2)
	KEYFN_HEAR_ALL,		// hear all (every talkgroup) on / off
	KEYFN_MONITOR_LATCH,	// monitor on / off, latched
	KEYFN_KEY_LOCK,		// lock / unlock the keys
	KEYFN_CALL_BACK,	// talk to the last call heard: its talkgroup, or its caller for a private call
	KEYFN_COUNT
} keyfn_t;

// fn may go in a press (hold false) or long press slot
bool keyassign_allowed(uint8_t fn, bool hold);
// The function in role's press or long press slot: KEYFN_DEFAULT without the marker or for a value not allowed there
uint8_t keyassign_get(const uint16_t slots[KEYASSIGN_SLOTS], uint8_t role, bool hold);
// Set it (KEYFN_DEFAULT for a value not allowed there). Slots without the marker are another firmware's: they are
// cleared and the marker written first
void keyassign_set(uint16_t slots[KEYASSIGN_SLOTS], uint8_t role, bool hold, uint8_t fn);
// The key map in use: base with the roles' keys (side_keys, side_key_count of them) changed as the slots say. out
// holds base_len entries
void keyassign_build(const keymap_entry_t *base, uint8_t base_len, const uint8_t *side_keys, uint8_t side_key_count,
		     const uint16_t slots[KEYASSIGN_SLOTS], keymap_entry_t *out);

#endif
