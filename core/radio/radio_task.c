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
static volatile bool hs_active;		// published: the radio is in hotspot mode
static volatile bool hs_carrier;	// published: hotspot carrier
static volatile int16_t hs_rssi;	// published: the last status poll's RSSI (dBm)
static volatile uint32_t hs_serial;
static uint32_t hs_orphan_ms;		// hotspot mode without a running hotspot since, 0 = not

// The hotspot engine gave up before it took the modem (or its leave was lost): back to the channel after this long
#define HOTSPOT_ORPHAN_MS	3000
#endif

static void publish(void)
{
	xSemaphoreTake(view_lock, portMAX_DELAY);
	shared_view = sm.view;
	shared_serial = sm.serial;
	xSemaphoreGive(view_lock);
#if CONFIG_HOTSPOT
	hs_active = sm.hotspot;
	hs_carrier = sm.hs_carrier;
	hs_rssi = sm.view.rssi_dbm;
#endif
}

#if CONFIG_HOTSPOT
// In hotspot mode with no hotspot running for a while: the enter was answered too late, or the host went away
static void hotspot_watch(uint32_t now)
{
	if (!sm.hotspot || hotspot_running()) {
		hs_orphan_ms = 0;
		return;
	}
	if (hs_orphan_ms == 0) {
		hs_orphan_ms = now != 0 ? now : 1;
	} else if (now - hs_orphan_ms >= HOTSPOT_ORPHAN_MS) {
		radio_req_t req = { .type = RREQ_HS_LEAVE };
		radio_sm_request(&sm, &req, now);
		hs_orphan_ms = 0;
	}
}
#endif

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
			hotspot_watch(hal_millis());
#endif
			next_poll += period;
			if ((int32_t)(now - next_poll) >= 0) {	// fell behind: don't try to catch up
				next_poll = now + period;
			}
		}
		publish();
	}
}

#if CONFIG_HOTSPOT
static const hotspot_radio_ops_t hotspot_ops;
#endif

void radio_core_start(void)
{
	view_lock = xSemaphoreCreateMutex();
	requests = xQueueCreate(RADIO_QUEUE_LEN, sizeof(radio_req_t));
	configASSERT(view_lock != NULL && requests != NULL);
	radio_sm_init(&sm, hal_millis());
	shared_view = sm.view;
	shared_serial = sm.serial;
	xTaskCreate(radio_task, "radio", RADIO_TASK_STACK, NULL, RADIO_TASK_PRIORITY, NULL);
#if CONFIG_HOTSPOT
	hotspot_set_radio(&hotspot_ops);
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

#if CONFIG_HOTSPOT
static void post2(uint8_t type, int32_t arg, uint32_t arg2)
{
	if (requests == NULL) {
		return;
	}
	radio_req_t req = { .type = type, .arg = arg, .arg2 = arg2 };
	xQueueSend(requests, &req, 0);
}
#endif

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
void radio_req_dmr_target(uint32_t id, bool private_call)
{
	post(RREQ_DMR_TARGET, (int32_t)((id & 0xFFFFFFu) | (private_call ? 1u << 24 : 0u)), 0);
}
void radio_req_dmr_contact(uint16_t index) { post(RREQ_DMR_CONTACT, index, 0); }
void radio_req_dmr_call_back(void) { post(RREQ_DMR_CALL_BACK, 0, 0); }
void radio_req_dmr_target_clear(void) { post(RREQ_DMR_TARGET_CLEAR, 0, 0); }
#if CONFIG_DMR_PRIVACY
void radio_req_dmr_privacy(uint8_t type, uint16_t key) { post(RREQ_DMR_PRIVACY, (int32_t)(((uint32_t)type << 16) | key), 0); }
#endif
#endif

#if CONFIG_HOTSPOT
// --- hotspot_radio_ops_t, called from the host link task. None of them waits ---

// The request runs in the radio task, which is above the host link task: it has normally been handled by the time
// the send returns. If not, false now and the hotspot asks again later (a repeat while in hotspot mode is a no-op)
static bool hs_enter(uint32_t rx_hz)
{
	if (hs_active) {
		return true;
	}
	post(RREQ_HS_ENTER, (int32_t)rx_hz, 0);
	return hs_active;
}

static bool hs_set_freq(uint32_t rx_hz, uint32_t tx_hz)
{
	if (!radio_sm_hotspot_freq_ok(rx_hz, tx_hz)) {
		return false;
	}
	post2(RREQ_HS_FREQ, (int32_t)rx_hz, tx_hz);
	return true;
}

static void hs_set_power(uint8_t rf_level)
{
	post(RREQ_HS_POWER, rf_level, 0);
}

static void hs_leave(void)
{
	post(RREQ_HS_LEAVE, 0, pdMS_TO_TICKS(50));	// must not be lost: the channel comes back with it
}

static bool hs_carrier_fn(void)
{
	return hs_carrier;
}

static int16_t hs_rssi_fn(void)
{
	return hs_active ? hs_rssi : 0;
}

static void hs_changed(void)
{
	hs_serial++;
}

static const hotspot_radio_ops_t hotspot_ops = {
	.enter = hs_enter, .set_freq = hs_set_freq, .set_power = hs_set_power, .leave = hs_leave,
	.carrier = hs_carrier_fn, .changed = hs_changed, .rssi_dbm = hs_rssi_fn,
	.cw_id = NULL, .cw_busy = NULL,	// no FM Morse keyer yet: the hotspot ACKs SEND_CWID and counts it unsent
};

uint32_t radio_hotspot_serial(void)
{
	return hs_serial;
}
#endif
