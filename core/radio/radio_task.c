// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's task: a request queue in front of the state machine (radio_sm.c), a status poll every
// RADIO_POLL_MS, the DMR call stack every RADIO_TICK_MS, and a snapshot of the view that other tasks copy under a
// mutex. It also implements the hotspot's radio operations (hotspot_radio_ops_t): they post requests and never wait,
// so the host link's MMDVM replies stay fast (docs/testing/mmdvm-audit.md A-15).
#include <string.h>
#include "radio/radio.h"
#include "radio/radio_sm.h"
#include "radio/radio_trunk.h"
#include "hal_time.h"
#include "config.h"
#if CONFIG_HOTSPOT
#include "hotspot/hotspot.h"
#include "radio/radio_hotspot.h"
#endif
#if CONFIG_DMR
#include "dmr/call/dmr_call.h"
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
#if CONFIG_POSITION
#include "position/position.h"
#endif
#if CONFIG_SATELLITE_TLE
#include "satellite/service.h"
#endif
#if CONFIG_ZONE_WRITE
#include "codeplug/zone_edit.h"
#endif
#if CONFIG_AIR_RAW
#include "debuglink/debuglink_air.h"
#endif
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

// Task layout (see core/app/app.c for the whole picture)
#define RADIO_TASK_PRIORITY	4
#if CONFIG_DMR
#define RADIO_TASK_STACK	776	// words: the DMR call stack and the AMBE FEC run here (the deepest maximal build: 2744 B + margin)
#else
#define RADIO_TASK_STACK	512	// words
#endif
#define RADIO_QUEUE_LEN		16

static radio_sm_t sm RFOUNDRY_CPU_RAM;	// owned by the radio task; CPU only
static radio_settings_t pending_settings;	// radio_set_settings -> the radio task, under view_lock
static radio_settings_t shared_settings;	// the published copy, under view_lock
// Settings generations, under view_lock: radio_set_settings counts sent, the radio task notes the one it copied
// (taken) and publish() the one shared_settings now holds (published). While published lags sent, the last sent
// settings are the ones in force soon, so radio_get_settings answers with them
static uint32_t settings_sent, settings_taken, settings_published;
static radio_view_t shared_view;	// the published copy, under view_lock
#if CONFIG_SATELLITE_TLE
static radio_satellite_view_t shared_satellite_view;
// A static for now, not pvPortMalloc: the RTOS heap holds every task stack and, where it is only the CPU RAM the statics
// leave, has no room for this. Zeroed at start-up like any .bss
static satellite_service_t satellite_service_state;
static satellite_service_t *const satellite_service = &satellite_service_state;
static SemaphoreHandle_t satellite_lock;
#endif
#if CONFIG_P25
static radio_p25_settings_t pending_p25;	// radio_req_p25_settings -> the radio task, under view_lock
#endif
#if CONFIG_NXDN
static radio_nxdn_settings_t pending_nxdn;	// radio_req_nxdn_settings -> the radio task, under view_lock
#endif
#if CONFIG_AIR_RAW
static volatile bool bench_held, bench_keyed;	// published sm.air_bench, sm.bench_keyed
static volatile uint8_t bench_ack;		// published sm.bench_seq
static uint8_t bench_seq;			// the debug link task's
static const debuglink_air_core_t bench_core;	// below
#endif
#if CONFIG_DMR_TRUNK
static radio_t3_view_t shared_t3;	// ... and the whole trunking view (radio_get_t3_view)
#endif
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

static bool send(const radio_req_t *req, TickType_t wait)
{
	if (xQueueSend(requests, req, wait) != pdTRUE) {
		req_dropped++;	// the queue was full: the request is lost
		return false;
	}
	return true;
}

#if CONFIG_DMR
// The DMR stack's wake (dmr_call_set_wake): the end of a transmission is acted on at once, not at the next tick up to
// RADIO_TICK_MS later, so the receiver is back for a reply 30 ms after the last burst. One in the queue at a time.
// Only the modem's event callback posts it, from one task, so the flag needs no atomics
static volatile bool wake_posted;

static void wake(void)
{
	if (!wake_posted) {
		wake_posted = true;
		radio_req_t req = { .type = RREQ_WAKE };
		if (xQueueSend(requests, &req, 0) != pdTRUE) {
			wake_posted = false;	// the queue was full: the tick comes anyway
		}
	}
}
#endif

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
#if CONFIG_SATELLITE_TLE
	satellite_service_live_view_t satellite_live;
	satellite_service_live_view(satellite_service, &satellite_live);
#endif
	xSemaphoreTake(view_lock, portMAX_DELAY);
#if CONFIG_DMR_TRUNK
	radio_sm_t3_brief(&sm);
	shared_t3 = radio_t3v;
#endif
	shared_view = sm.view;
#if CONFIG_SATELLITE_TLE
	shared_satellite_view.tracking = sm.satellite_view;
	shared_satellite_view.valid = sm.satellite_view.tracking && satellite_live.pass_valid;
	shared_satellite_view.visible_now = shared_satellite_view.valid && satellite_live.pass_visible_now;
	shared_satellite_view.los_known = shared_satellite_view.valid && satellite_live.pass_los_known;
	shared_satellite_view.horizon_clipped =
		shared_satellite_view.valid && satellite_live.pass_horizon_clipped;
	shared_satellite_view.aos_in_s = satellite_live.aos_in_s;
	shared_satellite_view.los_in_s = satellite_live.los_in_s;
	shared_satellite_view.maximum_elevation_tenths = satellite_live.maximum_elevation_tenths;
	shared_satellite_view.elevation_tenths = satellite_live.elevation_tenths;
#endif
	dtmf_ready = sm.view.dtmf_ready;
	radio_sm_shown_view(&sm, &shared_view);	// a roaming survey visit: the screens keep the site it left
	shared_serial = sm.serial;
	shared_settings = sm.set;
	settings_published = settings_taken;
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
#if CONFIG_AIR_RAW
	bench_held = sm.air_bench;
	bench_keyed = sm.bench_keyed;
	bench_ack = sm.bench_seq;
#endif
}
static void apply_pending_settings(void)
{
	radio_settings_t set;
	xSemaphoreTake(view_lock, portMAX_DELAY);
	set = pending_settings;
	settings_taken = settings_sent;
	xSemaphoreGive(view_lock);
	radio_sm_set_settings(&sm, &set, hal_millis());
}


#if CONFIG_SATELLITE_TLE
static satellite_service_channel_t satellite_channel(void)
{
	return (satellite_service_channel_t){
		.vfo = sm.view.vfo,
		.vfo_index = sm.view.vfo_index,
		.zone_index = sm.view.zone_index,
		.zone_pos = sm.view.zone_pos,
		.channel_number = sm.view.channel_number,
		.hotspot = sm.view.hotspot,
		.scan = sm.view.scan,
	};
}

static void satellite_apply_update(const satellite_service_update_t *update, uint32_t now_ms)
{
	radio_sm_satellite_update(&sm, update->tracking, update->record_index, update->status,
				  update->nominal_rx_hz, update->nominal_tx_hz, update->tx_tone_tenths,
				  update->override_active, update->targets.rx_hz, update->targets.tx_hz,
				  update->targets.rx_offset_hz, update->targets.tx_offset_hz, now_ms);
}

static void satellite_request(bool enabled, uint8_t record_index, uint32_t now_ms)
{
	satellite_service_update_t update;
	satellite_service_channel_t channel = satellite_channel();
	satellite_service_tracking_request(satellite_service, enabled, record_index, now_ms, &channel, &update);
	satellite_apply_update(&update, now_ms);
}

static void satellite_scan_request(bool enabled, uint32_t now_ms)
{
	satellite_service_update_t update;
	bool changed = satellite_service_scan_request(satellite_service, enabled, now_ms, &update);
	if (changed)
		satellite_apply_update(&update, now_ms);
}

static void satellite_run(uint32_t now_ms)
{
	satellite_service_channel_t channel = satellite_channel();
	satellite_service_update_t update;
	satellite_service_scan_step(satellite_service, now_ms);
	bool changed = satellite_service_tick(satellite_service, now_ms, &channel, &update);
	if (changed)
		satellite_apply_update(&update, now_ms);
}
#endif


static void radio_task(void *arg)
{
	(void)arg;
#if CONFIG_SATELLITE_TLE
	satellite_service_init(satellite_service);
#endif
	TickType_t period = pdMS_TO_TICKS(RADIO_POLL_MS);
	TickType_t tick_period = pdMS_TO_TICKS(RADIO_TICK_MS);
	TickType_t next_poll = xTaskGetTickCount() + period;
	TickType_t next_tick = xTaskGetTickCount() + tick_period;

	for (;;) {
		TickType_t due = (int32_t)(next_tick - next_poll) < 0 ? next_tick : next_poll;
		int32_t left = (int32_t)(due - xTaskGetTickCount());
		radio_req_t req;
		bool woken = false;
		if (xQueueReceive(requests, &req, left > 0 ? (TickType_t)left : 0) == pdTRUE) {
			do {
				req_handled++;
				if (req.type == RREQ_WAKE) {
#if CONFIG_DMR
					wake_posted = false;
#endif
					woken = true;
#if CONFIG_P25
				} else if (req.type == RREQ_P25_SET) {
					radio_p25_settings_t p;
					xSemaphoreTake(view_lock, portMAX_DELAY);
					p = pending_p25;
					xSemaphoreGive(view_lock);
					radio_p25_set_settings(&p);
					radio_sm_request(&sm, &req, hal_millis());
#endif
#if CONFIG_NXDN
				} else if (req.type == RREQ_NXDN_SET) {
					radio_nxdn_settings_t n;
					xSemaphoreTake(view_lock, portMAX_DELAY);
					n = pending_nxdn;
					xSemaphoreGive(view_lock);
					radio_nxdn_set_settings(&n);
					radio_sm_request(&sm, &req, hal_millis());
#endif
				} else if (req.type == RREQ_SETTINGS) {
					apply_pending_settings();
				}
#if CONFIG_SATELLITE_TLE
				else if (req.type == RREQ_SATELLITE_TRACK) {
					satellite_request(req.arg != 0, (uint8_t)req.arg2, hal_millis());
				}
				else if (req.type == RREQ_SATELLITE_SCAN) {
					satellite_scan_request(req.arg != 0, hal_millis());
				}
#endif
				else {
					radio_sm_request(&sm, &req, hal_millis());
				}
			} while (xQueueReceive(requests, &req, 0) == pdTRUE);
		}
		TickType_t now = xTaskGetTickCount();
		if (woken || (int32_t)(now - next_tick) >= 0) {
			radio_sm_tick(&sm, hal_millis());
			next_tick += tick_period;
			if (woken || (int32_t)(now - next_tick) >= 0) {
				next_tick = now + tick_period;	// a wake's tick counts as the next one
			}
		}
		#if CONFIG_SATELLITE_TLE
		satellite_run(hal_millis());
		#endif
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

#if CONFIG_POSITION
// The position service's and the APRS engine's any-task calls (position.h, aprs.h): short copies, the same way
static void copy_lock(void) { vTaskSuspendAll(); }
static void copy_unlock(void) { (void)xTaskResumeAll(); }
#endif

#if CONFIG_SATELLITE_TLE
static void satellite_set_locked(bool locked)
{
	if (locked)
		xSemaphoreTake(satellite_lock, portMAX_DELAY);
	else
		xSemaphoreGive(satellite_lock);
}
#endif

void radio_core_start(void)
{
#if CONFIG_APRS
	aprs_set_lock(copy_lock, copy_unlock);
#endif
#if CONFIG_POSITION
	position_set_lock(copy_lock, copy_unlock);
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
#if CONFIG_SATELLITE_TLE
	satellite_lock = xSemaphoreCreateMutex();
	configASSERT(satellite_lock != NULL);
	satellite_service_set_lock(satellite_set_locked);
#endif
#if CONFIG_ZONE_WRITE
	// A zone add or delete a power cut interrupted: the saved zone indexes match the zones before the radio core reads
	// them. Here, not in radio_sm_init: its frame isn't live under the settings save down to the flash driver
	zone_edit_settle();
#endif
	radio_sm_init(&sm, hal_millis());
#if CONFIG_DMR
	dmr_call_set_wake(wake);
#endif
#if CONFIG_DMR_TRUNK
	radio_sm_t3_brief(&sm);
	shared_t3 = radio_t3v;
#endif
	shared_view = sm.view;
	shared_serial = sm.serial;
	shared_settings = sm.set;
#if CONFIG_HOTSPOT
	radio_hs_init(hs_post);
#endif
	xTaskCreate(radio_task, "radio", RADIO_TASK_STACK, NULL, RADIO_TASK_PRIORITY, NULL);
#if CONFIG_AIR_RAW
	debuglink_air_set_core(&bench_core);
#endif
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

void radio_get_channel_profile(channel_t *ch)
{
	if (ch == NULL)
		return;
	if (view_lock == NULL) {
		memset(ch, 0, sizeof(*ch));
		return;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	*ch = shared_view.ch;
#if CONFIG_SATELLITE_TLE
	if (shared_satellite_view.tracking.tracking) {
		if (shared_satellite_view.tracking.nominal_rx_hz != 0u)
			ch->rx_hz = shared_satellite_view.tracking.nominal_rx_hz;
		if (shared_satellite_view.tracking.nominal_tx_hz != 0u)
			ch->tx_hz = shared_satellite_view.tracking.nominal_tx_hz;
	}
#endif
	xSemaphoreGive(view_lock);
}

#if CONFIG_SATELLITE_TLE
bool radio_get_satellite_path(satellite_path_view_t *path)
{
	if (path == NULL)
		return false;
	if (satellite_lock == NULL) {
		memset(path, 0, sizeof(*path));
		return false;
	}
	xSemaphoreTake(satellite_lock, portMAX_DELAY);
	satellite_service_path_view(satellite_service, path);
	xSemaphoreGive(satellite_lock);
	return true;
}

bool radio_get_satellite_view(radio_satellite_view_t *view)
{
	if (view == NULL)
		return false;
	if (view_lock == NULL) {
		memset(view, 0, sizeof(*view));
		return false;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	*view = shared_satellite_view;
	xSemaphoreGive(view_lock);
	return true;
}

void radio_req_satellite_scan(bool enabled)
{
	if (requests == NULL)
		return;
	radio_req_t req = { .type = RREQ_SATELLITE_SCAN, .arg = enabled };
	send(&req, 0);
}

void radio_get_satellite_scan(uint8_t *status, uint8_t *completed, uint8_t *total, uint8_t *passes)
{
	if (satellite_lock == NULL) {
		if (status != NULL) *status = RADIO_SAT_SCAN_IDLE;
		if (completed != NULL) *completed = 0u;
		if (total != NULL) *total = 0u;
		if (passes != NULL) *passes = 0u;
		return;
	}
	xSemaphoreTake(satellite_lock, portMAX_DELAY);
	satellite_service_scan_state(satellite_service, status, completed, total, passes, NULL, NULL);
	xSemaphoreGive(satellite_lock);
}

bool radio_get_satellite_pass(uint8_t rank, radio_satellite_pass_t *pass)
{
	if (pass == NULL || satellite_lock == NULL)
		return false;
	xSemaphoreTake(satellite_lock, portMAX_DELAY);
	uint32_t packed;
	int64_t aos_s;
	int64_t scan_start_utc_s;
	uint32_t scan_start_ms;
	satellite_service_scan_state(satellite_service, NULL, NULL, NULL, NULL,
				    &scan_start_utc_s, &scan_start_ms);
	if (!satellite_service_scan_result(satellite_service, rank, &packed, &aos_s)) {
		xSemaphoreGive(satellite_lock);
		return false;
	}
	uint32_t now_ms = hal_millis();
	int64_t now_utc_s = scan_start_utc_s + (uint32_t)(now_ms - scan_start_ms) / 1000u;
	pass->record_index = satellite_pass_summary_index(packed);
	pass->aos_minutes = satellite_pass_eta_minutes(aos_s, now_utc_s);
	pass->maximum_elevation_deg = satellite_pass_summary_maximum_elevation(packed);
	pass->visible_now = satellite_pass_summary_visible_now(packed);
	xSemaphoreGive(satellite_lock);
	return true;
}

bool radio_get_satellite_pass_summary(uint8_t record_index, satellite_record_brief_t *summary)
{
	if (summary == NULL || satellite_lock == NULL)
		return false;
	xSemaphoreTake(satellite_lock, portMAX_DELAY);
	bool valid = satellite_service_summary(satellite_service, record_index, summary);
	xSemaphoreGive(satellite_lock);
	return valid;
}

bool radio_get_satellite_oldest_tle_epoch(uint32_t *epoch)
{
	if (epoch == NULL || satellite_lock == NULL)
		return false;
	xSemaphoreTake(satellite_lock, portMAX_DELAY);
	bool valid = satellite_service_oldest_tle_epoch(satellite_service, epoch);
	xSemaphoreGive(satellite_lock);
	return valid;
}

uint32_t radio_get_satellite_generation(void)
{
	if (satellite_lock == NULL)
		return satellite_store_generation();
	xSemaphoreTake(satellite_lock, portMAX_DELAY);
	uint32_t generation = satellite_service->store_generation;
	xSemaphoreGive(satellite_lock);
	return generation;
}
#endif

#if CONFIG_DMR_TRUNK
void radio_with_t3_view(void (*fn)(const radio_t3_view_t *x, void *ctx), void *ctx)
{
	if (view_lock == NULL) {
		fn(&shared_t3, ctx);	// all zero: the radio task hasn't started
		return;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	fn(&shared_t3, ctx);
	xSemaphoreGive(view_lock);
}
#endif

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

bool radio_get_zone_pos(uint16_t *zone, uint16_t *pos, uint16_t *size)
{
	if (view_lock == NULL) {
		*zone = 0;
		*pos = 0;
		*size = 0;
		return false;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	*zone = shared_view.zone_index;
	*pos = shared_view.zone_pos;
	*size = shared_view.zone_size;
	bool ready = shared_view.state != RS_TX && !shared_view.hotspot;	// radio_sm_request's busy
	xSemaphoreGive(view_lock);
	return ready;
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
#if CONFIG_SATELLITE_TLE
bool radio_req_satellite_tracking(bool enabled, uint8_t record_index)
{
	if (requests == NULL)
		return false;
	radio_req_t req = { .type = RREQ_SATELLITE_TRACK, .arg = enabled, .arg2 = record_index };
	return send(&req, 0);
}
void radio_req_satellite_arm(void) { post(RREQ_SATELLITE_ARM, 0, 0); }
#endif

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
void radio_req_vfo_tx(uint32_t hz) { post(RREQ_VFO_TX, (int32_t)hz, 0); }
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
void radio_req_volume_hold(bool hold) { post(RREQ_VOLUME_HOLD, hold, pdMS_TO_TICKS(50)); }
void radio_req_clear_temporary(void) { post(RREQ_CLEAR_TEMP, 0, 0); }
void radio_req_retune(void) { post(RREQ_RETUNE, 0, pdMS_TO_TICKS(50)); }
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
void radio_req_vfo_sfr(uint8_t a) { post(RREQ_VFO_SFR, a, 0); }
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

static void post_pair(uint8_t type, int32_t arg, uint32_t arg2, TickType_t wait)
{
	if (requests == NULL) {
		return;
	}
	radio_req_t req = { .type = type, .arg = arg, .arg2 = arg2 };
	send(&req, wait);
}

#if CONFIG_DMR || CONFIG_P25 || CONFIG_NXDN
void radio_req_mode(uint8_t mode) { post(RREQ_MODE, mode, 0); }
#endif
#if CONFIG_P25
void radio_req_p25_settings(const radio_p25_settings_t *s)
{
	if (view_lock == NULL) {
		return;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	pending_p25 = *s;
	xSemaphoreGive(view_lock);
	post(RREQ_P25_SET, 0, pdMS_TO_TICKS(50));
}
#endif
#if CONFIG_NXDN
void radio_req_nxdn_settings(const radio_nxdn_settings_t *s)
{
	if (view_lock == NULL) {
		return;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	pending_nxdn = *s;
	xSemaphoreGive(view_lock);
	post(RREQ_NXDN_SET, 0, pdMS_TO_TICKS(50));
}
#endif

#if CONFIG_AIR_RAW
// The air interface bench's way to the radio (debuglink_air_core_t), from the debug link task. Enter and keying wait
// for the radio task's answer: it runs above the debug link, so it is normally there when the post returns, but a
// retune or a keying may sleep (a synthesizer locking). The unkey comes from the modem's task too: it only posts
static bool bench_wait(uint8_t type, int32_t arg, uint32_t arg2)
{
	uint8_t seq = ++bench_seq;
	post_pair(type, arg | ((int32_t)seq << 8), arg2, pdMS_TO_TICKS(50));
	for (int i = 0; i < 50 && bench_ack != seq; i++) {
		vTaskDelay(pdMS_TO_TICKS(2));
	}
	return bench_ack == seq;
}

static bool bench_enter(void)
{
	return bench_held || (bench_wait(RREQ_AIR_BENCH, 1, 0) && bench_held);
}

static void bench_leave(void)
{
	post_pair(RREQ_AIR_BENCH, 0, 0, pdMS_TO_TICKS(50));	// must not be lost: the channel comes back with it
}

static bool bench_tx(bool on, uint16_t max_ms)
{
	if (!on) {
		post_pair(RREQ_AIR_TX, 0, 0, 0);
		return true;
	}
	return bench_wait(RREQ_AIR_TX, 1, max_ms) && bench_keyed;
}

static const debuglink_air_core_t bench_core = { bench_enter, bench_leave, bench_tx };
#endif

void radio_req_scan(uint8_t cmd) { post(RREQ_SCAN, cmd, 0); }
void radio_req_scan_vfo(uint32_t lo_hz, uint32_t hi_hz) { post_pair(RREQ_SCAN_VFO, (int32_t)lo_hz, hi_hz, 0); }
void radio_req_scan_dual(uint16_t pos) { post_pair(RREQ_SCAN, RSCAN_DUAL, pos, 0); }
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
		post_pair(RREQ_DUAL, 0, 0, 0);
		return;
	}
	post_pair(RREQ_DUAL, (int32_t)radio_dual_arg(&ref[0], sel == 0), radio_dual_arg(&ref[1], sel != 0), 0);
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
void radio_req_beep_after(uint8_t b, uint32_t delay_ms) { post_pair(RREQ_BEEP, b, delay_ms, 0); }
void radio_req_vibrate(uint8_t pattern) { post(RREQ_VIBRATE, pattern, 0); }
void radio_req_roam(uint8_t cmd) { post(RREQ_ROAM, cmd, 0); }
void radio_req_t3(uint8_t cmd) { post(RREQ_T3, cmd, 0); }
void radio_req_t3_poll(uint32_t dst) { post_pair(RREQ_T3_POLL, 0, dst, 0); }
void radio_req_t3_dial(const char *digits)
{
#if CONFIG_DMR_TRUNK_GW
	uint32_t w[2];
	radio_t3_dial_args(digits, w);
	post_pair(RREQ_T3_DIAL, (int32_t)w[0], w[1], 0);
#else
	(void)digits;
#endif
}
// A lost release would leave the burst keyed until the time-out
void radio_req_tone_burst(bool on, uint32_t max_ms) { post_pair(RREQ_TONE_BURST, on, max_ms, pdMS_TO_TICKS(50)); }
void radio_req_tone_burst_key(bool down) { post(RREQ_TONE_BURST_KEY, down, pdMS_TO_TICKS(50)); }

void radio_set_settings(const radio_settings_t *s)
{
	if (view_lock == NULL) {
		return;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	pending_settings = *s;
	settings_sent++;
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
	*s = settings_published == settings_sent ? shared_settings : pending_settings;
	xSemaphoreGive(view_lock);
}

void radio_get_sfr_settings(uint8_t *rx, uint8_t *cwid)
{
	*rx = 0;		// the defaults (radio_settings_default), before the task runs
	*cwid = 0;
	if (view_lock == NULL) {
		return;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	const radio_settings_t *p = settings_published == settings_sent ? &shared_settings : &pending_settings;
	*rx = p->sfr_rx;
	*cwid = p->sfr_cwid;
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
