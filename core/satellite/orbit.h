// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Lightweight Keplerian TLE propagation and observer-relative satellite state.
#ifndef SATELLITE_ORBIT_H
#define SATELLITE_ORBIT_H

#include <stdbool.h>
#include <stdint.h>
#include "satellite/store.h"

typedef struct {
	double semi_major_axis_km;
	double mean_motion_rad_s;
	double mean_motion_dot_rad_s2;
	double eccentricity;
	double inclination_rad;
	double raan_rad;
	double argument_perigee_rad;
	double mean_anomaly_rad;
	double raan_rate_rad_s;
	double argument_perigee_rate_rad_s;
	int64_t epoch_us;
	uint32_t catalog_id;
	bool valid;
} satellite_orbit_t;

typedef struct {
	double position_km[3];
	double velocity_km_s[3];
} satellite_orbit_state_t;

typedef struct {
	double azimuth_deg;
	double elevation_deg;
	double range_km;
	double range_rate_m_s;
} satellite_observation_t;

bool satellite_orbit_init(satellite_orbit_t *orbit, const satellite_record_t *record);
bool satellite_orbit_is_fresh(const satellite_orbit_t *orbit, int64_t utc_us);
bool satellite_orbit_at(satellite_orbit_t *orbit, int64_t utc_us, satellite_orbit_state_t *state);
bool satellite_orbit_observe(satellite_orbit_t *orbit, int64_t utc_us, int32_t latitude_e7, int32_t longitude_e7,
			    int32_t altitude_m, satellite_observation_t *observation);

#endif