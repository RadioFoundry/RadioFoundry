// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bounded incremental pass prediction.
#include <math.h>
#include <string.h>
#include "satellite/pass.h"

#define MICROSECONDS_PER_SECOND INT64_C(1000000)
#define MICROSECONDS_PER_HOUR (INT64_C(3600) * MICROSECONDS_PER_SECOND)
#define SUMMARY_ETA_SHIFT 4u
#define SUMMARY_ETA_MASK 0x7FFu
#define SUMMARY_ELEVATION_SHIFT 15u
#define SUMMARY_VISIBLE_BIT (1u << 22)

bool satellite_pass_cache_fresh(bool cache_valid, uint32_t now_ms, uint32_t completed_ms)
{
	return cache_valid && (uint32_t)(now_ms - completed_ms) < SATELLITE_PASS_CACHE_MS;
}

uint32_t satellite_pass_summary_pack(uint8_t record_index, uint16_t aos_minutes,
				     uint8_t maximum_elevation_deg, bool visible_now)
{
	if (aos_minutes > SUMMARY_ETA_MASK)
		aos_minutes = SUMMARY_ETA_MASK;
	if (maximum_elevation_deg > 90u)
		maximum_elevation_deg = 90u;
	return ((uint32_t)(record_index & 0x0Fu)) |
		((uint32_t)aos_minutes << SUMMARY_ETA_SHIFT) |
		((uint32_t)maximum_elevation_deg << SUMMARY_ELEVATION_SHIFT) |
		(visible_now ? SUMMARY_VISIBLE_BIT : 0u);
}

uint8_t satellite_pass_summary_index(uint32_t summary)
{
	return (uint8_t)(summary & 0x0Fu);
}

uint16_t satellite_pass_summary_aos_minutes(uint32_t summary)
{
	return (uint16_t)((summary >> SUMMARY_ETA_SHIFT) & SUMMARY_ETA_MASK);
}

uint8_t satellite_pass_summary_maximum_elevation(uint32_t summary)
{
	return (uint8_t)((summary >> SUMMARY_ELEVATION_SHIFT) & 0x7Fu);
}

bool satellite_pass_summary_visible_now(uint32_t summary)
{
	return (summary & SUMMARY_VISIBLE_BIT) != 0u;
}

void satellite_pass_summary_insert(uint32_t *summaries, uint8_t *count, uint8_t capacity, uint32_t summary)
{
	if (summaries == NULL || count == NULL || capacity == 0u)
		return;
	uint8_t at = 0u;
	uint16_t eta = satellite_pass_summary_aos_minutes(summary);
	while (at < *count && satellite_pass_summary_aos_minutes(summaries[at]) <= eta)
		at++;
	if (*count < capacity)
		(*count)++;
	if (at >= capacity)
		return;
	for (uint8_t move = *count; move > at + 1u; move--)
		summaries[move - 1u] = summaries[move - 2u];
	summaries[at] = summary;
}

static int64_t crossing_time(int64_t before_us, double before_el, int64_t after_us, double after_el)
{
	double fraction = before_el / (before_el - after_el);
	if (fraction < 0.0)
		fraction = 0.0;
	else if (fraction > 1.0)
		fraction = 1.0;
	return before_us + (int64_t)((double)(after_us - before_us) * fraction);
}

static double crossing_fraction(double before_el, double after_el)
{
	double fraction = before_el / (before_el - after_el);
	if (fraction < 0.0)
		return 0.0;
	if (fraction > 1.0)
		return 1.0;
	return fraction;
}

static uint16_t observation_azimuth(double azimuth_deg)
{
	uint16_t azimuth = (uint16_t)(azimuth_deg + 0.5);
	return azimuth >= 360u ? 0u : azimuth;
}

static uint16_t crossing_azimuth(uint16_t before, uint16_t after, double fraction)
{
	int delta = (int)after - (int)before;
	if (delta > 180)
		delta -= 360;
	else if (delta < -180)
		delta += 360;
	int azimuth = (int)before + (int)(delta * fraction + (delta >= 0 ? 0.5 : -0.5));
	if (azimuth < 0)
		azimuth += 360;
	else if (azimuth >= 360)
		azimuth -= 360;
	return (uint16_t)azimuth;
}

bool satellite_pass_search_begin(satellite_pass_search_t *search, const satellite_orbit_t *orbit,
				 int64_t start_us, uint32_t horizon_hours, int32_t latitude_e7,
				 int32_t longitude_e7, int32_t altitude_m)
{
	if (search == NULL || orbit == NULL || !orbit->valid || start_us < 0 || horizon_hours == 0u ||
	    horizon_hours > SATELLITE_PASS_MAX_HORIZON_HOURS ||
	    !satellite_orbit_is_fresh(orbit, start_us) || latitude_e7 < -900000000 ||
	    latitude_e7 > 900000000 || longitude_e7 < -1800000000 || longitude_e7 > 1800000000 ||
	    altitude_m < -1000 || altitude_m > 100000)
		return false;
	memset(search, 0, sizeof(*search));
	search->status = SATELLITE_PASS_PENDING;
	search->start_us = start_us;
	search->cursor_us = start_us;
	search->end_us = start_us + (int64_t)horizon_hours * MICROSECONDS_PER_HOUR;
	search->catalog_id = orbit->catalog_id;
	search->epoch_us = orbit->epoch_us;
	search->latitude_e7 = latitude_e7;
	search->longitude_e7 = longitude_e7;
	search->altitude_m = altitude_m;
	return true;
}

static bool same_inputs(const satellite_pass_search_t *search, const satellite_orbit_t *orbit,
			int32_t latitude_e7, int32_t longitude_e7, int32_t altitude_m)
{
	return orbit != NULL && orbit->valid && orbit->catalog_id == search->catalog_id &&
	       orbit->epoch_us == search->epoch_us && latitude_e7 == search->latitude_e7 &&
	       longitude_e7 == search->longitude_e7 && altitude_m == search->altitude_m;
}

static void begin_pass(satellite_pass_search_t *search, int64_t aos_us, int64_t sample_us,
		       const satellite_observation_t *observation, bool visible_now)
{
	search->pass.aos_us = aos_us;
	search->pass.maximum_elevation_us = sample_us;
	search->pass.maximum_elevation_deg = observation->elevation_deg;
	search->pass.visible_now = visible_now;
	search->pass.aos_horizon_clipped = false;
	search->pass_valid = true;
	search->in_pass = true;
}

static void append_path_point(satellite_pass_search_t *search, const satellite_observation_t *observation)
{
	if (search->path_count == SATELLITE_PASS_PATH_POINTS) {
		uint8_t write = 1u;
		for (uint8_t read = 2u; read + 1u < search->path_count; read += 2u)
			search->path[write++] = search->path[read];
		search->path[write++] = search->path[search->path_count - 1u];
		search->path_count = write;
	}
	uint16_t azimuth = observation_azimuth(observation->azimuth_deg);
	uint8_t elevation = observation->elevation_deg <= 0.0 ? 0u :
		observation->elevation_deg >= 90.0 ? 90u : (uint8_t)(observation->elevation_deg + 0.5);
	search->path[search->path_count++] = satellite_path_pack(azimuth, elevation);
}

static void append_horizon_point(satellite_pass_search_t *search, uint16_t azimuth)
{
	satellite_observation_t observation = { .azimuth_deg = azimuth, .elevation_deg = 0.0 };
	append_path_point(search, &observation);
}

static void reverse_path(satellite_pass_search_t *search)
{
	for (uint8_t left = 0u, right = (uint8_t)(search->path_count - 1u);
	     left < right; left++, right--) {
		uint16_t point = search->path[left];
		search->path[left] = search->path[right];
		search->path[right] = point;
	}
}

static bool finish_history_search(satellite_pass_search_t *search, satellite_orbit_t *orbit,
				  int32_t latitude_e7, int32_t longitude_e7, int32_t altitude_m)
{
	reverse_path(search);
	search->backfilling_history = false;
	satellite_observation_t observation;
	if (!satellite_orbit_observe(orbit, search->start_us, latitude_e7, longitude_e7,
				     altitude_m, &observation))
		return false;
	search->previous_elevation_deg = observation.elevation_deg;
	search->previous_azimuth_deg = search->pass_start_azimuth_deg;
	search->cursor_us = search->start_us + (int64_t)SATELLITE_PASS_STEP_SECONDS * MICROSECONDS_PER_SECOND;
	return true;
}

satellite_pass_status_t satellite_pass_search_step(satellite_pass_search_t *search,
						   satellite_orbit_t *orbit, int32_t latitude_e7,
						   int32_t longitude_e7, int32_t altitude_m,
						   uint8_t sample_budget)
{
	if (search == NULL || search->status != SATELLITE_PASS_PENDING)
		return SATELLITE_PASS_INVALID;
	if (!same_inputs(search, orbit, latitude_e7, longitude_e7, altitude_m) || sample_budget == 0u) {
		search->status = SATELLITE_PASS_INVALID;
		return search->status;
	}
	if (sample_budget > SATELLITE_PASS_MAX_SAMPLES_PER_STEP)
		sample_budget = SATELLITE_PASS_MAX_SAMPLES_PER_STEP;
	for (uint8_t sample = 0; sample < sample_budget && search->cursor_us <= search->end_us; sample++) {
		if (!satellite_orbit_is_fresh(orbit, search->cursor_us)) {
			if (!search->backfilling_history) {
				search->status = SATELLITE_PASS_INVALID;
				return search->status;
			}
			search->pass.aos_horizon_clipped = true;
			if (!finish_history_search(search, orbit, latitude_e7, longitude_e7, altitude_m)) {
				search->status = SATELLITE_PASS_INVALID;
				return search->status;
			}
			continue;
		}
		satellite_observation_t observation;
		if (!satellite_orbit_observe(orbit, search->cursor_us, latitude_e7,
					     longitude_e7, altitude_m, &observation) ||
		    !isfinite(observation.elevation_deg)) {
			search->status = SATELLITE_PASS_INVALID;
			return search->status;
		}
		bool visible = observation.elevation_deg >= 0.0;
		if (!search->have_sample) {
			search->have_sample = true;
			search->previous_elevation_deg = observation.elevation_deg;
			search->previous_azimuth_deg = observation_azimuth(observation.azimuth_deg);
			if (visible)
				begin_pass(search, search->start_us, search->cursor_us, &observation, true);
			if (visible) {
				search->pass_start_azimuth_deg = search->previous_azimuth_deg;
				append_path_point(search, &observation);
				if (search->start_us >= (int64_t)SATELLITE_PASS_STEP_SECONDS * MICROSECONDS_PER_SECOND) {
					search->backfilling_history = true;
					search->cursor_us = search->start_us -
						(int64_t)SATELLITE_PASS_STEP_SECONDS * MICROSECONDS_PER_SECOND;
					continue;
				}
				search->pass.aos_horizon_clipped = true;
				if (!finish_history_search(search, orbit, latitude_e7, longitude_e7, altitude_m)) {
					search->status = SATELLITE_PASS_INVALID;
					return search->status;
				}
				continue;
			}
		} else {
			if (search->backfilling_history) {
				if (!visible) {
					double fraction = crossing_fraction(observation.elevation_deg,
								    search->previous_elevation_deg);
					search->pass.aos_us = crossing_time(search->cursor_us,
						observation.elevation_deg, search->cursor_us +
						(int64_t)SATELLITE_PASS_STEP_SECONDS * MICROSECONDS_PER_SECOND,
						search->previous_elevation_deg);
					append_horizon_point(search, crossing_azimuth(observation_azimuth(observation.azimuth_deg),
						search->previous_azimuth_deg, fraction));
				} else {
					if (observation.elevation_deg > search->pass.maximum_elevation_deg) {
						search->pass.maximum_elevation_deg = observation.elevation_deg;
						search->pass.maximum_elevation_us = search->cursor_us;
					}
					append_path_point(search, &observation);
					search->previous_elevation_deg = observation.elevation_deg;
					search->previous_azimuth_deg = observation_azimuth(observation.azimuth_deg);
					int64_t history_limit = search->start_us -
						(int64_t)SATELLITE_PASS_HISTORY_MAX_MINUTES * 60 * MICROSECONDS_PER_SECOND;
					if (search->cursor_us > history_limit) {
						search->cursor_us -= (int64_t)SATELLITE_PASS_STEP_SECONDS * MICROSECONDS_PER_SECOND;
						continue;
					}
					search->pass.aos_horizon_clipped = true;
				}
				if (!finish_history_search(search, orbit, latitude_e7, longitude_e7, altitude_m)) {
					search->status = SATELLITE_PASS_INVALID;
					return search->status;
				}
				continue;
			}
			bool was_visible = search->previous_elevation_deg >= 0.0;
			if (!was_visible && visible) {
				double fraction = crossing_fraction(search->previous_elevation_deg, observation.elevation_deg);
				append_horizon_point(search, crossing_azimuth(search->previous_azimuth_deg,
									observation_azimuth(observation.azimuth_deg), fraction));
				begin_pass(search, crossing_time(search->cursor_us -
								   (int64_t)SATELLITE_PASS_STEP_SECONDS * MICROSECONDS_PER_SECOND,
								   search->previous_elevation_deg, search->cursor_us,
								   observation.elevation_deg), search->cursor_us, &observation, false);
			} else if (search->in_pass && visible) {
				if (observation.elevation_deg > search->pass.maximum_elevation_deg) {
					search->pass.maximum_elevation_deg = observation.elevation_deg;
					search->pass.maximum_elevation_us = search->cursor_us;
				}
			} else if (search->in_pass && was_visible && !visible) {
				int64_t previous_us = search->cursor_us -
					(int64_t)SATELLITE_PASS_STEP_SECONDS * MICROSECONDS_PER_SECOND;
				search->pass.los_us = crossing_time(previous_us, search->previous_elevation_deg,
								     search->cursor_us, observation.elevation_deg);
				search->pass.horizon_clipped = false;
				double fraction = crossing_fraction(search->previous_elevation_deg, observation.elevation_deg);
				append_horizon_point(search, crossing_azimuth(search->previous_azimuth_deg,
									observation_azimuth(observation.azimuth_deg), fraction));
				search->in_pass = false;
				search->status = SATELLITE_PASS_COMPLETE;
			}
			search->previous_elevation_deg = observation.elevation_deg;
			search->previous_azimuth_deg = observation_azimuth(observation.azimuth_deg);
		}
		if (search->in_pass && visible && !search->backfilling_history)
			append_path_point(search, &observation);
		if (search->status == SATELLITE_PASS_COMPLETE)
			break;
		if (search->cursor_us == search->end_us)
			break;
		int64_t next_us = search->cursor_us + (int64_t)SATELLITE_PASS_STEP_SECONDS * MICROSECONDS_PER_SECOND;
		search->cursor_us = next_us > search->end_us ? search->end_us : next_us;
	}
	if (search->status == SATELLITE_PASS_PENDING && search->cursor_us == search->end_us) {
		if (search->in_pass) {
			search->pass.los_us = search->end_us;
			search->pass.horizon_clipped = true;
		}
		search->status = SATELLITE_PASS_COMPLETE;
	}
	return search->status;
}