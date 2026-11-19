// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bounded incremental pass prediction.
#include <limits.h>
#include <string.h>
#include "satellite/pass.h"

#define SECONDS_PER_HOUR INT64_C(3600)
#define SECONDS_PER_MINUTE INT64_C(60)
#define SUMMARY_ETA_SHIFT 4u
#define SUMMARY_ETA_MASK 0x7FFu
#define SUMMARY_ELEVATION_SHIFT 15u
#define SUMMARY_VISIBLE_BIT (1u << 22)
#define SUMMARY_AOS_REMAINDER_SHIFT 23u
#define SUMMARY_AOS_REMAINDER_MASK 0x3Fu

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

uint32_t satellite_pass_summary_set_aos_remainder(uint32_t summary, uint8_t remainder_s)
{
	if (remainder_s >= 60u)
		remainder_s = 0u;
	return (summary & ~(SUMMARY_AOS_REMAINDER_MASK << SUMMARY_AOS_REMAINDER_SHIFT)) |
		((uint32_t)remainder_s << SUMMARY_AOS_REMAINDER_SHIFT);
}

uint8_t satellite_pass_summary_aos_remainder(uint32_t summary)
{
	return (uint8_t)((summary >> SUMMARY_AOS_REMAINDER_SHIFT) & SUMMARY_AOS_REMAINDER_MASK);
}

uint8_t satellite_pass_summary_maximum_elevation(uint32_t summary)
{
	return (uint8_t)((summary >> SUMMARY_ELEVATION_SHIFT) & 0x7Fu);
}

bool satellite_pass_summary_visible_now(uint32_t summary)
{
	return (summary & SUMMARY_VISIBLE_BIT) != 0u;
}

uint16_t satellite_pass_eta_minutes(int64_t aos_s, int64_t now_s)
{
	int64_t delta = aos_s - now_s;
	if (delta <= 0)
		return 0u;
	int64_t minutes = (delta + SECONDS_PER_MINUTE - 1) / SECONDS_PER_MINUTE;
	return minutes > 1440 ? 1440u : (uint16_t)minutes;
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

static int64_t crossing_time(int64_t before_s, int16_t before_elevation,
			     int64_t after_s, int16_t after_elevation)
{
	int32_t denominator = (int32_t)before_elevation - after_elevation;
	if (denominator == 0)
		return before_s;
	int32_t offset = (int32_t)(after_s - before_s) * before_elevation / denominator;
	return before_s + offset;
}

static uint16_t crossing_azimuth(uint16_t before, uint16_t after,
				 int16_t before_elevation, int16_t after_elevation)
{
	int32_t denominator = (int32_t)before_elevation - after_elevation;
	int32_t delta = (int32_t)after - before;
	if (delta > 1800)
		delta -= 3600;
	else if (delta < -1800)
		delta += 3600;
	int32_t offset = denominator == 0 ? 0 : delta * before_elevation / denominator;
	int32_t azimuth = (int32_t)before + offset;
	while (azimuth < 0)
		azimuth += 3600;
	while (azimuth >= 3600)
		azimuth -= 3600;
	return (uint16_t)azimuth;
}

static bool search_inputs_valid(int32_t latitude_e7, int32_t longitude_e7, int32_t altitude_m)
{
	return latitude_e7 >= -900000000 && latitude_e7 <= 900000000 &&
		longitude_e7 >= -1800000000 && longitude_e7 <= 1800000000 &&
		altitude_m >= -1000 && altitude_m <= 100000;
}

bool satellite_pass_search_begin(satellite_pass_search_t *search, const satellite_orbit_t *orbit,
				 int64_t start_s, uint32_t horizon_hours, int32_t latitude_e7,
				 int32_t longitude_e7, int32_t altitude_m)
{
	if (search == NULL || orbit == NULL || !orbit->valid || start_s < 0 || horizon_hours == 0u ||
	    horizon_hours > SATELLITE_PASS_MAX_HORIZON_HOURS ||
	    !satellite_orbit_is_fresh(orbit, start_s) ||
	    !search_inputs_valid(latitude_e7, longitude_e7, altitude_m))
		return false;
	int64_t horizon_s = (int64_t)horizon_hours * SECONDS_PER_HOUR;
	if (start_s > INT64_MAX - horizon_s)
		return false;
	memset(search, 0, sizeof(*search));
	search->status = SATELLITE_PASS_PENDING;
	search->start_s = start_s;
	search->cursor_s = start_s;
	search->end_s = start_s + horizon_s;
	search->catalog_id = orbit->catalog_id;
	search->epoch_s = orbit->epoch_s;
	search->latitude_e7 = latitude_e7;
	search->longitude_e7 = longitude_e7;
	search->altitude_m = altitude_m;
	return true;
}

static bool same_inputs(const satellite_pass_search_t *search, const satellite_orbit_t *orbit,
			int32_t latitude_e7, int32_t longitude_e7, int32_t altitude_m)
{
	return orbit != NULL && orbit->valid && orbit->catalog_id == search->catalog_id &&
	       orbit->epoch_s == search->epoch_s && latitude_e7 == search->latitude_e7 &&
	       longitude_e7 == search->longitude_e7 && altitude_m == search->altitude_m;
}

static void begin_pass(satellite_pass_search_t *search, int64_t aos_s, int64_t sample_s,
		       const satellite_observation_t *observation, bool visible_now)
{
	search->pass.aos_s = aos_s;
	search->pass.maximum_elevation_s = sample_s;
	search->pass.maximum_elevation_tenths_deg =
		observation->elevation_tenths_deg > 0 ? (uint16_t)observation->elevation_tenths_deg : 0u;
	search->pass.visible_now = visible_now;
	search->pass.aos_horizon_clipped = false;
	search->pass_valid = true;
	search->in_pass = true;
}

static void append_path_point(satellite_pass_search_t *search,
			      const satellite_observation_t *observation)
{
	if (search->path_count == SATELLITE_PASS_PATH_POINTS) {
		uint8_t write = 1u;
		for (uint8_t read = 2u; read + 1u < search->path_count; read += 2u)
			search->path[write++] = search->path[read];
		search->path[write++] = search->path[search->path_count - 1u];
		search->path_count = write;
	}
	uint16_t azimuth = (uint16_t)((observation->azimuth_tenths_deg + 5u) / 10u);
	if (azimuth >= 360u)
		azimuth = 0u;
	uint8_t elevation = observation->elevation_tenths_deg <= 0 ? 0u :
		observation->elevation_tenths_deg >= 900 ? 90u :
		(uint8_t)((observation->elevation_tenths_deg + 5) / 10);
	search->path[search->path_count++] = satellite_path_pack(azimuth, elevation);
}

static void append_horizon_point(satellite_pass_search_t *search, uint16_t azimuth_tenths_deg)
{
	satellite_observation_t observation = {
		.azimuth_tenths_deg = azimuth_tenths_deg,
		.elevation_tenths_deg = 0,
	};
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
	if (!satellite_orbit_observe(orbit, search->start_s, latitude_e7, longitude_e7,
				     altitude_m, &observation))
		return false;
	search->previous_elevation_tenths_deg = observation.elevation_tenths_deg;
	search->previous_azimuth_tenths_deg = search->pass_start_azimuth_tenths_deg;
	search->cursor_s = search->start_s + SATELLITE_PASS_STEP_SECONDS;
	return true;
}

satellite_pass_status_t satellite_pass_search_step(satellite_pass_search_t *search,
						   satellite_orbit_t *orbit, int32_t latitude_e7,
						   int32_t longitude_e7, int32_t altitude_m,
						   uint8_t sample_budget)
{
	if (search == NULL || search->status != SATELLITE_PASS_PENDING)
		return SATELLITE_PASS_INVALID;
	if (!same_inputs(search, orbit, latitude_e7, longitude_e7, altitude_m) ||
	    !search_inputs_valid(latitude_e7, longitude_e7, altitude_m) || sample_budget == 0u) {
		search->status = SATELLITE_PASS_INVALID;
		return search->status;
	}
	if (sample_budget > SATELLITE_PASS_MAX_SAMPLES_PER_STEP)
		sample_budget = SATELLITE_PASS_MAX_SAMPLES_PER_STEP;
	for (uint8_t sample = 0u; sample < sample_budget && search->cursor_s <= search->end_s; sample++) {
		if (!satellite_orbit_is_fresh(orbit, search->cursor_s)) {
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
		if (!satellite_orbit_observe(orbit, search->cursor_s, latitude_e7,
					     longitude_e7, altitude_m, &observation)) {
			search->status = SATELLITE_PASS_INVALID;
			return search->status;
		}
		bool visible = observation.elevation_tenths_deg >= 0;
		if (!search->have_sample) {
			search->have_sample = true;
			search->previous_elevation_tenths_deg = observation.elevation_tenths_deg;
			search->previous_azimuth_tenths_deg = observation.azimuth_tenths_deg;
			if (visible) {
				begin_pass(search, search->start_s, search->cursor_s, &observation, true);
				search->pass_start_azimuth_tenths_deg = observation.azimuth_tenths_deg;
				append_path_point(search, &observation);
				if (search->start_s >= SATELLITE_PASS_STEP_SECONDS) {
					search->backfilling_history = true;
					search->cursor_s = search->start_s - SATELLITE_PASS_STEP_SECONDS;
					continue;
				}
				search->pass.aos_horizon_clipped = true;
				if (!finish_history_search(search, orbit, latitude_e7, longitude_e7, altitude_m)) {
					search->status = SATELLITE_PASS_INVALID;
					return search->status;
				}
				continue;
			}
		} else if (search->backfilling_history) {
			if (!visible) {
				search->pass.aos_s = crossing_time(search->cursor_s,
					observation.elevation_tenths_deg, search->cursor_s +
					SATELLITE_PASS_STEP_SECONDS, search->previous_elevation_tenths_deg);
				append_horizon_point(search, crossing_azimuth(observation.azimuth_tenths_deg,
					search->previous_azimuth_tenths_deg, observation.elevation_tenths_deg,
					search->previous_elevation_tenths_deg));
			} else {
				if (observation.elevation_tenths_deg >
				    (int16_t)search->pass.maximum_elevation_tenths_deg) {
					search->pass.maximum_elevation_tenths_deg =
						(uint16_t)observation.elevation_tenths_deg;
					search->pass.maximum_elevation_s = search->cursor_s;
				}
				append_path_point(search, &observation);
				search->previous_elevation_tenths_deg = observation.elevation_tenths_deg;
				search->previous_azimuth_tenths_deg = observation.azimuth_tenths_deg;
				int64_t history_limit = search->start_s -
					(int64_t)SATELLITE_PASS_HISTORY_MAX_MINUTES * SECONDS_PER_MINUTE;
				if (search->cursor_s > history_limit) {
					search->cursor_s -= SATELLITE_PASS_STEP_SECONDS;
					continue;
				}
				search->pass.aos_horizon_clipped = true;
			}
			if (!finish_history_search(search, orbit, latitude_e7, longitude_e7, altitude_m)) {
				search->status = SATELLITE_PASS_INVALID;
				return search->status;
			}
			continue;
		} else {
			bool was_visible = search->previous_elevation_tenths_deg >= 0;
			if (!was_visible && visible) {
				append_horizon_point(search, crossing_azimuth(search->previous_azimuth_tenths_deg,
					observation.azimuth_tenths_deg,
					search->previous_elevation_tenths_deg,
					observation.elevation_tenths_deg));
				begin_pass(search, crossing_time(search->cursor_s - SATELLITE_PASS_STEP_SECONDS,
					search->previous_elevation_tenths_deg, search->cursor_s,
					observation.elevation_tenths_deg), search->cursor_s, &observation, false);
				search->pass_start_azimuth_tenths_deg = observation.azimuth_tenths_deg;
			} else if (search->in_pass && visible) {
				if (observation.elevation_tenths_deg >
				    (int16_t)search->pass.maximum_elevation_tenths_deg) {
					search->pass.maximum_elevation_tenths_deg =
						(uint16_t)observation.elevation_tenths_deg;
					search->pass.maximum_elevation_s = search->cursor_s;
				}
			} else if (search->in_pass && was_visible && !visible) {
				int64_t previous_s = search->cursor_s - SATELLITE_PASS_STEP_SECONDS;
				search->pass.los_s = crossing_time(previous_s,
					search->previous_elevation_tenths_deg, search->cursor_s,
					observation.elevation_tenths_deg);
				search->pass.horizon_clipped = false;
				append_horizon_point(search, crossing_azimuth(search->previous_azimuth_tenths_deg,
					observation.azimuth_tenths_deg,
					search->previous_elevation_tenths_deg,
					observation.elevation_tenths_deg));
				search->in_pass = false;
				search->status = SATELLITE_PASS_COMPLETE;
			}
			search->previous_elevation_tenths_deg = observation.elevation_tenths_deg;
			search->previous_azimuth_tenths_deg = observation.azimuth_tenths_deg;
		}
		if (search->in_pass && visible && !search->backfilling_history)
			append_path_point(search, &observation);
		if (search->status == SATELLITE_PASS_COMPLETE)
			break;
		if (search->cursor_s == search->end_s)
			break;
		int64_t next_s = search->cursor_s + SATELLITE_PASS_STEP_SECONDS;
		search->cursor_s = next_s > search->end_s ? search->end_s : next_s;
	}
	if (search->status == SATELLITE_PASS_PENDING && search->cursor_s == search->end_s) {
		if (search->in_pass) {
			search->pass.los_s = search->end_s;
			search->pass.horizon_clipped = true;
		}
		search->status = SATELLITE_PASS_COMPLETE;
	}
	return search->status;
}
