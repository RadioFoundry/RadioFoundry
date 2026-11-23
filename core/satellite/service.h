// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host-testable coordination for satellite pass scans and tracking requests.
#ifndef SATELLITE_SERVICE_H
#define SATELLITE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "satellite/store.h"
#include "satellite/tracking.h"

#define SATELLITE_SERVICE_SCAN_MAX_RECORDS SATELLITE_STORE_MAX_RECORDS

typedef enum {
	SATELLITE_SERVICE_SCAN_IDLE,
	SATELLITE_SERVICE_SCAN_RUNNING,
	SATELLITE_SERVICE_SCAN_COMPLETE,
	SATELLITE_SERVICE_SCAN_UNAVAILABLE,
} satellite_service_scan_status_t;

typedef struct {
	bool vfo;
	uint8_t vfo_index;
	uint16_t zone_index;
	uint16_t zone_pos;
	uint16_t channel_number;
	bool hotspot;
	uint8_t scan;
} satellite_service_channel_t;

typedef struct {
	bool tracking;
	uint8_t record_index;
	uint8_t status;
	uint32_t nominal_rx_hz;
	uint32_t nominal_tx_hz;
	uint16_t tx_tone_tenths;
	bool override_active;
	satellite_doppler_targets_t targets;
} satellite_service_update_t;

typedef struct {
	bool pass_valid;
	bool pass_visible_now;
	bool pass_los_known;
	bool pass_horizon_clipped;
	uint32_t aos_in_s;
	uint32_t los_in_s;
	uint16_t maximum_elevation_tenths;
	int16_t elevation_tenths;
	satellite_path_view_t path;
} satellite_service_live_view_t;

typedef struct {
	satellite_tracking_t tracking;
	bool tracking_requested;
	uint8_t record_index;
	bool channel_vfo;
	uint8_t channel_vfo_index;
	uint16_t channel_zone;
	uint16_t channel_pos;
	uint16_t channel_number;
	uint32_t next_tracking_ms;

	uint32_t scan_results[SATELLITE_SERVICE_SCAN_MAX_RECORDS];
	char scan_summary_names[SATELLITE_SERVICE_SCAN_MAX_RECORDS][SATELLITE_RECORD_BRIEF_NAME_MAX];
	uint32_t scan_oldest_tle_epoch;
	int64_t scan_start_utc_s;
	uint32_t scan_start_ms;
	int32_t scan_latitude_e7;
	int32_t scan_longitude_e7;
	int16_t scan_altitude_m;
	uint8_t scan_status;
	uint8_t scan_completed;
	uint8_t scan_total;
	uint8_t scan_result_count;
	bool scan_search_started;
	bool scan_cache_valid;
	uint32_t scan_completed_ms;
	uint32_t next_scan_ms;
	uint32_t store_generation;
} satellite_service_t;

void satellite_service_set_lock(void (*set_locked)(bool locked));
void satellite_service_init(satellite_service_t *service);
void satellite_service_tracking_request(satellite_service_t *service, bool enabled, uint8_t record_index,
					uint32_t now_ms, const satellite_service_channel_t *channel,
					satellite_service_update_t *update);
bool satellite_service_scan_request(satellite_service_t *service, bool enabled, uint32_t now_ms,
				    satellite_service_update_t *update);
void satellite_service_scan_step(satellite_service_t *service, uint32_t now_ms);
bool satellite_service_tick(satellite_service_t *service, uint32_t now_ms,
			    const satellite_service_channel_t *channel, satellite_service_update_t *update);
void satellite_service_live_view(const satellite_service_t *service, satellite_service_live_view_t *view);
void satellite_service_path_view(const satellite_service_t *service, satellite_path_view_t *path);
void satellite_service_scan_state(const satellite_service_t *service, uint8_t *status, uint8_t *completed,
				  uint8_t *total, uint8_t *passes, int64_t *start_utc_s, uint32_t *start_ms);
bool satellite_service_scan_result(const satellite_service_t *service, uint8_t rank, uint32_t *packed,
				   int64_t *aos_s);
bool satellite_service_summary(const satellite_service_t *service, uint8_t record_index,
			       satellite_record_brief_t *summary);
bool satellite_service_oldest_tle_epoch(const satellite_service_t *service, uint32_t *epoch);

#endif
