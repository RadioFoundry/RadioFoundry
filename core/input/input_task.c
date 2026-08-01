// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The input task: scans board.input every INPUT_SCAN_MS, runs the gesture engine, queues events for the UI.
// PTT and MONITOR edges also go straight to the radio core so they work whatever the UI is doing.
#include "board.h"
#include "hal_time.h"
#include "input/input.h"
#include "input/input_engine.h"
#include "radio/radio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

// Task layout (see core/app/app.c for the whole picture)
#define INPUT_TASK_PRIORITY	3
#define INPUT_TASK_STACK	256	// words
#define INPUT_QUEUE_LEN		16

static input_engine_t engine;
static QueueHandle_t events;
static volatile uint32_t raw_keys;
static volatile int32_t enc_total;

static void emit(const input_event_t *ev, void *ctx)
{
	(void)ctx;
	if (ev->action == ACT_PTT && (ev->gesture == GESTURE_DOWN || ev->gesture == GESTURE_UP)) {
		radio_req_ptt(ev->gesture == GESTURE_DOWN);
	} else if (ev->action == ACT_MONITOR && (ev->gesture == GESTURE_DOWN || ev->gesture == GESTURE_UP)) {
		radio_req_monitor(ev->gesture == GESTURE_DOWN);
	}
	xQueueSend(events, ev, 0);	// a full queue drops the event (the UI is far behind anyway)
}

static void input_task(void *arg)
{
	(void)arg;
	TickType_t wake = xTaskGetTickCount();
	for (;;) {
		uint32_t keys = board.input->scan();
		int8_t enc = board.input->encoder();
		raw_keys = keys;
		enc_total += enc;
		input_engine_step(&engine, keys, enc, hal_millis(), emit, NULL);
		vTaskDelayUntil(&wake, pdMS_TO_TICKS(INPUT_SCAN_MS));
	}
}

void input_start(void)
{
	events = xQueueCreate(INPUT_QUEUE_LEN, sizeof(input_event_t));
	configASSERT(events != NULL);
	input_engine_init(&engine, board.keymap, board.keymap_len, hal_millis());
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
	return hal_millis() - engine.last_activity_ms;
}

uint32_t input_raw_keys(void)
{
	return raw_keys;
}

int32_t input_encoder_total(void)
{
	return enc_total;
}
