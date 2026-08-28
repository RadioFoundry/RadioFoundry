// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The gesture engine behind core/input, without the RTOS: physical key masks and encoder counts in, logical
// events out through a callback. input_task.c runs it every INPUT_SCAN_MS; the host tests drive it directly.
#ifndef INPUT_ENGINE_H
#define INPUT_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "input/input.h"

#define INPUT_SCAN_MS		10
#define INPUT_KEYS_MAX		32	// key map entries handled (key bits are 0..31)

typedef void (*input_emit_t)(const input_event_t *ev, void *ctx);

typedef struct {
	uint32_t t_down;	// when the key went down
	uint32_t t_next;	// next REPEAT
	bool down;
	bool long_done;		// LONG already sent for this hold
} input_key_t;

typedef struct {
	const keymap_entry_t *keymap;
	uint8_t keymap_len;
	bool started;
	uint32_t raw_prev;	// last raw sample (debounce: a bit counts once two samples agree)
	uint32_t held;		// debounced mask
	uint32_t ignore;	// keys held at start-up, ignored until released (no TX from a PTT held at power-on)
	uint32_t last_activity_ms;
	uint16_t long_ms;	// long press threshold, 0 = INPUT_LONG_MS
	uint16_t repeat_ms;	// repeat period, 0 = INPUT_REPEAT_MS
	input_key_t keys[INPUT_KEYS_MAX];
} input_engine_t;

void input_engine_init(input_engine_t *e, const keymap_entry_t *keymap, uint8_t keymap_len, uint32_t now_ms);
// One scan: raw key mask and encoder detents since the last call
// Gesture timing in ms (0 = the defaults); keys held now keep their LONG / REPEAT state
void input_engine_set_timing(input_engine_t *e, uint16_t long_ms, uint16_t repeat_ms);
void input_engine_step(input_engine_t *e, uint32_t raw, int8_t enc, uint32_t now_ms, input_emit_t emit, void *ctx);

#endif
