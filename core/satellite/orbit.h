// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Lightweight Keplerian TLE propagation and observer-relative satellite state.
#ifndef SATELLITE_ORBIT_H
#define SATELLITE_ORBIT_H

#include <stdbool.h>
#include <stdint.h>
#include "satellite/store.h"

typedef struct {
	uint32_t semi_major_axis_m;
	uint32_t mean_motion_rev_day_e8;
	int32_t mean_motion_dot_half_rev_day2_e8;
	uint32_t eccentricity_q30;
	uint32_t inclination_phase;
	uint32_t raan_phase;
	uint32_t argument_perigee_phase;
	uint32_t mean_anomaly_phase;
	int32_t raan_rate_phase_s_q16;
	int32_t argument_perigee_rate_phase_s_q16;
	int64_t epoch_s;
	uint16_t epoch_fraction_q16;
	uint32_t catalog_id;
	bool valid;
} satellite_orbit_t;

typedef struct {
	int32_t position_m[3];
	int32_t velocity_cm_s[3];
} satellite_orbit_state_t;

typedef struct {
	uint16_t azimuth_tenths_deg;
	int16_t elevation_tenths_deg;
	uint32_t range_m;
	int32_t range_rate_cm_s;
} satellite_observation_t;

bool satellite_orbit_init(satellite_orbit_t *orbit, const satellite_record_t *record);
bool satellite_tle_epoch(const char line1[SATELLITE_TLE_LINE_LENGTH + 1u], int64_t *epoch_s,
			 uint16_t *fraction_q16);
bool satellite_orbit_is_fresh(const satellite_orbit_t *orbit, int64_t utc_s);
bool satellite_orbit_at(satellite_orbit_t *orbit, int64_t utc_s, satellite_orbit_state_t *state);
bool satellite_orbit_observe(satellite_orbit_t *orbit, int64_t utc_s, int32_t latitude_e7, int32_t longitude_e7,
			    int32_t altitude_m, satellite_observation_t *observation);

#endif