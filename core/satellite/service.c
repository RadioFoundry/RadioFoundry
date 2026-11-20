// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pass-scan and tracking request coordination without RTOS or radio-state-machine dependencies.
#include <string.h>
#include "config.h"
#include "position/position.h"
#include "satellite/frequency.h"
#include "satellite/pass.h"
#include "satellite/service.h"

#define SERVICE_TICK_MS 10u
#define SERVICE_TRACKING_PERIOD_MS 1000u

static void (*service_lock)(bool locked);

void satellite_service_set_lock(void (*set_locked)(bool locked))
{
	service_lock = set_locked;
}

static void lock(void)
{
	if (service_lock != NULL)
		service_lock(true);
}

static void unlock(void)
{
	if (service_lock != NULL)
		service_lock(false);
}

static void satellite_stop(satellite_service_t *service, satellite_service_update_t *update)
{
	service->tracking_requested = false;
	satellite_tracking_stop(&service->tracking);
	update->tracking = false;
	update->record_index = 0u;
	update->status = SATELLITE_TRACKING_OFF;
	update->nominal_rx_hz = 0u;
	update->nominal_tx_hz = 0u;
	update->tx_tone_tenths = 0u;
	update->override_active = false;
	memset(&update->targets, 0, sizeof(update->targets));
}

static void satellite_update(const satellite_service_t *service, uint8_t status, uint16_t tone_tenths,
			     satellite_service_update_t *update)
{
	update->tracking = service->tracking_requested;
	update->record_index = service->record_index;
	update->status = status;
	update->nominal_rx_hz = service->tracking.nominal_rx_hz;
	update->nominal_tx_hz = service->tracking.nominal_tx_hz;
	update->tx_tone_tenths = tone_tenths;
	update->override_active = service->tracking.override_active;
	update->targets = service->tracking.targets;
}

static bool satellite_channel_matches(const satellite_service_t *service,
				      const satellite_service_channel_t *channel)
{
	return channel != NULL && channel->vfo == service->channel_vfo &&
	       channel->vfo_index == service->channel_vfo_index && channel->zone_index == service->channel_zone &&
	       channel->zone_pos == service->channel_pos && channel->channel_number == service->channel_number;
}

static void satellite_capture_channel(satellite_service_t *service,
				      const satellite_service_channel_t *channel)
{
	service->channel_vfo = channel->vfo;
	service->channel_vfo_index = channel->vfo_index;
	service->channel_zone = channel->zone_index;
	service->channel_pos = channel->zone_pos;
	service->channel_number = channel->channel_number;
}

static bool satellite_refresh_inventory(satellite_service_t *service, uint32_t generation)
{
	uint8_t count = satellite_store_count();
	lock();
	service->scan_total = 0u;
	memset(service->scan_summary_names, 0, sizeof(service->scan_summary_names));
	service->scan_oldest_tle_epoch = 0u;
	unlock();
	for (uint8_t i = 0u; i < count; i++) {
		satellite_record_summary_t summary;
		if (!satellite_store_summary_get(i, &summary))
			goto unavailable;
		uint8_t length = summary.name_length;
		if (length > SATELLITE_RECORD_BRIEF_NAME_MAX) {
			length = SATELLITE_RECORD_BRIEF_NAME_MAX;
			while (length > 0u && ((uint8_t)summary.name[length] & 0xC0u) == 0x80u)
				length--;
		}
		lock();
		memcpy(service->scan_summary_names[i], summary.name, length);
		if (i == 0u || summary.epoch < service->scan_oldest_tle_epoch)
			service->scan_oldest_tle_epoch = summary.epoch;
		unlock();
	}
	if (satellite_store_generation() != generation)
		goto unavailable;
	lock();
	service->scan_total = count;
	unlock();
	return true;

unavailable:
	lock();
	memset(service->scan_summary_names, 0, sizeof(service->scan_summary_names));
	service->scan_oldest_tle_epoch = 0u;
	service->scan_total = count;
	service->scan_cache_valid = false;
	service->scan_status = SATELLITE_SERVICE_SCAN_UNAVAILABLE;
	unlock();
	return false;
}

static bool satellite_refresh_store_if_changed(satellite_service_t *service, bool *inventory_ok)
{
	uint32_t generation = satellite_store_generation();
	*inventory_ok = true;
	if (generation == service->store_generation)
		return false;
	lock();
	service->store_generation = generation;
	service->scan_cache_valid = false;
	service->scan_status = SATELLITE_SERVICE_SCAN_IDLE;
	service->scan_completed = 0u;
	service->scan_total = 0u;
	service->scan_result_count = 0u;
	service->scan_search_started = false;
	memset(service->scan_results, 0, sizeof(service->scan_results));
	unlock();
	*inventory_ok = satellite_refresh_inventory(service, generation);
	return true;
}
void satellite_service_init(satellite_service_t *service)
{
	if (service == NULL)
		return;
	lock();
	memset(service, 0, sizeof(*service));
	service->scan_status = SATELLITE_SERVICE_SCAN_IDLE;
	satellite_tracking_stop(&service->tracking);
	unlock();
	uint32_t generation = satellite_store_generation();
	lock();
	service->store_generation = generation;
	unlock();
	(void)satellite_refresh_inventory(service, generation);
}

void satellite_service_tracking_request(satellite_service_t *service, bool enabled, uint8_t record_index,
					uint32_t now_ms, const satellite_service_channel_t *channel,
					satellite_service_update_t *update)
{
	if (service == NULL || update == NULL)
		return;
	bool inventory_ok;
	(void)satellite_refresh_store_if_changed(service, &inventory_ok);
	lock();
	service->scan_search_started = false;
	if (service->scan_status == SATELLITE_SERVICE_SCAN_RUNNING)
		service->scan_status = SATELLITE_SERVICE_SCAN_IDLE;
	if (!enabled || !inventory_ok) {
		satellite_stop(service, update);
		unlock();
		return;
	}
	satellite_stop(service, update);
	unlock();
	satellite_record_summary_t record;
	satellite_frequency_profile_t profile;
	if (!satellite_store_summary_get(record_index, &record) ||
	    !satellite_store_profile_get(record.catalog_id, &profile) ||
	    !satellite_store_get(record_index, &service->tracking.record_scratch) ||
	    satellite_store_generation() != service->store_generation)
		return;
	lock();
	service->record_index = record_index;
	if (channel != NULL)
		satellite_capture_channel(service, channel);
	if (channel == NULL || channel->hotspot || channel->scan != 0u ||
	    !satellite_tracking_start_record(&service->tracking, record_index, profile.rx_hz, profile.tx_hz,
					     &service->tracking.record_scratch)) {
		satellite_stop(service, update);
		unlock();
		return;
	}
	service->tracking_requested = true;
	satellite_update(service, SATELLITE_TRACKING_UNAVAILABLE, profile.tx_tone_tenths, update);
	service->next_tracking_ms = now_ms;
	unlock();
}

static void satellite_scan_finish(satellite_service_t *service, uint32_t now_ms)
{
	service->scan_status = SATELLITE_SERVICE_SCAN_COMPLETE;
	service->scan_completed_ms = now_ms;
	service->scan_cache_valid = true;
}

bool satellite_service_scan_request(satellite_service_t *service, bool enabled, uint32_t now_ms,
				    satellite_service_update_t *update)
{
	if (service == NULL)
		return false;
	bool inventory_ok;
	bool store_changed = satellite_refresh_store_if_changed(service, &inventory_ok);
	bool stopped_tracking = false;
	lock();
	if (store_changed && service->tracking_requested) {
		if (update != NULL)
			satellite_stop(service, update);
		else {
			service->tracking_requested = false;
			satellite_tracking_stop(&service->tracking);
		}
		stopped_tracking = true;
	}
	if (!enabled) {
		service->scan_search_started = false;
		if (service->scan_status == SATELLITE_SERVICE_SCAN_RUNNING)
			service->scan_status = SATELLITE_SERVICE_SCAN_IDLE;
		unlock();
		return stopped_tracking;
	}
	if (!inventory_ok) {
		unlock();
		return stopped_tracking;
	}
	if (satellite_pass_cache_fresh(service->scan_cache_valid, now_ms, service->scan_completed_ms) ||
	    service->scan_status == SATELLITE_SERVICE_SCAN_RUNNING) {
		unlock();
		return stopped_tracking;
	}
	stopped_tracking = stopped_tracking || service->tracking_requested;
	if (stopped_tracking && update != NULL)
		satellite_stop(service, update);
	else if (stopped_tracking)
		satellite_tracking_stop(&service->tracking);
	service->tracking_requested = false;
	service->scan_search_started = false;

	service->scan_start_ms = now_ms;
	service->scan_completed = 0u;
	service->scan_result_count = 0u;
	service->scan_status = SATELLITE_SERVICE_SCAN_RUNNING;
	unlock();
	if (!store_changed)
		(void)satellite_refresh_inventory(service, service->store_generation);
	lock();
	if (service->scan_status == SATELLITE_SERVICE_SCAN_UNAVAILABLE) {
		unlock();
		return stopped_tracking;
	}
	if (service->scan_total == 0u) {
		satellite_scan_finish(service, now_ms);
		unlock();
		return stopped_tracking;
	}
	unlock();

	position_fix_t fix;
	uint32_t date_ddmmyy, utc_hms;
	int64_t utc_s;
#if CONFIG_POSITION
	uint8_t source = position_fix_for_satellite(&fix, now_ms);
	if (source == POSITION_SRC_NONE || fix.alt_m == POSITION_ALT_NONE ||
	    !position_utc_fields(&date_ddmmyy, &utc_hms, now_ms) ||
	    !satellite_utc_from_fields(date_ddmmyy, utc_hms, &utc_s)) {
		lock();
		service->scan_status = SATELLITE_SERVICE_SCAN_UNAVAILABLE;
		unlock();
		return stopped_tracking;
	}
#else
	(void)fix;
	(void)date_ddmmyy;
	(void)utc_hms;
	(void)utc_s;
	lock();
	service->scan_status = SATELLITE_SERVICE_SCAN_UNAVAILABLE;
	unlock();
	return stopped_tracking;
#endif
	lock();
	service->scan_start_utc_s = utc_s;
	service->scan_latitude_e7 = fix.lat;
	service->scan_longitude_e7 = fix.lon;
	service->scan_altitude_m = (int16_t)fix.alt_m;
	service->scan_status = SATELLITE_SERVICE_SCAN_RUNNING;
	service->next_scan_ms = now_ms;
	unlock();
	return stopped_tracking;
}

#if CONFIG_POSITION
static void satellite_scan_add_result(satellite_service_t *service, uint8_t index, const satellite_pass_t *pass)
{
	uint16_t eta_minutes = satellite_pass_eta_minutes(pass->aos_s, service->scan_start_utc_s);
	uint32_t maximum_elevation = (pass->maximum_elevation_tenths_deg + 5u) / 10u;
	if (maximum_elevation > 90u)
		maximum_elevation = 90u;
	uint32_t packed = satellite_pass_summary_pack(index, eta_minutes, (uint8_t)maximum_elevation,
						      pass->visible_now);
	int64_t aos_offset_s = pass->aos_s - service->scan_start_utc_s;
	if (aos_offset_s > 0)
		packed = satellite_pass_summary_set_aos_remainder(packed, (uint8_t)(aos_offset_s % 60));
	satellite_pass_summary_insert(service->scan_results, &service->scan_result_count,
				      SATELLITE_SERVICE_SCAN_MAX_RECORDS, packed);
}

static void satellite_scan_advance_record(satellite_service_t *service)
{
	if (service->scan_completed < service->scan_total)
		service->scan_completed++;
	service->scan_search_started = false;
}
#endif

__attribute__((noinline)) void satellite_service_scan_step(satellite_service_t *service, uint32_t now_ms)
{
	if (service == NULL || service->scan_status != SATELLITE_SERVICE_SCAN_RUNNING ||
	    (int32_t)(now_ms - service->next_scan_ms) < 0)
		return;
	bool have_record = true;
#if CONFIG_POSITION
	if (!service->scan_search_started && service->scan_completed < service->scan_total) {
		have_record = satellite_store_get(service->scan_completed, &service->tracking.record_scratch);
		if (satellite_store_generation() != service->store_generation) {
			lock();
			service->scan_cache_valid = false;
			service->scan_result_count = 0u;
			service->scan_status = SATELLITE_SERVICE_SCAN_UNAVAILABLE;
			unlock();
			return;
		}
	}
#endif
	lock();
	if (!service->scan_search_started) {
		if (service->scan_completed >= service->scan_total) {
			satellite_scan_finish(service, now_ms);
			unlock();
			return;
		}
#if CONFIG_POSITION
		if (!have_record ||
		    !satellite_tracking_start_record(&service->tracking, service->scan_completed, 0u, 0u,
						     &service->tracking.record_scratch)) {
			satellite_scan_advance_record(service);
			service->next_scan_ms = now_ms + SERVICE_TICK_MS;
			unlock();
			return;
		}
		service->tracking.enabled = false;
		if (!satellite_pass_search_begin(&service->tracking.pass_search, &service->tracking.orbit,
						 service->scan_start_utc_s, SATELLITE_PASS_MAX_HORIZON_HOURS,
						 service->scan_latitude_e7, service->scan_longitude_e7,
						 service->scan_altitude_m)) {
			satellite_scan_advance_record(service);
			service->next_scan_ms = now_ms + SERVICE_TICK_MS;
			unlock();
			return;
		}
		service->scan_search_started = true;
		service->next_scan_ms = now_ms + SERVICE_TICK_MS;
		unlock();
		return;
#else
		(void)have_record;
		service->scan_status = SATELLITE_SERVICE_SCAN_UNAVAILABLE;
		unlock();
		return;
#endif
	}
#if CONFIG_POSITION
	satellite_pass_status_t pass_status = satellite_pass_search_step(&service->tracking.pass_search,
		&service->tracking.orbit, service->scan_latitude_e7, service->scan_longitude_e7,
		service->scan_altitude_m, SATELLITE_PASS_SCAN_SAMPLES_PER_STEP);
	service->next_scan_ms = now_ms + SERVICE_TICK_MS;
	if (pass_status == SATELLITE_PASS_PENDING) {
		unlock();
		return;
	}
	if (pass_status == SATELLITE_PASS_COMPLETE && service->tracking.pass_search.pass_valid)
		satellite_scan_add_result(service, service->scan_completed, &service->tracking.pass_search.pass);
	satellite_scan_advance_record(service);
#endif
	unlock();
}

bool satellite_service_tick(satellite_service_t *service, uint32_t now_ms,
			    const satellite_service_channel_t *channel, satellite_service_update_t *update)
{
	if (service == NULL || update == NULL || !service->tracking_requested)
		return false;
	lock();
	if (!satellite_channel_matches(service, channel) || channel->hotspot || channel->scan != 0u) {
		satellite_stop(service, update);
		unlock();
		return true;
	}
	if (service->tracking.enabled && service->tracking.pass_search_started &&
	    service->tracking.pass_search.status == SATELLITE_PASS_PENDING)
		satellite_tracking_forecast_step(&service->tracking);
	if ((int32_t)(now_ms - service->next_tracking_ms) < 0) {
		unlock();
		return false;
	}
	service->next_tracking_ms = now_ms + SERVICE_TRACKING_PERIOD_MS;
	satellite_tracking_status_t status = service->tracking.enabled ?
		satellite_tracking_tick(&service->tracking, now_ms, 10u) : SATELLITE_TRACKING_UNAVAILABLE;
	satellite_update(service, status, 0u, update);
	unlock();
	return true;
}

void satellite_service_live_view(const satellite_service_t *service, satellite_service_live_view_t *view)
{
	if (service == NULL || view == NULL)
		return;
	memset(view, 0, sizeof(*view));
	if (!service->tracking_requested)
		return;
	const satellite_tracking_t *tracking = &service->tracking;
	const satellite_pass_t *next_pass = satellite_tracking_next_pass(tracking);
	view->pass_valid = service->tracking_requested && tracking->enabled && next_pass != NULL;
	view->pass_los_known = view->pass_valid && next_pass->los_s != 0;
	view->pass_horizon_clipped = view->pass_valid && next_pass->horizon_clipped;
	satellite_service_path_view(service, &view->path);
	if (view->pass_valid) {
		if (next_pass->aos_s > tracking->last_utc_s) {
			int64_t seconds = next_pass->aos_s - tracking->last_utc_s;
			view->aos_in_s = seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
		}
		if (next_pass->los_s > tracking->last_utc_s) {
			int64_t seconds = next_pass->los_s - tracking->last_utc_s;
			view->los_in_s = seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
		}
		view->pass_visible_now = tracking->observation.elevation_tenths_deg >= 0 &&
			tracking->last_utc_s >= next_pass->aos_s &&
			(next_pass->los_s == 0 || tracking->last_utc_s <= next_pass->los_s);
		view->maximum_elevation_tenths = next_pass->maximum_elevation_tenths_deg;
	}
	view->elevation_tenths = tracking->enabled ? tracking->observation.elevation_tenths_deg : 0;
}

void satellite_service_path_view(const satellite_service_t *service, satellite_path_view_t *path)
{
	if (path == NULL)
		return;
	memset(path, 0, sizeof(*path));
	if (service == NULL)
		return;
	const satellite_tracking_t *tracking = &service->tracking;
	if (!service->tracking_requested || !tracking->enabled ||
	    satellite_tracking_next_pass(tracking) == NULL)
		return;
	path->count = tracking->pass_search.path_count;
	memcpy(path->points, tracking->pass_search.path,
	       (size_t)path->count * sizeof(path->points[0]));
	path->azimuth_deg = (uint16_t)((tracking->observation.azimuth_tenths_deg + 5u) / 10u);
	if (path->azimuth_deg >= 360u)
		path->azimuth_deg = 0u;
}

void satellite_service_scan_state(const satellite_service_t *service, uint8_t *status, uint8_t *completed,
				  uint8_t *total, uint8_t *passes, int64_t *start_utc_s, uint32_t *start_ms)
{
	if (service == NULL)
		return;
	if (status != NULL)
		*status = service->scan_status;
	if (completed != NULL)
		*completed = service->scan_completed;
	if (total != NULL)
		*total = service->scan_total;
	if (passes != NULL)
		*passes = service->scan_result_count;
	if (start_utc_s != NULL)
		*start_utc_s = service->scan_start_utc_s;
	if (start_ms != NULL)
		*start_ms = service->scan_start_ms;
}

bool satellite_service_scan_result(const satellite_service_t *service, uint8_t rank, uint32_t *packed,
				   int64_t *aos_s)
{
	if (service == NULL || packed == NULL || aos_s == NULL || rank >= service->scan_result_count)
		return false;
	*packed = service->scan_results[rank];
	uint8_t index = satellite_pass_summary_index(*packed);
	if (index >= service->scan_total)
		return false;
	uint16_t aos_minutes = satellite_pass_summary_aos_minutes(*packed);
	uint8_t remainder_s = satellite_pass_summary_aos_remainder(*packed);
	if (remainder_s != 0u && aos_minutes > 0u)
		aos_minutes--;
	int64_t offset_s = (int64_t)aos_minutes * 60 + remainder_s;
	*aos_s = service->scan_start_utc_s + offset_s;
	return true;
}

bool satellite_service_summary(const satellite_service_t *service, uint8_t record_index,
			       satellite_record_brief_t *summary)
{
	if (service == NULL || summary == NULL || record_index >= service->scan_total ||
	    service->scan_summary_names[record_index][0] == '\0')
		return false;
	memset(summary, 0, sizeof(*summary));
	memcpy(summary->name, service->scan_summary_names[record_index], SATELLITE_RECORD_BRIEF_NAME_MAX);
	while (summary->name_length < SATELLITE_RECORD_BRIEF_NAME_MAX &&
	       summary->name[summary->name_length] != '\0')
		summary->name_length++;
	return true;
}

bool satellite_service_oldest_tle_epoch(const satellite_service_t *service, uint32_t *epoch)
{
	if (service == NULL || epoch == NULL || service->scan_total == 0u ||
	    service->scan_summary_names[0][0] == '\0')
		return false;
	*epoch = service->scan_oldest_tle_epoch;
	return true;
}
