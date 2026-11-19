// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bounded incremental pass prediction for a selected satellite and observer.
#ifndef SATELLITE_PASS_H
#define SATELLITE_PASS_H

#include <stdbool.h>
#include <stdint.h>
#include "satellite/orbit.h"
#include "satellite/path.h"

#define SATELLITE_PASS_MAX_HORIZON_HOURS 24u
#define SATELLITE_PASS_STEP_SECONDS 60u
#define SATELLITE_PASS_HISTORY_MAX_MINUTES 30u
#define SATELLITE_PASS_MAX_SAMPLES_PER_STEP 32u
#define SATELLITE_PASS_SCAN_SAMPLES_PER_STEP 1u
#define SATELLITE_PASS_CACHE_MS 300000u

typedef enum {
	SATELLITE_PASS_PENDING,
	SATELLITE_PASS_COMPLETE,
	SATELLITE_PASS_INVALID,
} satellite_pass_status_t;

typedef struct {
	int64_t aos_s;
	int64_t los_s;
	int64_t maximum_elevation_s;
	uint16_t maximum_elevation_tenths_deg;
	bool visible_now;
	bool aos_horizon_clipped;
	bool horizon_clipped;
} satellite_pass_t;

typedef struct {
	satellite_pass_t pass;
	uint16_t path[SATELLITE_PASS_PATH_POINTS];
	uint8_t path_count;
	satellite_pass_status_t status;
	int64_t start_s;
	int64_t cursor_s;
	int64_t end_s;
	uint32_t catalog_id;
	int64_t epoch_s;
	int32_t latitude_e7;
	int32_t longitude_e7;
	int32_t altitude_m;
	int16_t previous_elevation_tenths_deg;
	uint16_t previous_azimuth_tenths_deg;
	uint16_t pass_start_azimuth_tenths_deg;
	bool pass_valid;
	bool have_sample;
	bool in_pass;
	bool backfilling_history;
} satellite_pass_search_t;

uint32_t satellite_pass_summary_pack(uint8_t record_index, uint16_t aos_minutes,
				    uint8_t maximum_elevation_deg, bool visible_now);
uint8_t satellite_pass_summary_index(uint32_t summary);
uint16_t satellite_pass_summary_aos_minutes(uint32_t summary);
uint32_t satellite_pass_summary_set_aos_remainder(uint32_t summary, uint8_t remainder_s);
uint8_t satellite_pass_summary_aos_remainder(uint32_t summary);
uint8_t satellite_pass_summary_maximum_elevation(uint32_t summary);
bool satellite_pass_summary_visible_now(uint32_t summary);
void satellite_pass_summary_insert(uint32_t *summaries, uint8_t *count, uint8_t capacity, uint32_t summary);
uint16_t satellite_pass_eta_minutes(int64_t aos_s, int64_t now_s);
bool satellite_pass_cache_fresh(bool cache_valid, uint32_t now_ms, uint32_t completed_ms);

bool satellite_pass_search_begin(satellite_pass_search_t *search, const satellite_orbit_t *orbit,
				 int64_t start_s, uint32_t horizon_hours, int32_t latitude_e7,
				 int32_t longitude_e7, int32_t altitude_m);
satellite_pass_status_t satellite_pass_search_step(satellite_pass_search_t *search,
						   satellite_orbit_t *orbit, int32_t latitude_e7,
						   int32_t longitude_e7, int32_t altitude_m,
						   uint8_t sample_budget);

#endif