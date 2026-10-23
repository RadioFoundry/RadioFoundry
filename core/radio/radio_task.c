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
#if CONFIG_DMR_DATA_CORE
#include "dmr/dmrDataService.h"
#endif
#if CONFIG_DMR_EMERGENCY
#include "dmr/dmr_emergency.h"
#endif
#if CONFIG_APRS
#include "aprs/aprs.h"
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

static radio_sm_t sm ODR_CPU_RAM;	// owned by the radio task; CPU only
static radio_settings_t pending_settings;	// radio_set_settings -> the radio task, under view_lock
static radio_settings_t shared_settings;	// the published copy, under view_lock
static radio_view_t shared_view;	// the published copy, under view_lock
static volatile bool dtmf_ready;	// shared_view.dtmf_ready, read without the lock (radio_dtmf_ready)
#if CONFIG_DUAL_WATCH
static channel_t shared_vfo[2];		// the VFOs' settings (radio_get_vfo), under view_lock
static bool shared_vfo_ok[2];
#endif
#if CONFIG_SCAN
static radio_dual_ref_t shared_dual[2];	// dual watch (radio_get_dual), under view_lock
static uint8_t shared_dual_sel, shared_dual_at;
static bool shared_dual_on;
#endif
static volatile uint32_t shared_serial;
static volatile bool power_off_busy;	// radio_req_power_off until the radio core is done
static volatile bool power_off_wanted;	// published view.power_off
static SemaphoreHandle_t view_lock;
static QueueHandle_t requests;

static volatile uint32_t req_handled, req_dropped;

static void send(const radio_req_t *req, TickType_t wait)
{
	if (xQueueSend(requests, req, wait) != pdTRUE) {
		req_dropped++;	// the queue was full: the request is lost
	}
}

#if CONFIG_HOTSPOT
// radio_hotspot.c's way into the queue: from the host link task, never waiting except for a request that must not be
// lost (the leave: the channel comes back with it)
static void hs_post(const radio_req_t *req, bool must)
{
	if (requests != NULL) {
		send(req, must ? pdMS_TO_TICKS(50) : 0);
	}
}
#endif

static void publish(void)
{
	xSemaphoreTake(view_lock, portMAX_DELAY);
	shared_view = sm.view;
	dtmf_ready = sm.view.dtmf_ready;
	radio_sm_shown_view(&sm, &shared_view);	// a roaming survey visit: the screens keep the site it left
	shared_serial = sm.serial;
	shared_settings = sm.set;
#if CONFIG_DUAL_WATCH
	shared_vfo[sm.vfo_idx & 1u] = sm.vfo_ch;
	shared_vfo_ok[sm.vfo_idx & 1u] = sm.vfo_seeded;
	shared_vfo[!sm.vfo_idx] = sm.vfo_other.ch;
	shared_vfo_ok[!sm.vfo_idx] = sm.vfo_other.seeded;
#endif
#if CONFIG_SCAN
	shared_dual_on = sm.scan.kind == SCAN_DUAL;
	shared_dual[0] = sm.dual[0];
	shared_dual[1] = sm.dual[1];
	shared_dual_sel = sm.dual_sel;
	shared_dual_at = sm.dual_at;
#endif
	xSemaphoreGive(view_lock);
	power_off_wanted = sm.view.power_off;
	if (sm.power_off_req) {
		power_off_busy = sm.power_off_busy;
	}
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
				req_handled++;
				if (req.type == RREQ_SETTINGS) {
					radio_settings_t set;
					xSemaphoreTake(view_lock, portMAX_DELAY);
					set = pending_settings;
					xSemaphoreGive(view_lock);
					radio_sm_set_settings(&sm, &set, hal_millis());
				} else {
					radio_sm_request(&sm, &req, hal_millis());
				}
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

#if CONFIG_DMR_DATA_CORE
// The data service's API (dmr_data_api.h) is called from the UI and debug link tasks: its copies are short (an inbox
// message), so the scheduler is held rather than a mutex taken (no priority games with the radio task)
static void data_lock(void) { vTaskSuspendAll(); }
static void data_unlock(void) { (void)xTaskResumeAll(); }
#endif

#if CONFIG_APRS
// The APRS engine's any-task calls (core/aprs/aprs.h): short copies, the same way
static void aprs_lock(void) { vTaskSuspendAll(); }
static void aprs_unlock(void) { (void)xTaskResumeAll(); }
#endif

void radio_core_start(void)
{
#if CONFIG_APRS
	aprs_set_lock(aprs_lock, aprs_unlock);
#endif
#if CONFIG_DMR_DATA_CORE
	dmrDataServiceSetLock(data_lock, data_unlock);
#endif
#if CONFIG_DMR_EMERGENCY
	dmr_emergency_set_lock(data_lock, data_unlock);	// the same short copies
#endif
	view_lock = xSemaphoreCreateMutex();
	requests = xQueueCreate(RADIO_QUEUE_LEN, sizeof(radio_req_t));
	configASSERT(view_lock != NULL && requests != NULL);
	radio_sm_init(&sm, hal_millis());
	shared_view = sm.view;
	shared_serial = sm.serial;
	shared_settings = sm.set;
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

radio_state_t radio_get_state(void)
{
	if (view_lock == NULL) {
		return RS_IDLE;		// as radio_get_view's zeroed view
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	radio_state_t st = shared_view.state;
	xSemaphoreGive(view_lock);
	return st;
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
	send(&req, wait);
}

void radio_req_stats(uint32_t *handled, uint32_t *dropped)
{
	*handled = req_handled;
	*dropped = req_dropped;
}


void radio_req_channel_step(int8_t dir) { post(RREQ_CH_STEP, dir, 0); }
void radio_req_zone_pos(uint16_t pos) { post(RREQ_ZONE_POS, pos, 0); }
void radio_req_zone(uint16_t index) { post(RREQ_ZONE, index, 0); }
void radio_req_vfo(bool on) { post(RREQ_VFO, on, 0); }
void radio_req_vfo_step(int8_t dir) { post(RREQ_VFO_STEP, dir, 0); }
void radio_req_vfo_copy(void) { post(RREQ_VFO_COPY, 0, 0); }

radio_vfo_copy_t radio_req_vfo_copy_checked(void)
{
	radio_vfo_copy_t r = VFO_COPY_NO_VFO;
	if (view_lock != NULL) {
		xSemaphoreTake(view_lock, portMAX_DELAY);
		r = radio_vfo_copy_check(&shared_view.ch, shared_view.vfo);
		xSemaphoreGive(view_lock);
	}
	if (r == VFO_COPY_OK) {
		radio_req_vfo_copy();
	}
	return r;
}
void radio_req_vfo_set(uint32_t hz) { post(RREQ_VFO_SET, (int32_t)hz, 0); }
void radio_req_vfo_step_size(uint32_t hz) { post(RREQ_VFO_STEP_SIZE, (int32_t)hz, 0); }
// PTT and monitor edges must not be lost (a lost release would keep transmitting until the time-out)
void radio_req_ptt(bool down) { post(RREQ_PTT, down, pdMS_TO_TICKS(50)); }
void radio_req_ptt_host(bool down) { post(RREQ_PTT, down ? 2 : 0, pdMS_TO_TICKS(50)); }
void radio_req_monitor(bool down) { post(RREQ_MONITOR, down, pdMS_TO_TICKS(50)); }
void radio_req_monitor_held_up(void) { post(RREQ_MONITOR, 2, pdMS_TO_TICKS(50)); }
void radio_req_power(int8_t dir) { post(RREQ_POWER, dir, 0); }
void radio_req_squelch(int8_t dir) { post(RREQ_SQUELCH, dir, 0); }
// The level must not be lost: the user looks at the bar, and the setting is already saved
void radio_req_volume(uint8_t step) { post(RREQ_VOLUME, step, pdMS_TO_TICKS(50)); }
void radio_req_volume_override(int8_t step) { post(RREQ_VOLUME_OVERRIDE, step, pdMS_TO_TICKS(50)); }
void radio_req_clear_temporary(void) { post(RREQ_CLEAR_TEMP, 0, 0); }
void radio_req_reload(void) { post(RREQ_RELOAD, 0, pdMS_TO_TICKS(50)); }
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
void radio_req_dmr_timeslot(uint8_t ts) { post(RREQ_DMR_TS, ts, 0); }
void radio_req_dmr_color_code(uint8_t cc) { post(RREQ_DMR_CC, cc, 0); }
void radio_req_mode(uint8_t mode) { post(RREQ_MODE, mode, 0); }
void radio_req_dmr_hear_all(bool on) { post(RREQ_DMR_HEAR_ALL, on, 0); }
// A lost focus change would send the next PTT to the wrong target: wait a little for room in the queue
void radio_req_dmr_focus(uint32_t id, bool private_call)
{
	post(RREQ_DMR_FOCUS, (int32_t)((id & 0xFFFFFFu) | (private_call ? 1u << 24 : 0u)), pdMS_TO_TICKS(20));
}
#if CONFIG_DMR_PRIVACY
void radio_req_dmr_privacy(uint8_t type, uint16_t key) { post(RREQ_DMR_PRIVACY, (int32_t)(((uint32_t)type << 16) | key), 0); }
#endif
#endif

static void post_pair(uint8_t type, int32_t arg, uint32_t arg2)
{
	if (requests == NULL) {
		return;
	}
	radio_req_t req = { .type = type, .arg = arg, .arg2 = arg2 };
	send(&req, 0);
}

void radio_req_scan(uint8_t cmd) { post(RREQ_SCAN, cmd, 0); }
void radio_req_scan_vfo(uint32_t lo_hz, uint32_t hi_hz) { post_pair(RREQ_SCAN_VFO, (int32_t)lo_hz, hi_hz); }
void radio_req_scan_dual(uint16_t pos) { post_pair(RREQ_SCAN, RSCAN_DUAL, pos); }
bool radio_dtmf_ready(void)
{
	return dtmf_ready;
}

// A lost key up would hold the tone until PTT is released: wait a little for room, as PTT does. Without CONFIG_DTMF
// the radio core drops it
void radio_req_dtmf(char digit, bool down) { post(RREQ_DTMF, (uint8_t)digit | (down ? 0x100 : 0), pdMS_TO_TICKS(50)); }

void radio_req_dual(const radio_dual_ref_t ref[2], uint8_t sel)
{
	if (ref == NULL) {
		post_pair(RREQ_DUAL, 0, 0);
		return;
	}
	post_pair(RREQ_DUAL, (int32_t)radio_dual_arg(&ref[0], sel == 0), radio_dual_arg(&ref[1], sel != 0));
}

bool radio_get_vfo(uint8_t index, channel_t *out)
{
#if CONFIG_DUAL_WATCH
	if (view_lock == NULL) {
		return false;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	*out = shared_vfo[index & 1u];
	bool ok = shared_vfo_ok[index & 1u];
	xSemaphoreGive(view_lock);
	return ok;
#else
	(void)index;
	(void)out;
	return false;
#endif
}
bool radio_get_dual(radio_dual_ref_t ref[2], uint8_t *sel, uint8_t *at)
{
#if CONFIG_SCAN
	if (view_lock == NULL) {
		return false;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	ref[0] = shared_dual[0];
	ref[1] = shared_dual[1];
	*sel = shared_dual_sel;
	*at = shared_dual_at;
	bool on = shared_dual_on;
	xSemaphoreGive(view_lock);
	return on;
#else
	(void)ref;
	(void)sel;
	(void)at;
	return false;
#endif
}
void radio_req_beep(uint8_t b) { post(RREQ_BEEP, b, 0); }
void radio_req_vibrate(uint8_t pattern) { post(RREQ_VIBRATE, pattern, 0); }
void radio_req_roam(uint8_t cmd) { post(RREQ_ROAM, cmd, 0); }
// A lost release would leave the burst keyed until the time-out
void radio_req_tone_burst(bool on) { post(RREQ_TONE_BURST, on, pdMS_TO_TICKS(50)); }

void radio_set_settings(const radio_settings_t *s)
{
	if (view_lock == NULL) {
		return;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	pending_settings = *s;
	xSemaphoreGive(view_lock);
	post(RREQ_SETTINGS, 0, pdMS_TO_TICKS(50));
}

void radio_get_settings(radio_settings_t *s)
{
	if (view_lock == NULL) {
		radio_settings_default(s);
		return;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	*s = shared_settings;
	xSemaphoreGive(view_lock);
}

void radio_req_power_off(void)
{
	power_off_busy = true;
	post(RREQ_POWER_OFF, 0, pdMS_TO_TICKS(50));
}

void radio_req_power_off_cancel(void)
{
	power_off_busy = false;
	post(RREQ_POWER_OFF, 1, pdMS_TO_TICKS(50));
}

bool radio_power_off_busy(void)
{
	return power_off_busy;
}

bool radio_power_off_wanted(void)
{
	return power_off_wanted;
}

uint8_t radio_backlight_pct(uint8_t brightness, uint32_t idle_ms)
{
	if (view_lock == NULL) {
		return brightness;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	uint16_t dim_s = shared_settings.backlight_dim_s, off_s = shared_settings.backlight_off_s;
	uint8_t level = shared_view.battery;
	xSemaphoreGive(view_lock);
	return batt_backlight_pct(brightness, dim_s, off_s, idle_ms, level);
}

#if CONFIG_HOTSPOT
uint32_t radio_hotspot_serial(void)
{
	return radio_hs_serial();
}
#endif
