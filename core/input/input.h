// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Logical input: polls board.input, applies the board key map, produces actions with gestures
#ifndef CORE_INPUT_H
#define CORE_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_input.h"

#define INPUT_LONG_MS		600
#define INPUT_REPEAT_MS		120

void input_start(void);
// Wait up to wait_ms for the next event (0 = don't wait). PTT and MONITOR edges also go straight to the radio core
bool input_next(input_event_t *ev, uint32_t wait_ms);
// Milliseconds since the last key or encoder activity (backlight time-out)
uint32_t input_idle_ms(void);
// Raw state for the diagnostics page: physical key bits held now, encoder detents since start
uint32_t input_raw_keys(void);
int32_t input_encoder_total(void);
// PTT presses the key engine produced since start, the blocked ones included (each is also in the debug log)
uint32_t input_ptt_downs(void);
// The board's encoder decoder counts (false: none) and how often the UI's queue was full and detents waited for the
// next scan (debug link: input.enc_*)
bool input_encoder_stats(hal_encoder_stats_t *st, uint32_t *deferred);
// Gesture timing (Settings > Keys): the long press threshold and the repeat period in ms, 0 = the defaults above.
// Clamped to 200..3000 and 40..1000; in effect from the next scan
void input_set_timing(uint16_t long_ms, uint16_t repeat_ms);
uint16_t input_long_ms(void);		// in effect now
// PTT while the keys are locked and Settings > Privacy > PTT when locked is off: a PTT press is not handed to the
// radio core (the UI still gets it, to say why). A release always is, so a transmission never sticks
void input_set_ptt_block(bool blocked);
// Keys locked: DTMF keys don't send during FM TX (radio-features.md 3.7)
void input_set_dtmf_block(bool blocked);
// The key map in use (core/input/keyassign.c builds it): board.keymap_len entries, the board's keys in the board's
// order with other actions. Copied; in effect once no key is held
void input_set_keymap(const keymap_entry_t *map);

// --- Injection (the debug link, core/debuglink): host-driven keys processed by the input task exactly like real ones.
// Injected physical key bits are ORed into the scan mask, so they go through the same debounce and gesture engine
// (PTT and MONITOR edges reach the radio core as usual) and count as key activity for input_idle_ms(). Every hold is
// capped at INPUT_INJECT_MAX_MS, so a host that goes away can't leave a key (PTT) held. Callable from any task.

#define INPUT_INJECT_MAX_MS	10000u	// longest hold of any injected key, PTT included
#define INPUT_INJECT_GAP_MS	40u	// after an injected key is released, the next one waits this long (debounce)
#define INPUT_INJECT_PRESS_MS	80u	// how long a short PRESS is held
#define INPUT_INJECT_LONG_EXTRA_MS	120u	// a LONG is held the long press time plus this (released after the LONG)

typedef enum {
	INPUT_SRC_KEYS,		// the radio's own keys / encoder
	INPUT_SRC_HOST,		// injected by the host (debug link)
} input_source_t;

typedef enum {
	INPUT_INJ_OK,
	INPUT_INJ_BUSY,		// an injected key is still held (or released less than INPUT_INJECT_GAP_MS or 3 scans ago)
	INPUT_INJ_NO_KEY,	// no key in the board's key map gives that action / gesture
	INPUT_INJ_BAD,		// bad argument
} input_inject_t;

// Hold physical key bits (the board's scan mask bits) for hold_ms (capped at INPUT_INJECT_MAX_MS), then release
input_inject_t input_inject_keys(uint32_t mask, uint32_t hold_ms);
// Release injected keys in mask now, before their hold ends (the task notices at its next scan; a key still ends
// only after the scans that debounce it have seen it)
void input_inject_release(uint32_t mask);
// A logical action with a gesture, through the key map: PRESS holds the key that gives it for INPUT_INJECT_PRESS_MS,
// LONG for input_long_ms() + INPUT_INJECT_LONG_EXTRA_MS; DOWN / UP hold and release a raw-edge key (MONITOR; PTT goes to input_inject_ptt);
// ACT_ENC_STEP with GESTURE_DOWN adds value detents to the encoder
input_inject_t input_inject_action(uint8_t action, uint8_t gesture, int8_t value);
// Hold the key that gives action (as its short or its long action) for hold_ms, whatever the gesture that makes
// (capped at INPUT_INJECT_MAX_MS; ACT_PTT goes through input_inject_ptt with hold_ms as its time-out)
input_inject_t input_inject_action_hold(uint8_t action, uint32_t hold_ms);
// The PTT key: down holds it for at most max_ms (0 or more than INPUT_INJECT_MAX_MS: INPUT_INJECT_MAX_MS), up
// releases it. Independent of the other injected keys
input_inject_t input_inject_ptt(bool down, uint32_t max_ms);
// Injected keys held now (bits), PTT included
uint32_t input_injected_keys(void);
// Where the last key activity came from (the UI may treat host keys differently, e.g. not as a wake-up key)
input_source_t input_last_source(void);
// Count as key activity now without any key (wakes the backlight through input_idle_ms; no event)
void input_inject_wake(void);

#endif
