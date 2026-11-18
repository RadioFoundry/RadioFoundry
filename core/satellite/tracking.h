// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Satellite input and Doppler tracking service; radio tuning remains externally owned.
#ifndef SATELLITE_TRACKING_H
#define SATELLITE_TRACKING_H

#include <stdbool.h>
#include <stdint.h>
#include "position/position.h"
#include "satellite/doppler.h"
#include "satellite/orbit.h"
#include "satellite/pass.h"

typedef enum {
	SATELLITE_TRACKING_OFF,
	SATELLITE_TRACKING_UNAVAILABLE,
	SATELLITE_TRACKING_BELOW_HORIZON,
	SATELLITE_TRACKING_VISIBLE,
} satellite_tracking_status_t;

typedef struct {
	bool enabled;
	bool override_active;
	bool initialized;
	uint8_t record_index;
	uint32_t nominal_rx_hz;
	uint32_t nominal_tx_hz;
	uint32_t last_update_ms;
	int64_t last_utc_s;
	satellite_tracking_status_t status;
	satellite_observation_t observation;
	satellite_doppler_targets_t targets;
	satellite_pass_search_t pass_search;
	bool pass_search_started;
	bool pass_forecast_valid;
} satellite_tracking_workspace_t;

typedef struct {
	satellite_orbit_t orbit;
	union {
		struct {
			bool enabled;
			bool override_active;
			bool initialized;
			uint8_t record_index;
			uint32_t nominal_rx_hz;
			uint32_t nominal_tx_hz;
			uint32_t last_update_ms;
			int64_t last_utc_s;
			satellite_tracking_status_t status;
			satellite_observation_t observation;
			satellite_doppler_targets_t targets;
			satellite_pass_search_t pass_search;
			bool pass_search_started;
			bool pass_forecast_valid;
		};
		satellite_record_t record_scratch;
	};
} satellite_tracking_t;

bool satellite_utc_from_fields(uint32_t date_ddmmyy, uint32_t utc_hms, int64_t *utc_s);
bool satellite_tle_age_days(uint32_t epoch, int64_t utc_s, uint16_t *age_days);
bool satellite_tracking_start(satellite_tracking_t *tracking, uint8_t record_index,
			      uint32_t nominal_rx_hz, uint32_t nominal_tx_hz);
bool satellite_tracking_start_scan(satellite_tracking_t *tracking, uint8_t record_index);
bool satellite_tracking_start_record(satellite_tracking_t *tracking, uint8_t record_index,
				     uint32_t nominal_rx_hz, uint32_t nominal_tx_hz,
				     const satellite_record_t *record);
void satellite_tracking_stop(satellite_tracking_t *tracking);
satellite_tracking_status_t satellite_tracking_update(satellite_tracking_t *tracking,
						       const position_fix_t *fix, uint8_t position_source,
						       int64_t utc_s, uint32_t now_ms,
						       uint32_t tuning_step_hz);
satellite_pass_status_t satellite_tracking_forecast_step(satellite_tracking_t *tracking);
satellite_tracking_status_t satellite_tracking_tick(satellite_tracking_t *tracking, uint32_t now_ms,
						     uint32_t tuning_step_hz);
const satellite_pass_t *satellite_tracking_next_pass(const satellite_tracking_t *tracking);

#endif