// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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
#include "satellite/frequency.h"
#include "satellite/tracking.h"
#include "satellite/store.h"
#endif
#if CONFIG_ZONE_WRITE
#include "codeplug/zone_edit.h"
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

static radio_sm_t sm RFOUNDRY_CPU_RAM;	// owned by the radio task; CPU only
static radio_settings_t pending_settings;	// radio_set_settings -> the radio task, under view_lock
static radio_settings_t shared_settings;	// the published copy, under view_lock
// Settings generations, under view_lock: radio_set_settings counts sent, the radio task notes the one it copied
// (taken) and publish() the one shared_settings now holds (published). While published lags sent, the last sent
// settings are the ones in force soon, so radio_get_settings answers with them
static uint32_t settings_sent, settings_taken, settings_published;
static radio_view_t shared_view;	// the published copy, under view_lock
#if CONFIG_SATELLITE_TLE
static satellite_path_view_t shared_satellite_path;
#endif
#if CONFIG_SATELLITE_TLE
static satellite_tracking_t satellite_tracking;
static bool satellite_requested;
static uint8_t satellite_record_index;
static bool satellite_channel_vfo;
static uint8_t satellite_channel_vfo_index;
static uint16_t satellite_channel_zone, satellite_channel_pos, satellite_channel_number;
static uint32_t satellite_next_work_ms;
#define SATELLITE_SCAN_MAX_RECORDS 10u
static uint32_t satellite_scan_results[SATELLITE_SCAN_MAX_RECORDS];
static int64_t satellite_scan_start_utc_us;
static int32_t satellite_scan_latitude_e7, satellite_scan_longitude_e7;
static int16_t satellite_scan_altitude_m;
static uint8_t satellite_scan_status, satellite_scan_completed, satellite_scan_total;
static uint8_t satellite_scan_result_count, satellite_scan_record_index;
static bool satellite_scan_search_started;
static uint32_t satellite_scan_completed_ms;
static bool satellite_scan_cache_valid;
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

static void send(const radio_req_t *req, TickType_t wait)
{
	if (xQueueSend(requests, req, wait) != pdTRUE) {
		req_dropped++;	// the queue was full: the request is lost
	}
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
	xSemaphoreTake(view_lock, portMAX_DELAY);
	shared_view = sm.view;
#if CONFIG_SATELLITE_TLE
	const satellite_pass_t *next_pass = satellite_tracking_next_pass(&satellite_tracking);
	shared_view.satellite_pass_valid = sm.view.satellite_tracking && satellite_tracking.enabled &&
		next_pass != NULL;
	shared_view.satellite_los_known = shared_view.satellite_pass_valid && next_pass->los_us != 0;
	shared_view.satellite_pass_visible_now = shared_view.satellite_pass_valid &&
		next_pass->visible_now;
	shared_view.satellite_pass_horizon_clipped = shared_view.satellite_pass_valid &&
		next_pass->horizon_clipped;
	shared_view.satellite_aos_in_s = 0u;
	shared_view.satellite_los_in_s = 0u;
	shared_satellite_path.count = shared_view.satellite_pass_valid ? satellite_tracking.pass_search.path_count : 0u;
	if (shared_view.satellite_pass_valid) {
		memcpy(shared_satellite_path.points, satellite_tracking.pass_search.path,
		       (size_t)shared_satellite_path.count * sizeof(shared_satellite_path.points[0]));
		shared_satellite_path.azimuth_deg = (uint16_t)(satellite_tracking.observation.azimuth_deg + 0.5);
		if (shared_satellite_path.azimuth_deg >= 360u)
			shared_satellite_path.azimuth_deg = 0u;
	}
	if (shared_view.satellite_pass_valid && next_pass->aos_us > satellite_tracking.last_utc_us) {
		int64_t seconds = (next_pass->aos_us - satellite_tracking.last_utc_us) / 1000000;
		shared_view.satellite_aos_in_s = seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
	}
	if (shared_view.satellite_pass_valid && next_pass->los_us > satellite_tracking.last_utc_us) {
		int64_t seconds = (next_pass->los_us - satellite_tracking.last_utc_us) / 1000000;
		shared_view.satellite_los_in_s = seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
	}
	shared_view.satellite_pass_visible_now = shared_view.satellite_pass_valid &&
		satellite_tracking.observation.elevation_deg >= 0.0 &&
		satellite_tracking.last_utc_us >= next_pass->aos_us &&
		(next_pass->los_us == 0 || satellite_tracking.last_utc_us <= next_pass->los_us);
	if (shared_view.satellite_pass_valid && !shared_view.satellite_pass_visible_now &&
	    next_pass->aos_us > satellite_tracking.last_utc_us) {
		shared_view.satellite_pass_visible_now = false;
	}
	shared_view.satellite_max_elevation_tenths = shared_view.satellite_pass_valid ?
		(uint16_t)(next_pass->maximum_elevation_deg * 10.0 + 0.5) : 0u;
	shared_view.satellite_elevation_tenths = satellite_tracking.enabled ?
		(int16_t)(satellite_tracking.observation.elevation_deg * 10.0 +
			 (satellite_tracking.observation.elevation_deg >= 0.0 ? 0.5 : -0.5)) : 0;
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
static void satellite_capture_channel(void)
{
	satellite_channel_vfo = sm.view.vfo;
	satellite_channel_vfo_index = sm.view.vfo_index;
	satellite_channel_zone = sm.view.zone_index;
	satellite_channel_pos = sm.view.zone_pos;
	satellite_channel_number = sm.view.channel_number;
}

static bool satellite_channel_matches(void)
{
	return sm.view.vfo == satellite_channel_vfo && sm.view.vfo_index == satellite_channel_vfo_index &&
	       sm.view.zone_index == satellite_channel_zone && sm.view.zone_pos == satellite_channel_pos &&
	       sm.view.channel_number == satellite_channel_number;
}

static void satellite_stop(uint32_t now_ms)
{
	satellite_requested = false;
	satellite_tracking_stop(&satellite_tracking);
	radio_sm_satellite_update(&sm, false, 0u, 0u, 0u, 0u, 0u, false, 0u, 0u, 0, 0, now_ms);
}

static void satellite_scan_status_set(uint8_t status)
{
	if (view_lock == NULL)
		return;
	xSemaphoreTake(view_lock, portMAX_DELAY);
	satellite_scan_status = status;
	xSemaphoreGive(view_lock);
}

static void satellite_scan_finish(uint32_t now_ms)
{
	if (view_lock == NULL)
		return;
	xSemaphoreTake(view_lock, portMAX_DELAY);
	satellite_scan_status = RADIO_SAT_SCAN_COMPLETE;
	satellite_scan_completed_ms = now_ms;
	satellite_scan_cache_valid = true;
	xSemaphoreGive(view_lock);
}

static void satellite_scan_request(bool enabled, uint32_t now_ms)
{
	if (!enabled) {
		satellite_scan_search_started = false;
		if (satellite_scan_status == RADIO_SAT_SCAN_RUNNING)
			satellite_scan_status_set(RADIO_SAT_SCAN_IDLE);
		return;
	}
	if (view_lock != NULL) {
		xSemaphoreTake(view_lock, portMAX_DELAY);
		bool cache_fresh = satellite_pass_cache_fresh(satellite_scan_cache_valid, now_ms,
							      satellite_scan_completed_ms);
		bool scan_running = satellite_scan_status == RADIO_SAT_SCAN_RUNNING;
		xSemaphoreGive(view_lock);
		if (cache_fresh || scan_running)
			return;
	}
	if (satellite_requested)
		satellite_stop(now_ms);
	satellite_tracking_stop(&satellite_tracking);
	satellite_scan_search_started = false;
	position_fix_t fix;
	uint32_t date_ddmmyy, utc_hms;
	int64_t utc_us;
#if CONFIG_POSITION
	uint8_t source = position_fix_for_satellite(&fix, now_ms);
	if (source == POSITION_SRC_NONE || fix.alt_m == POSITION_ALT_NONE ||
	    !position_utc_fields(&date_ddmmyy, &utc_hms, now_ms) ||
	    !satellite_utc_from_fields(date_ddmmyy, utc_hms, &utc_us)) {
		satellite_scan_status_set(RADIO_SAT_SCAN_UNAVAILABLE);
		return;
	}
#else
	(void)fix;
	(void)date_ddmmyy;
	(void)utc_hms;
	(void)utc_us;
	satellite_scan_status_set(RADIO_SAT_SCAN_UNAVAILABLE);
	return;
#endif
	xSemaphoreTake(view_lock, portMAX_DELAY);
	satellite_scan_start_utc_us = utc_us;
	satellite_scan_latitude_e7 = fix.lat;
	satellite_scan_longitude_e7 = fix.lon;
	satellite_scan_altitude_m = fix.alt_m;
	satellite_scan_record_index = 0u;
	satellite_scan_completed = 0u;
	satellite_scan_total = satellite_store_count();
	satellite_scan_result_count = 0u;
	satellite_scan_status = RADIO_SAT_SCAN_RUNNING;
	satellite_next_work_ms = now_ms;
	xSemaphoreGive(view_lock);
	if (satellite_scan_total == 0u)
		satellite_scan_finish(now_ms);
}

static void satellite_scan_add_result(uint8_t index, const satellite_pass_t *pass)
{
	int64_t delta = pass->aos_us - satellite_scan_start_utc_us;
	uint32_t eta_minutes = delta <= 0 ? 0u : (uint32_t)((delta + INT64_C(59999999)) / INT64_C(60000000));
	if (eta_minutes > 1440u)
		eta_minutes = 1440u;
	uint32_t maximum_elevation = (uint32_t)(pass->maximum_elevation_deg + 0.5);
	if (maximum_elevation > 90u)
		maximum_elevation = 90u;
	uint32_t packed = satellite_pass_summary_pack(index, (uint16_t)eta_minutes,
						      (uint8_t)maximum_elevation, pass->visible_now);
	xSemaphoreTake(view_lock, portMAX_DELAY);
	satellite_pass_summary_insert(satellite_scan_results, &satellite_scan_result_count,
				      SATELLITE_SCAN_MAX_RECORDS, packed);
	xSemaphoreGive(view_lock);
}

static void satellite_scan_advance_record(void)
{
	xSemaphoreTake(view_lock, portMAX_DELAY);
	if (satellite_scan_completed < satellite_scan_total)
		satellite_scan_completed++;
	xSemaphoreGive(view_lock);
	satellite_scan_record_index++;
	satellite_scan_search_started = false;
}

__attribute__((noinline)) static void satellite_scan_service(uint32_t now_ms)
{
	if (satellite_scan_status != RADIO_SAT_SCAN_RUNNING)
		return;
	if ((int32_t)(now_ms - satellite_next_work_ms) < 0)
		return;
	if (!satellite_scan_search_started) {
		if (satellite_scan_record_index >= satellite_scan_total) {
			satellite_scan_finish(now_ms);
			return;
		}
#if CONFIG_POSITION
		if (!satellite_tracking_start_scan(&satellite_tracking, satellite_scan_record_index)) {
			satellite_scan_advance_record();
			satellite_next_work_ms = hal_millis() + RADIO_TICK_MS;
			return;
		}
		satellite_tracking.enabled = false;
		if (!satellite_pass_search_begin(&satellite_tracking.pass_search, &satellite_tracking.orbit,
						 satellite_scan_start_utc_us, SATELLITE_PASS_MAX_HORIZON_HOURS,
						 satellite_scan_latitude_e7, satellite_scan_longitude_e7,
						 satellite_scan_altitude_m)) {
			satellite_scan_advance_record();
			satellite_next_work_ms = hal_millis() + RADIO_TICK_MS;
			return;
		}
		satellite_scan_search_started = true;
		satellite_next_work_ms = hal_millis() + RADIO_TICK_MS;
		return;
#else
		(void)now_ms;
		satellite_scan_status_set(RADIO_SAT_SCAN_UNAVAILABLE);
		return;
#endif
	}
#if CONFIG_POSITION
	satellite_pass_status_t pass_status = satellite_pass_search_step(&satellite_tracking.pass_search,
		&satellite_tracking.orbit, satellite_scan_latitude_e7,
		satellite_scan_longitude_e7, satellite_scan_altitude_m,
		SATELLITE_PASS_SCAN_SAMPLES_PER_STEP);
	satellite_next_work_ms = hal_millis() + RADIO_TICK_MS;
	if (pass_status == SATELLITE_PASS_PENDING)
		return;
	if (pass_status == SATELLITE_PASS_COMPLETE && satellite_tracking.pass_search.pass_valid)
		satellite_scan_add_result(satellite_scan_record_index, &satellite_tracking.pass_search.pass);
	satellite_scan_advance_record();
#endif
}

__attribute__((noinline)) static void satellite_request(bool enabled, uint8_t record_index, uint32_t now_ms)
{
	satellite_scan_request(false, now_ms);
	if (!enabled) {
		satellite_stop(now_ms);
		return;
	}
	satellite_tracking_stop(&satellite_tracking);
	satellite_record_summary_t record;
	satellite_frequency_profile_t profile;
	if (!satellite_store_summary_get(record_index, &record) ||
	    !satellite_frequency_profile(record.catalog_id, &profile)) {
		satellite_requested = false;
		radio_sm_satellite_update(&sm, false, 0u, 0u, 0u, 0u, 0u, false, 0u, 0u, 0, 0, now_ms);
		return;
	}
	satellite_requested = true;
	satellite_record_index = record_index;
	satellite_capture_channel();
	(void)satellite_tracking_start(&satellite_tracking, record_index, profile.rx_hz, profile.tx_hz);
	if (sm.view.hotspot || sm.view.scan != 0u) {
		satellite_stop(now_ms);
		return;
	}
	radio_sm_satellite_update(&sm, true, record_index, SATELLITE_TRACKING_UNAVAILABLE,
				  profile.rx_hz, profile.tx_hz, profile.tx_tone_tenths,
				  false, 0u, 0u, 0, 0, now_ms);
	satellite_next_work_ms = now_ms;
}

__attribute__((noinline)) static void satellite_service(uint32_t now_ms)
{
	if (!satellite_requested)
		return;
	if (!satellite_channel_matches() || sm.view.hotspot || sm.view.scan != 0u) {
		satellite_stop(now_ms);
		return;
	}
	if (satellite_tracking.enabled && satellite_tracking.pass_search_started &&
	    satellite_tracking.pass_search.status == SATELLITE_PASS_PENDING)
		satellite_tracking_forecast_step(&satellite_tracking);
	if ((int32_t)(now_ms - satellite_next_work_ms) < 0)
		return;
	satellite_next_work_ms = now_ms + 1000u;
	satellite_tracking_status_t status = satellite_tracking.enabled ?
		satellite_tracking_tick(&satellite_tracking, now_ms, 10u) : SATELLITE_TRACKING_UNAVAILABLE;
	radio_sm_satellite_update(&sm, true, satellite_record_index, status,
				  satellite_tracking.nominal_rx_hz, satellite_tracking.nominal_tx_hz,
				  0u, satellite_tracking.override_active,
				  satellite_tracking.targets.rx_hz, satellite_tracking.targets.tx_hz,
				  satellite_tracking.targets.rx_offset_hz, satellite_tracking.targets.tx_offset_hz, now_ms);
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
		bool woken = false;
		if (xQueueReceive(requests, &req, left > 0 ? (TickType_t)left : 0) == pdTRUE) {
			do {
				req_handled++;
				if (req.type == RREQ_WAKE) {
#if CONFIG_DMR
					wake_posted = false;
#endif
					woken = true;
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
		satellite_scan_service(hal_millis());
		satellite_service(hal_millis());
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
#if CONFIG_ZONE_WRITE
	// A zone add or delete a power cut interrupted: the saved zone indexes match the zones before the radio core reads
	// them. Here, not in radio_sm_init: its frame isn't live under the settings save down to the flash driver
	zone_edit_settle();
#endif
	radio_sm_init(&sm, hal_millis());
#if CONFIG_DMR
	dmr_call_set_wake(wake);
#endif
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
	if (shared_view.satellite_tracking) {
		if (shared_view.satellite_nominal_rx_hz != 0u)
			ch->rx_hz = shared_view.satellite_nominal_rx_hz;
		if (shared_view.satellite_nominal_tx_hz != 0u)
			ch->tx_hz = shared_view.satellite_nominal_tx_hz;
	}
#endif
	xSemaphoreGive(view_lock);
}

#if CONFIG_SATELLITE_TLE
bool radio_get_satellite_path(satellite_path_view_t *path)
{
	if (path == NULL)
		return false;
	if (view_lock == NULL) {
		memset(path, 0, sizeof(*path));
		return false;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	*path = shared_satellite_path;
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
	if (view_lock == NULL) {
		if (status != NULL) *status = RADIO_SAT_SCAN_IDLE;
		if (completed != NULL) *completed = 0u;
		if (total != NULL) *total = 0u;
		if (passes != NULL) *passes = 0u;
		return;
	}
	xSemaphoreTake(view_lock, portMAX_DELAY);
	if (status != NULL) *status = satellite_scan_status;
	if (completed != NULL) *completed = satellite_scan_completed;
	if (total != NULL) *total = satellite_scan_total;
	if (passes != NULL) *passes = satellite_scan_result_count;
	xSemaphoreGive(view_lock);
}

bool radio_get_satellite_pass(uint8_t rank, radio_satellite_pass_t *pass)
{
	if (pass == NULL || view_lock == NULL)
		return false;
	xSemaphoreTake(view_lock, portMAX_DELAY);
	if (rank >= satellite_scan_result_count) {
		xSemaphoreGive(view_lock);
		return false;
	}
	uint32_t packed = satellite_scan_results[rank];
	pass->record_index = satellite_pass_summary_index(packed);
	pass->aos_minutes = satellite_pass_summary_aos_minutes(packed);
	pass->maximum_elevation_deg = satellite_pass_summary_maximum_elevation(packed);
	pass->visible_now = satellite_pass_summary_visible_now(packed);
	xSemaphoreGive(view_lock);
	return true;
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
void radio_req_satellite_tracking(bool enabled, uint8_t record_index)
{
	if (requests == NULL)
		return;
	radio_req_t req = { .type = RREQ_SATELLITE_TRACK, .arg = enabled, .arg2 = record_index };
	send(&req, 0);
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
void radio_req_mode(uint8_t mode) { post(RREQ_MODE, mode, 0); }
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
