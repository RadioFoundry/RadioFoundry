// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's task: a request queue in front of the state machine (radio_sm.c), a status poll every
// RADIO_POLL_MS, the DMR call stack every RADIO_TICK_MS, and a snapshot of the view that other tasks copy under a
// mutex. It also implements the hotspot's radio operations (hotspot_radio_ops_t): they post requests and never wait,
// so the host link's MMDVM replies stay fast (docs/testing/mmdvm-audit.md A-15).
#include <string.h>
#include "radio/radio.h"
#include "radio/radio_sm.h"
#include "hal_time.h"
#include "config.h"
#if CONFIG_HOTSPOT
#include "hotspot/hotspot.h"
#include "radio/radio_hotspot.h"
#endif
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

// Task layout (see core/app/app.c for the whole picture)
#define RADIO_TASK_PRIORITY	4
#if CONFIG_DMR
#define RADIO_TASK_STACK	768	// words: the DMR call stack and the AMBE FEC run here
#else
#define RADIO_TASK_STACK	512	// words
#endif
#define RADIO_QUEUE_LEN		16

static radio_sm_t sm;			// owned by the radio task
static radio_view_t shared_view;	// the published copy, under view_lock
static volatile uint32_t shared_serial;
static SemaphoreHandle_t view_lock;
static QueueHandle_t requests;

#if CONFIG_HOTSPOT
// radio_hotspot.c's way into the queue: from the host link task, never waiting except for a request that must not be
// lost (the leave: the channel comes back with it)
static void hs_post(const radio_req_t *req, bool must)
{
	if (requests != NULL) {
		xQueueSend(requests, req, must ? pdMS_TO_TICKS(50) : 0);
	}
}
#endif

static void publish(void)
{
	xSemaphoreTake(view_lock, portMAX_DELAY);
	shared_view = sm.view;
	shared_serial = sm.serial;
	xSemaphoreGive(view_lock);
#if CONFIG_HOTSPOT
	radio_hs_publish(&sm);
#endif
}


static void radio_task(void *arg)
{
	(void)arg;
	TickType_t period = pdMS_TO_TICKS(RADIO_POLL_MS);
	TickType_t tick_period = pdMS_TO_TICKS(RADIO_TICK_MS);
	TickType_t next_poll = xTaskGetTickCount() + period;
	TickType_t next_tick = xTaskGetTickCount() + tick_period;

	for (;;) {
		TickType_t due = (int32_t)(next_tick - next_poll) < 0 ? next_tick : next_poll;
		int32_t left = (int32_t)(due - xTaskGetTickCount());
		radio_req_t req;
		if (xQueueReceive(requests, &req, left > 0 ? (TickType_t)left : 0) == pdTRUE) {
			do {
				radio_sm_request(&sm, &req, hal_millis());
			} while (xQueueReceive(requests, &req, 0) == pdTRUE);
		}
		TickType_t now = xTaskGetTickCount();
		if ((int32_t)(now - next_tick) >= 0) {
			radio_sm_tick(&sm, hal_millis());
			next_tick += tick_period;
			if ((int32_t)(now - next_tick) >= 0) {
				next_tick = now + tick_period;
			}
		}
		if ((int32_t)(now - next_poll) >= 0) {
			radio_sm_poll(&sm, hal_millis());
#if CONFIG_HOTSPOT
			radio_hs_watch(&sm, hal_millis());
#endif
			next_poll += period;
			if ((int32_t)(now - next_poll) >= 0) {	// fell behind: don't try to catch up
				next_poll = now + period;
			}
		}
		publish();
	}
}

void radio_core_start(void)
{
	view_lock = xSemaphoreCreateMutex();
	requests = xQueueCreate(RADIO_QUEUE_LEN, sizeof(radio_req_t));
	configASSERT(view_lock != NULL && requests != NULL);
	radio_sm_init(&sm, hal_millis());
	shared_view = sm.view;
	shared_serial = sm.serial;
#if CONFIG_HOTSPOT
	radio_hs_init(hs_post);
#endif
	xTaskCreate(radio_task, "radio", RADIO_TASK_STACK, NULL, RADIO_TASK_PRIORITY, NULL);
#if CONFIG_HOTSPOT
	hotspot_set_radio(&radio_hs_ops);
#endif
}

void radio_get_view(radio_view_t *v)
{
	if (view_lock == NULL) {
		memset(v, 0, sizeof(*v));
		return;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	*v = shared_view;
	xSemaphoreGive(view_lock);
}

uint32_t radio_view_serial(void)
{
	return shared_serial;
}

static void post(uint8_t type, int32_t arg, TickType_t wait)
{
	if (requests == NULL) {
		return;
	}
	radio_req_t req = { .type = type, .arg = arg };
	xQueueSend(requests, &req, wait);
}


void radio_req_channel_step(int8_t dir) { post(RREQ_CH_STEP, dir, 0); }
void radio_req_zone_pos(uint16_t pos) { post(RREQ_ZONE_POS, pos, 0); }
void radio_req_zone(uint16_t index) { post(RREQ_ZONE, index, 0); }
void radio_req_vfo(bool on) { post(RREQ_VFO, on, 0); }
void radio_req_vfo_step(int8_t dir) { post(RREQ_VFO_STEP, dir, 0); }
void radio_req_vfo_set(uint32_t hz) { post(RREQ_VFO_SET, (int32_t)hz, 0); }
// PTT and monitor edges must not be lost (a lost release would keep transmitting until the time-out)
void radio_req_ptt(bool down) { post(RREQ_PTT, down, pdMS_TO_TICKS(50)); }
void radio_req_monitor(bool down) { post(RREQ_MONITOR, down, pdMS_TO_TICKS(50)); }
void radio_req_power(int8_t dir) { post(RREQ_POWER, dir, 0); }
void radio_req_squelch(int8_t dir) { post(RREQ_SQUELCH, dir, 0); }
void radio_req_clear_temporary(void) { post(RREQ_CLEAR_TEMP, 0, 0); }
void radio_req_bandwidth(radio_bw_t bw) { post(RREQ_BANDWIDTH, bw, 0); }
void radio_req_tone(uint8_t which, subtone_t tone) { post(RREQ_TONE, radio_tone_arg(which, tone), 0); }
#if CONFIG_DMR
void radio_req_reply_cancel(void) { post(RREQ_REPLY_CANCEL, 0, 0); }
#endif

#if CONFIG_HOTSPOT
uint32_t radio_hotspot_serial(void)
{
	return radio_hs_serial();
}
#endif
