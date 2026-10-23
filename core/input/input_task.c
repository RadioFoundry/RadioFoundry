// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The input task: scans board.input every INPUT_SCAN_MS, runs the gesture engine, queues events for the UI.
// PTT and MONITOR edges also go straight to the radio core so they work whatever the UI is doing.
// Keys injected by the host (input_inject_*, for the debug link) are ORed into the scan here.
#include <string.h>
#include "board.h"
#include "config.h"
#include "hal_time.h"
#include "input/input.h"
#include "input/input_engine.h"
#include "radio/radio.h"
#include "debuglink/dlog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

// Task layout (see core/app/app.c for the whole picture)
#define INPUT_TASK_PRIORITY	3
#define INPUT_TASK_STACK	256	// words
#define INPUT_QUEUE_LEN		16

static input_engine_t engine;
// The key map in use: the board's, with the side keys as assigned (input_set_keymap). A new one waits in keymap_next
// until no key is held, so a key never changes meaning between its press and its release
static keymap_entry_t keymap_live[INPUT_KEYS_MAX], keymap_next[INPUT_KEYS_MAX];
static volatile bool keymap_pending;
static QueueHandle_t events;
static volatile uint32_t raw_keys;
static volatile int32_t enc_total;
static int16_t enc_carry;		// detents the full queue couldn't take: sent with the next scan (input task only)
static volatile uint32_t enc_deferred;	// how often that happened

// Injection state, shared with the callers of input_inject_* (any task): every access in a critical section
static input_inj_state_t inj = { .idle = INPUT_INJECT_MIN_SCANS };	// generic injected keys
static uint32_t ptt_mask, ptt_until;			// the injected PTT key, on its own timer
static bool ptt_held;
static int8_t inj_enc;
static uint32_t wake_ms;
static bool wake_set;
static volatile uint8_t last_source = INPUT_SRC_KEYS;
static volatile uint32_t timing;	// input_set_timing: long ms << 16 | repeat ms (one word: no lock needed)
static volatile uint16_t long_now = INPUT_LONG_MS;
static volatile bool ptt_blocked;	// input_set_ptt_block
static volatile uint32_t ptt_downs;	// PTT presses seen by the engine (debug link: input.ptt_downs)
static volatile bool dtmf_blocked;	// input_set_dtmf_block

static void emit(const input_event_t *ev, void *ctx)
{
	(void)ctx;
	if (ev->action == ACT_PTT && ev->gesture == GESTURE_DOWN) {
		// Every press is logged with where it came from: a transmission nobody asked for shows its cause
		ptt_downs++;
		dlog("input: PTT down (%s, keys 0x%08x%s)", last_source == INPUT_SRC_HOST ? "host" : "keys",
		     (unsigned)raw_keys, ptt_blocked ? ", blocked" : "");
	}
	if (ev->action == ACT_PTT && (ev->gesture == GESTURE_UP || (ev->gesture == GESTURE_DOWN && !ptt_blocked))) {
		radio_req_ptt(ev->gesture == GESTURE_DOWN);
	} else if (ev->action == ACT_MONITOR && (ev->gesture == GESTURE_DOWN || ev->gesture == GESTURE_UP)) {
		if (ev->gesture == GESTURE_UP && ev->value != 0) {
			radio_req_monitor_held_up();	// its hold function ran: not a press (DMR doesn't toggle)
		} else {
			radio_req_monitor(ev->gesture == GESTURE_DOWN);
		}
	}
	if (xQueueSend(events, ev, 0) != pdTRUE && ev->action == ACT_ENC_STEP) {
		// The UI is behind: keep the detents for the next scan instead of losing them (a full queue drops key
		// events, but a lost encoder step is a wrong channel or row)
		enc_carry = (int16_t)(enc_carry + ev->value);
		enc_deferred++;
	}
}

#if CONFIG_DTMF
// A DTMF key's edges (radio-features.md 3.7): while the radio core says a digit would send now, the key is the radio
// core's for this hold and the UI sees none of it (no Find, Quick settings or key lock under the TX screen)
static bool dtmf_edge(char digit, bool down, void *ctx)
{
	(void)ctx;
	if (down && (dtmf_blocked || !radio_dtmf_ready())) {
		return false;
	}
	radio_req_dtmf(digit, down);
	return true;
}
#endif

static bool due(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) >= 0;
}

// The injected keys for this scan (expired holds released) and the injected encoder detents
static uint32_t take_injected(uint32_t now, int8_t *enc)
{
	bool ptt_expired = false;

	taskENTER_CRITICAL();
	if (ptt_held && due(now, ptt_until)) {
		ptt_held = false;
		ptt_expired = true;
	}
	uint32_t m = input_inj_scan(&inj, now) | (ptt_held ? ptt_mask : 0);
	*enc = inj_enc;
	inj_enc = 0;
	taskEXIT_CRITICAL();
	if (ptt_expired) {
		dlog("input: injected PTT released by its time-out");
	}
	return m;
}

static void input_task(void *arg)
{
	(void)arg;
	TickType_t wake = xTaskGetTickCount();
	for (;;) {
		uint32_t now = hal_millis();
		int8_t ienc;
		uint32_t inj_keys = take_injected(now, &ienc);
		uint32_t keys = board.input->scan();
		int8_t enc = board.input->encoder != NULL ? board.input->encoder() : 0;	// keypad-only radios have none
		raw_keys = keys;
		enc_total += enc;
		if ((keys != 0) || (enc != 0)) {
			last_source = INPUT_SRC_KEYS;
		} else if ((inj_keys != 0) || (ienc != 0)) {
			last_source = INPUT_SRC_HOST;
		}
		uint32_t tm = timing;
		input_engine_set_timing(&engine, (uint16_t)(tm >> 16), (uint16_t)tm);
		int32_t steps = enc_carry + enc + ienc;
		enc_carry = 0;
		if (steps > 127 || steps < -127) {	// the rest waits for the next scan
			enc_carry = (int16_t)(steps > 0 ? steps - 127 : steps + 127);
			steps = steps > 0 ? 127 : -127;
		}
		if (keymap_pending && engine.held == 0) {
			taskENTER_CRITICAL();
			memcpy(keymap_live, keymap_next, sizeof(keymap_live));
			keymap_pending = false;
			taskEXIT_CRITICAL();
		}
		input_engine_step(&engine, keys | inj_keys, (int8_t)steps, now, emit, NULL);
		vTaskDelayUntil(&wake, pdMS_TO_TICKS(INPUT_SCAN_MS));
	}
}

void input_start(void)
{
	events = xQueueCreate(INPUT_QUEUE_LEN, sizeof(input_event_t));
	configASSERT(events != NULL);
	uint8_t n = board.keymap_len > INPUT_KEYS_MAX ? INPUT_KEYS_MAX : board.keymap_len;
	memcpy(keymap_live, board.keymap, n * sizeof(keymap_live[0]));
	input_engine_init(&engine, keymap_live, n, hal_millis());
#if CONFIG_DTMF
	input_engine_set_dtmf(&engine, dtmf_edge, NULL);
#endif
	xTaskCreate(input_task, "input", INPUT_TASK_STACK, NULL, INPUT_TASK_PRIORITY, NULL);
}

bool input_next(input_event_t *ev, uint32_t wait_ms)
{
	if (events == NULL) {
		if (wait_ms != 0) {
			hal_delay_ms(wait_ms > 100 ? 100 : wait_ms);
		}
		return false;
	}
	TickType_t wait = wait_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
	return xQueueReceive(events, ev, wait) == pdTRUE;
}

uint32_t input_idle_ms(void)
{
	uint32_t now = hal_millis();
	uint32_t idle = now - engine.last_activity_ms;

	taskENTER_CRITICAL();
	if (wake_set && (now - wake_ms) < idle) {
		idle = now - wake_ms;
	}
	taskEXIT_CRITICAL();
	return idle;
}

void input_set_timing(uint16_t long_ms, uint16_t repeat_ms)
{
	if (long_ms != 0) {
		long_ms = long_ms < 200 ? 200 : long_ms > 3000 ? 3000 : long_ms;
	}
	if (repeat_ms != 0) {
		repeat_ms = repeat_ms < 40 ? 40 : repeat_ms > 1000 ? 1000 : repeat_ms;
	}
	timing = ((uint32_t)long_ms << 16) | repeat_ms;
	long_now = long_ms != 0 ? long_ms : INPUT_LONG_MS;
}

void input_set_keymap(const keymap_entry_t *map)
{
	uint8_t n = board.keymap_len > INPUT_KEYS_MAX ? INPUT_KEYS_MAX : board.keymap_len;	// also before input_start
	taskENTER_CRITICAL();
	memcpy(keymap_next, map, n * sizeof(keymap_next[0]));
	keymap_pending = true;
	taskEXIT_CRITICAL();
}

void input_set_ptt_block(bool blocked)
{
	ptt_blocked = blocked;
}

void input_set_dtmf_block(bool blocked)
{
	dtmf_blocked = blocked;
}

uint16_t input_long_ms(void)
{
	return long_now;
}

uint32_t input_raw_keys(void)
{
	return raw_keys;
}

uint32_t input_ptt_downs(void)
{
	return ptt_downs;
}

int32_t input_encoder_total(void)
{
	return enc_total;
}

bool input_encoder_stats(hal_encoder_stats_t *st, uint32_t *deferred)
{
	*deferred = enc_deferred;
	if (board.input == NULL || board.input->encoder_stats == NULL)
		return false;
	board.input->encoder_stats(st);
	return true;
}

// --- Injection ---

static const keymap_entry_t *key_for(uint8_t action, bool long_action)
{
	for (uint8_t i = 0; i < engine.keymap_len; i++) {
		const keymap_entry_t *m = &keymap_live[i];
		if ((long_action ? m->long_action : m->short_action) == action && m->bit < 32) {
			return m;
		}
	}
	return NULL;
}

input_inject_t input_inject_keys(uint32_t mask, uint32_t hold_ms)
{
	uint32_t now = hal_millis();
	input_inject_t r = INPUT_INJ_OK;

	if (mask == 0) {
		return INPUT_INJ_BAD;
	}
	if (hold_ms > INPUT_INJECT_MAX_MS) {
		hold_ms = INPUT_INJECT_MAX_MS;
	}
	if (hold_ms < 3u * INPUT_SCAN_MS) {	// two agreeing scans to debounce, and one to spare
		hold_ms = 3u * INPUT_SCAN_MS;
	}
	taskENTER_CRITICAL();
	if (!input_inj_ready(&inj, now, INPUT_INJECT_GAP_MS)) {
		r = INPUT_INJ_BUSY;
	} else {
		input_inj_hold(&inj, mask, now + hold_ms);
	}
	taskEXIT_CRITICAL();
	return r;
}

// Release injected keys in mask now (the task notices at its next scan)
static void release_keys(uint32_t mask)
{
	taskENTER_CRITICAL();
	if (inj.active && (inj.mask & mask) != 0) {
		inj.until = hal_millis();
	}
	taskEXIT_CRITICAL();
}

// An action no key gives: the event goes to the UI directly (rate limited like a key)
static input_inject_t post_direct(uint8_t action, uint8_t gesture, int8_t value)
{
	uint32_t now = hal_millis();
	input_inject_t r = INPUT_INJ_OK;

	if (events == NULL) {
		return INPUT_INJ_NO_KEY;
	}
	taskENTER_CRITICAL();
	if (!input_inj_ready(&inj, now, INPUT_INJECT_GAP_MS)) {
		r = INPUT_INJ_BUSY;
	} else {
		inj.released_ms = now;
		wake_ms = now;
		wake_set = true;
	}
	taskEXIT_CRITICAL();
	if (r == INPUT_INJ_OK) {
		input_event_t ev = { .action = action, .gesture = gesture, .value = value };
		last_source = INPUT_SRC_HOST;
		xQueueSend(events, &ev, 0);
	}
	return r;
}

input_inject_t input_inject_action(uint8_t action, uint8_t gesture, int8_t value)
{
	const keymap_entry_t *m;

	if (action == ACT_NONE || action >= ACT_COUNT) {
		return INPUT_INJ_BAD;
	}
	if (action == ACT_PTT) {
		if (gesture == GESTURE_DOWN || gesture == GESTURE_UP) {
			return input_inject_ptt(gesture == GESTURE_DOWN, 0);
		}
		return INPUT_INJ_BAD;
	}
	if (action == ACT_ENC_STEP) {
		if (gesture != GESTURE_DOWN || value == 0) {
			return INPUT_INJ_BAD;
		}
		taskENTER_CRITICAL();
		int v = inj_enc + value;
		inj_enc = (int8_t)(v > 127 ? 127 : (v < -127 ? -127 : v));
		taskEXIT_CRITICAL();
		return INPUT_INJ_OK;
	}
	switch (gesture) {
	case GESTURE_PRESS:
		m = key_for(action, false);
		if (m != NULL && !m->raw_edges) {
			return input_inject_keys(1u << m->bit, INPUT_INJECT_PRESS_MS);
		}
		break;
	case GESTURE_LONG:
	case GESTURE_REPEAT:
		m = key_for(action, true);
		if (m != NULL && !m->raw_edges && (gesture == GESTURE_LONG || m->repeat)) {
			uint32_t rep = (uint16_t)timing != 0 ? (uint16_t)timing : INPUT_REPEAT_MS;
			uint32_t extra = gesture == GESTURE_REPEAT ? (uint32_t)(value > 0 ? value : 1) * rep : 0;
			return input_inject_keys(1u << m->bit, input_long_threshold(m->long_action, long_now) +
						 INPUT_INJECT_LONG_EXTRA_MS + extra);
		}
		break;
	case GESTURE_DOWN:
	case GESTURE_UP:
		m = key_for(action, false);
		if (m != NULL && m->raw_edges) {
			if (gesture == GESTURE_DOWN) {
				return input_inject_keys(1u << m->bit, INPUT_INJECT_MAX_MS);
			}
			release_keys(1u << m->bit);
			return INPUT_INJ_OK;
		}
		break;
	default:
		return INPUT_INJ_BAD;
	}
	if (gesture == GESTURE_DOWN || gesture == GESTURE_UP) {
		return INPUT_INJ_NO_KEY;	// raw edges need a real key (they act on the radio core)
	}
	return post_direct(action, gesture, value);
}

input_inject_t input_inject_action_hold(uint8_t action, uint32_t hold_ms)
{
	if (action == ACT_PTT) {
		return input_inject_ptt(true, hold_ms);
	}
	const keymap_entry_t *m = key_for(action, false);
	if (m == NULL) {
		m = key_for(action, true);
	}
	if (m == NULL) {
		return INPUT_INJ_NO_KEY;
	}
	return input_inject_keys(1u << m->bit, hold_ms);
}

input_inject_t input_inject_ptt(bool down, uint32_t max_ms)
{
	const keymap_entry_t *m = key_for(ACT_PTT, false);

	if (m == NULL || !m->raw_edges) {
		return INPUT_INJ_NO_KEY;
	}
	if (max_ms == 0 || max_ms > INPUT_INJECT_MAX_MS) {
		max_ms = INPUT_INJECT_MAX_MS;
	}
	taskENTER_CRITICAL();
	if (down) {
		ptt_mask = 1u << m->bit;
		ptt_until = hal_millis() + max_ms;	// a repeated DOWN re-arms the timer (the host is still there)
		ptt_held = true;
	} else {
		ptt_held = false;
	}
	taskEXIT_CRITICAL();
	return INPUT_INJ_OK;
}

uint32_t input_injected_keys(void)
{
	taskENTER_CRITICAL();
	uint32_t m = (inj.active ? inj.mask : 0) | (ptt_held ? ptt_mask : 0);
	taskEXIT_CRITICAL();
	return m;
}

input_source_t input_last_source(void)
{
	return (input_source_t)last_source;
}

void input_inject_wake(void)
{
	taskENTER_CRITICAL();
	wake_ms = hal_millis();
	wake_set = true;
	taskEXIT_CRITICAL();
}
