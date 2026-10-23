// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Gestures from key masks (docs/ux/information-architecture.md section 2): PRESS on release before the long-press
// threshold, LONG once at the threshold, REPEAT while held after LONG for repeating keys, DOWN / UP for raw-edge keys,
// ENC_STEP (DOWN, value = detents) for the encoder. Several keys may be held at once; each is tracked on its own.
// A DTMF key (input_dtmf_digit) goes to the DTMF callback first; a hold it takes gives no gestures (radio-features.md
// 3.7).
#include <string.h>
#include "input/input_engine.h"

void input_engine_init(input_engine_t *e, const keymap_entry_t *keymap, uint8_t keymap_len, uint32_t now_ms)
{
	memset(e, 0, sizeof(*e));
	e->keymap = keymap;
	e->keymap_len = keymap_len > INPUT_KEYS_MAX ? INPUT_KEYS_MAX : keymap_len;
	e->last_activity_ms = now_ms;
}

void input_engine_set_timing(input_engine_t *e, uint16_t long_ms, uint16_t repeat_ms)
{
	e->long_ms = long_ms;
	e->repeat_ms = repeat_ms;
}

void input_engine_set_dtmf(input_engine_t *e, input_dtmf_t cb, void *ctx)
{
	e->dtmf = cb;
	e->dtmf_ctx = ctx;
}

static void send(input_emit_t emit, void *ctx, uint8_t action, uint8_t gesture, int8_t value)
{
	if (action == ACT_NONE) {
		return;
	}
	input_event_t ev = { .action = action, .gesture = gesture, .value = value };
	emit(&ev, ctx);
}

void input_engine_step(input_engine_t *e, uint32_t raw, int8_t enc, uint32_t now_ms, input_emit_t emit, void *ctx)
{
	if (!e->started) {
		e->started = true;
		e->raw_prev = raw;
		e->held = 0;
		e->ignore = raw;
	}
	uint32_t stable = ~(raw ^ e->raw_prev);
	e->raw_prev = raw;
	uint32_t held = (e->held & ~stable) | (raw & stable);
	e->ignore &= held;		// a start-up key counts again once it has been released
	if (held != e->held || held != 0 || enc != 0) {
		e->last_activity_ms = now_ms;
	}
	e->held = held;
	held &= ~e->ignore;
	uint32_t long_ms = e->long_ms != 0 ? e->long_ms : INPUT_LONG_MS;
	uint32_t repeat_ms = e->repeat_ms != 0 ? e->repeat_ms : INPUT_REPEAT_MS;

	for (uint8_t i = 0; i < e->keymap_len; i++) {
		const keymap_entry_t *m = &e->keymap[i];
		input_key_t *k = &e->keys[i];
		bool down = m->bit < 32 && (held & (1u << m->bit)) != 0;

		if (down && !k->down) {
			k->down = true;
			k->long_done = false;
			k->t_down = now_ms;
			char digit = e->dtmf != NULL ? input_dtmf_digit(m) : 0;
			k->dtmf = digit != 0 && e->dtmf(digit, true, e->dtmf_ctx);
			if (m->raw_edges) {
				send(emit, ctx, m->short_action, GESTURE_DOWN, 0);
			}
		} else if (k->dtmf) {
			// A DTMF hold: only its end, to the same callback (it took the down, so it is still set)
			if (!down) {
				k->down = false;
				k->dtmf = false;
				if (e->dtmf != NULL) {
					e->dtmf(input_dtmf_digit(m), false, e->dtmf_ctx);
				}
			}
		} else if (!down && k->down) {
			k->down = false;
			if (m->raw_edges) {
				// value 1: the key's long press ran before this release (a monitor key with a hold function)
				send(emit, ctx, m->short_action, GESTURE_UP, k->long_done ? 1 : 0);
			} else if (!k->long_done) {
				send(emit, ctx, m->short_action, GESTURE_PRESS, 0);
			}
		} else if (down && (!m->raw_edges || m->long_action != m->short_action)) {
			// A raw-edge key whose long action differs (a monitor key with a hold function, keyassign.c) gets its
			// long press too, marked with value 1, never repeated
			if (!k->long_done) {
				if (now_ms - k->t_down >= input_long_threshold(m->long_action, long_ms)) {
					k->long_done = true;
					k->t_next = now_ms + repeat_ms;
					send(emit, ctx, m->long_action, GESTURE_LONG, m->raw_edges ? 1 : 0);
				}
			} else if (m->repeat && !m->raw_edges && (int32_t)(now_ms - k->t_next) >= 0) {
				k->t_next += repeat_ms;
				if ((int32_t)(now_ms - k->t_next) >= 0) {	// fell behind: no burst of repeats
					k->t_next = now_ms + repeat_ms;
				}
				send(emit, ctx, m->long_action, GESTURE_REPEAT, 0);
			}
		}
	}

	if (enc != 0) {
		send(emit, ctx, ACT_ENC_STEP, GESTURE_DOWN, enc);
	}
}
