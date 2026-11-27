// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Satellite input and Doppler tracking service; radio tuning remains externally owned.
#include <string.h>
#include "satellite/store.h"
#include "satellite/tracking.h"

#define TRACKING_UPDATE_MS 1000u
#define SATELLITE_OBSERVER_POSITION_TOLERANCE_E7 32000
#define SATELLITE_OBSERVER_ALTITUDE_TOLERANCE_M 100

_Static_assert(sizeof(satellite_tracking_workspace_t) >= sizeof(satellite_record_t),
	       "satellite tracking workspace must hold a catalog record");

static bool leap_year(uint32_t year)
{
	return year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
}

static bool observer_moved(const satellite_pass_search_t *search, const position_fix_t *fix)
{
	int64_t latitude_delta = (int64_t)search->latitude_e7 - fix->lat;
	int64_t longitude_delta = (int64_t)search->longitude_e7 - fix->lon;
	int32_t altitude_delta = (int32_t)search->altitude_m - fix->alt_m;
	return latitude_delta > SATELLITE_OBSERVER_POSITION_TOLERANCE_E7 ||
	       latitude_delta < -SATELLITE_OBSERVER_POSITION_TOLERANCE_E7 ||
	       longitude_delta > SATELLITE_OBSERVER_POSITION_TOLERANCE_E7 ||
	       longitude_delta < -SATELLITE_OBSERVER_POSITION_TOLERANCE_E7 ||
	       altitude_delta > SATELLITE_OBSERVER_ALTITUDE_TOLERANCE_M ||
	       altitude_delta < -SATELLITE_OBSERVER_ALTITUDE_TOLERANCE_M;
}

bool satellite_utc_from_fields(uint32_t date_ddmmyy, uint32_t utc_hms, int64_t *utc_s)
{
	if (utc_s == NULL || date_ddmmyy > 999999u || utc_hms > 999999u)
		return false;
	uint32_t day = date_ddmmyy / 10000u;
	uint32_t month = date_ddmmyy / 100u % 100u;
	uint32_t short_year = date_ddmmyy % 100u;
	uint32_t year = short_year >= 80u ? 1900u + short_year : 2000u + short_year;
	uint32_t hour = utc_hms / 10000u;
	uint32_t minute = utc_hms / 100u % 100u;
	uint32_t second = utc_hms % 100u;
	static const uint8_t month_days[] = { 31u, 28u, 31u, 30u, 31u, 30u,
					      31u, 31u, 30u, 31u, 30u, 31u };
	if (month == 0u || month > 12u || day == 0u || hour > 23u || minute > 59u || second > 59u)
		return false;
	uint32_t month_day_limit = month_days[month - 1u] + (month == 2u && leap_year(year) ? 1u : 0u);
	if (day > month_day_limit)
		return false;
	int64_t days = 0;
	for (uint32_t y = 1970u; y < year; y++)
		days += leap_year(y) ? 366 : 365;
	for (uint32_t m = 1u; m < month; m++)
		days += month_days[m - 1u] + (m == 2u && leap_year(year) ? 1u : 0u);
	days += day - 1u;
	*utc_s = days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
	return true;
}

bool satellite_tle_age_days(uint32_t epoch, int64_t utc_s, uint16_t *age_days)
{
	if (age_days == NULL)
		return false;
	if (utc_s < epoch) {
		*age_days = 0u;
		return true;
	}
	uint64_t days = (uint64_t)(utc_s - epoch) / 86400u;
	*age_days = days > UINT16_MAX ? UINT16_MAX : (uint16_t)days;
	return true;
}

bool satellite_tracking_start_record(satellite_tracking_t *tracking, uint8_t record_index,
				     uint32_t nominal_rx_hz, uint32_t nominal_tx_hz,
				     const satellite_record_t *record)
{
	if (tracking == NULL || record == NULL || !satellite_orbit_init(&tracking->orbit, record))
		return false;
	tracking->enabled = true;
	tracking->override_active = false;
	tracking->initialized = false;
	tracking->record_index = record_index;
	tracking->nominal_rx_hz = nominal_rx_hz;
	tracking->nominal_tx_hz = nominal_tx_hz;
	tracking->status = SATELLITE_TRACKING_UNAVAILABLE;
	memset(&tracking->observation, 0, sizeof(tracking->observation));
	memset(&tracking->targets, 0, sizeof(tracking->targets));
	memset(&tracking->pass_search, 0, sizeof(tracking->pass_search));
	tracking->pass_search_started = false;
	tracking->pass_forecast_valid = false;
	return true;
}

bool satellite_tracking_start(satellite_tracking_t *tracking, uint8_t record_index,
			      uint32_t nominal_rx_hz, uint32_t nominal_tx_hz)
{
	if (tracking == NULL || !satellite_store_get(record_index, &tracking->record_scratch))
		return false;
	return satellite_tracking_start_record(tracking, record_index, nominal_rx_hz, nominal_tx_hz,
					       &tracking->record_scratch);
}

bool satellite_tracking_start_scan(satellite_tracking_t *tracking, uint8_t record_index)
{
	if (tracking == NULL || !satellite_store_get(record_index, &tracking->record_scratch))
		return false;
	return satellite_tracking_start_record(tracking, record_index, 0u, 0u, &tracking->record_scratch);
}

void satellite_tracking_stop(satellite_tracking_t *tracking)
{
	if (tracking == NULL)
		return;
	tracking->enabled = false;
	tracking->override_active = false;
	tracking->initialized = false;
	tracking->status = SATELLITE_TRACKING_OFF;
	tracking->targets.rx_hz = tracking->nominal_rx_hz;
	tracking->targets.tx_hz = tracking->nominal_tx_hz;
	tracking->targets.rx_offset_hz = 0;
	tracking->targets.tx_offset_hz = 0;
	tracking->pass_search_started = false;
	tracking->pass_forecast_valid = false;
}

satellite_tracking_status_t satellite_tracking_update(satellite_tracking_t *tracking,
						       const position_fix_t *fix, uint8_t position_source,
						       int64_t utc_s, uint32_t now_ms,
						       uint32_t tuning_step_hz)
{
	if (tracking == NULL || !tracking->enabled)
		return SATELLITE_TRACKING_OFF;
	if (fix == NULL || position_source == POSITION_SRC_NONE || fix->alt_m == POSITION_ALT_NONE ||
	    fix->lat < -900000000 || fix->lat > 900000000 || fix->lon < -1800000000 || fix->lon > 1800000000 ||
	    !satellite_orbit_is_fresh(&tracking->orbit, utc_s) || tuning_step_hz == 0u) {
		tracking->override_active = false;
		tracking->status = SATELLITE_TRACKING_UNAVAILABLE;
		tracking->pass_search_started = false;
		tracking->pass_forecast_valid = false;
		return tracking->status;
	}
	if (tracking->initialized && (uint32_t)(now_ms - tracking->last_update_ms) < TRACKING_UPDATE_MS)
		return tracking->status;
	tracking->initialized = true;
	tracking->last_update_ms = now_ms;
	tracking->last_utc_s = utc_s;
	tracking->override_active = false;
	if (!satellite_orbit_observe(&tracking->orbit, utc_s, fix->lat, fix->lon, fix->alt_m,
				     &tracking->observation) ||
	    !satellite_doppler_calculate(tracking->nominal_rx_hz, tracking->nominal_tx_hz,
					 tracking->observation.range_rate_cm_s, tuning_step_hz,
					 &tracking->targets)) {
		tracking->status = SATELLITE_TRACKING_UNAVAILABLE;
		return tracking->status;
	}
	if (tracking->pass_search_started && tracking->observation.elevation_tenths_deg < 0 &&
	    observer_moved(&tracking->pass_search, fix)) {
		tracking->pass_search_started = false;
		tracking->pass_forecast_valid = false;
	}
	const satellite_pass_t *next_pass = satellite_tracking_next_pass(tracking);
	if (tracking->pass_search_started && tracking->pass_search.status == SATELLITE_PASS_COMPLETE &&
	    (utc_s - tracking->pass_search.start_s >= INT64_C(12) * 3600 ||
	     (next_pass != NULL && next_pass->los_s <= utc_s))) {
		tracking->pass_search_started = false;
		tracking->pass_forecast_valid = false;
	}
	if (!tracking->pass_search_started) {
		tracking->pass_search_started = satellite_pass_search_begin(&tracking->pass_search, &tracking->orbit,
										      utc_s,
										      SATELLITE_PASS_MAX_HORIZON_HOURS,
										      fix->lat, fix->lon,
										      fix->alt_m);
		tracking->pass_forecast_valid = false;
	}
	tracking->override_active = true;
	tracking->status = tracking->observation.elevation_tenths_deg >= 0 ?
			   SATELLITE_TRACKING_VISIBLE : SATELLITE_TRACKING_BELOW_HORIZON;
	return tracking->status;
}

satellite_pass_status_t satellite_tracking_forecast_step(satellite_tracking_t *tracking)
{
	if (tracking == NULL || !tracking->enabled || !tracking->pass_search_started)
		return SATELLITE_PASS_INVALID;
	satellite_pass_search_t *search = &tracking->pass_search;
	if (search->status != SATELLITE_PASS_PENDING)
		return search->status;
	satellite_pass_status_t status = satellite_pass_search_step(search, &tracking->orbit,
		search->latitude_e7, search->longitude_e7, search->altitude_m,
		SATELLITE_PASS_SCAN_SAMPLES_PER_STEP);
	if (status == SATELLITE_PASS_INVALID) {
		tracking->pass_search_started = false;
		tracking->pass_forecast_valid = false;
	} else {
		tracking->pass_forecast_valid = search->pass_valid;
	}
	return status;
}

const satellite_pass_t *satellite_tracking_next_pass(const satellite_tracking_t *tracking)
{
	if (tracking == NULL || !tracking->pass_forecast_valid ||
	    !tracking->pass_search.pass_valid)
		return NULL;
	return &tracking->pass_search.pass;
}

satellite_tracking_status_t satellite_tracking_tick(satellite_tracking_t *tracking, uint32_t now_ms,
						     uint32_t tuning_step_hz)
{
#if CONFIG_POSITION
	position_fix_t fix;
	uint8_t source = position_fix_for_satellite(&fix, now_ms);
	uint32_t date_ddmmyy, utc_hms;
	int64_t utc_s;
	if (source == POSITION_SRC_NONE || !position_utc_fields(&date_ddmmyy, &utc_hms, now_ms) ||
	    !satellite_utc_from_fields(date_ddmmyy, utc_hms, &utc_s))
		return satellite_tracking_update(tracking, NULL, POSITION_SRC_NONE, -1, now_ms, tuning_step_hz);
	return satellite_tracking_update(tracking, &fix, source, utc_s, now_ms, tuning_step_hz);
#else
	(void)now_ms;
	(void)tuning_step_hz;
	return satellite_tracking_update(tracking, NULL, POSITION_SRC_NONE, -1, now_ms, tuning_step_hz);
#endif
}