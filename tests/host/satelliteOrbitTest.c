// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "satellite/doppler.h"
#include "satellite/orbit.h"
#include "satellite/pass.h"
#include "satellite/tracking.h"

static int failures, checks;
static satellite_record_t stored_record;
static bool stored_record_available;

bool satellite_store_get(uint8_t index, satellite_record_t *record)
{
	if (index != 0u || !stored_record_available)
		return false;
	*record = stored_record;
	return true;
}

#define CHECK(condition)                                                                                           \
	do {                                                                                                         \
		checks++;                                                                                            \
		if (!(condition)) {                                                                                  \
			failures++;                                                                                   \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
		}                                                                                                    \
	} while (0)

static void set_checksum(char line[70])
{
	unsigned sum = 0;
	for (uint8_t i = 0; i < 68u; i++) {
		if (line[i] >= '0' && line[i] <= '9')
			sum += (unsigned)(line[i] - '0');
		else if (line[i] == '-')
			sum++;
	}
	line[68] = (char)('0' + sum % 10u);
}

static void set_line(char out[70], const char *text)
{
	memset(out, ' ', 69u);
	size_t length = strlen(text);
	if (length > 68u)
		length = 68u;
	memcpy(out, text, length);
	set_checksum(out);
	out[69] = 0;
}

static satellite_record_t record_for(const char *line1, const char *line2, uint32_t catalog_id, uint32_t epoch)
{
	satellite_record_t record = { .catalog_id = catalog_id, .epoch = epoch };
	set_line(record.line1, line1);
	set_line(record.line2, line2);
	return record;
}

static void check_orbit_bounds(const satellite_orbit_t *orbit, const satellite_orbit_state_t *state)
{
	double radius = sqrt(state->position_km[0] * state->position_km[0] +
		state->position_km[1] * state->position_km[1] +
		state->position_km[2] * state->position_km[2]);
	double speed = sqrt(state->velocity_km_s[0] * state->velocity_km_s[0] +
		state->velocity_km_s[1] * state->velocity_km_s[1] +
		state->velocity_km_s[2] * state->velocity_km_s[2]);
	CHECK(isfinite(radius) && isfinite(speed));
	CHECK(radius >= orbit->semi_major_axis_km * (1.0 - orbit->eccentricity) - 1.0);
	CHECK(radius <= orbit->semi_major_axis_km * (1.0 + orbit->eccentricity) + 1.0);
}

static void test_keplerian_propagation(void)
{
	satellite_record_t record = record_for(
		"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
		"2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667", 5u, 962131819u);
	satellite_orbit_t orbit;
	CHECK(satellite_orbit_init(&orbit, &record));
	CHECK(fabs(orbit.mean_motion_dot_rad_s2 - 2.0 * 0.00000023 *
		6.28318530717958647693 / (86400.0 * 86400.0)) < 1e-20);
	CHECK(satellite_orbit_is_fresh(&orbit, INT64_C(962131819733568)));
	CHECK(!satellite_orbit_is_fresh(&orbit, INT64_C(962131819733568) + INT64_C(31) * 86400000000));
	CHECK(orbit.semi_major_axis_km > 8600.0 && orbit.semi_major_axis_km < 8700.0);
	satellite_orbit_state_t state;
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_us, &state));
	check_orbit_bounds(&orbit, &state);
	satellite_orbit_state_t after_one_second;
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_us + INT64_C(1000000), &after_one_second));
	for (uint8_t axis = 0u; axis < 3u; axis++) {
		double finite_difference = after_one_second.position_km[axis] - state.position_km[axis];
		CHECK(fabs(finite_difference - state.velocity_km_s[axis]) < 0.01);
	}
	double radius = sqrt(state.position_km[0] * state.position_km[0] +
		state.position_km[1] * state.position_km[1] + state.position_km[2] * state.position_km[2]);
	double speed_squared = state.velocity_km_s[0] * state.velocity_km_s[0] +
		state.velocity_km_s[1] * state.velocity_km_s[1] + state.velocity_km_s[2] * state.velocity_km_s[2];
	double specific_energy = speed_squared / 2.0 - 398600.8 / radius;
	CHECK(fabs(specific_energy + 398600.8 / (2.0 * orbit.semi_major_axis_km)) < 0.2);
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_us + INT64_C(43200) * 1000000, &state));
	check_orbit_bounds(&orbit, &state);
	satellite_observation_t observation;
	CHECK(satellite_orbit_observe(&orbit, INT64_C(962131819733568), 0, 0, 0, &observation));
	CHECK(observation.azimuth_deg >= 0.0 && observation.azimuth_deg < 360.0);
	CHECK(observation.elevation_deg >= -90.0 && observation.elevation_deg <= 90.0);
	CHECK(observation.range_km > 0.0 && isfinite(observation.range_rate_m_s));
	CHECK(!satellite_orbit_observe(&orbit, INT64_C(962131819733568), 900000001, 0, 0, &observation));
	record.line1[68] = record.line1[68] == '9' ? '0' : (char)(record.line1[68] + 1);
	CHECK(!satellite_orbit_init(&orbit, &record));
}

static void test_circular_kepler_orbit(void)
{
	satellite_record_t record = record_for(
		"1 00005U 58002B   00179.78495062  .00000000  00000-0  00000-0 0  4753",
		"2 00005   0.0000   0.0000 0000000   0.0000   0.0000 15.00000000 00000", 5u, 962131819u);
	satellite_orbit_t orbit;
	CHECK(satellite_orbit_init(&orbit, &record));
	satellite_orbit_state_t state;
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_us, &state));
	CHECK(fabs(state.position_km[0] - orbit.semi_major_axis_km) < 1e-7);
	CHECK(fabs(state.position_km[1]) < 1e-7 && fabs(state.position_km[2]) < 1e-7);
	double angular_rate = orbit.mean_motion_rad_s + orbit.argument_perigee_rate_rad_s + orbit.raan_rate_rad_s;
	int64_t quarter_period_us = (int64_t)(1.57079632679489661923 / angular_rate * 1000000.0);
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_us + quarter_period_us, &state));
	CHECK(fabs(state.position_km[0]) < 1e-4);
	CHECK(fabs(state.position_km[1] - orbit.semi_major_axis_km) < 1e-4);
	CHECK(fabs(state.position_km[2]) < 1e-4);
}

static void test_invalid_decimal_field(void)
{
	satellite_record_t record = record_for(
		"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
		"2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667", 5u, 962131819u);
	satellite_orbit_t orbit;
	record.line2[8] = 'x';
	set_checksum(record.line2);
	CHECK(!satellite_orbit_init(&orbit, &record));
	memset(record.line2 + 8u, ' ', 8u);
	set_checksum(record.line2);
	CHECK(!satellite_orbit_init(&orbit, &record));
}

static void test_pass_summary_order(void)
{
	uint32_t summaries[3] = {0};
	uint8_t count = 0u;
	satellite_pass_summary_insert(summaries, &count, 3u,
		satellite_pass_summary_pack(2u, 45u, 41u, false));
	satellite_pass_summary_insert(summaries, &count, 3u,
		satellite_pass_summary_pack(1u, 10u, 36u, false));
	satellite_pass_summary_insert(summaries, &count, 3u,
		satellite_pass_summary_pack(3u, 25u, 35u, false));
	satellite_pass_summary_insert(summaries, &count, 3u,
		satellite_pass_summary_pack(4u, 5u, 90u, true));
	CHECK(count == 3u);
	CHECK(satellite_pass_summary_index(summaries[0]) == 4u);
	CHECK(satellite_pass_summary_aos_minutes(summaries[0]) == 5u);
	CHECK(satellite_pass_summary_maximum_elevation(summaries[0]) == 90u);
	CHECK(satellite_pass_summary_visible_now(summaries[0]));
	CHECK(satellite_pass_summary_index(summaries[1]) == 1u);
	CHECK(satellite_pass_summary_index(summaries[2]) == 3u);
	uint32_t bounded = satellite_pass_summary_pack(17u, UINT16_MAX, 120u, false);
	CHECK(satellite_pass_summary_index(bounded) == 1u);
	CHECK(satellite_pass_summary_aos_minutes(bounded) == 2047u);
	CHECK(satellite_pass_summary_maximum_elevation(bounded) == 90u);
}

static void test_pass_cache_age(void)
{
	CHECK(!satellite_pass_cache_fresh(false, 100u, 100u));
	CHECK(satellite_pass_cache_fresh(true, 100u, 100u));
	CHECK(satellite_pass_cache_fresh(true, 300099u, 100u));
	CHECK(!satellite_pass_cache_fresh(true, 300100u, 100u));
	CHECK(satellite_pass_cache_fresh(true, 198u, UINT32_MAX - 100u));
	CHECK(!satellite_pass_cache_fresh(true, (uint32_t)(UINT32_MAX - 100u + SATELLITE_PASS_CACHE_MS),
					  UINT32_MAX - 100u));
}

static void test_pass_search(void)
{
	satellite_record_t record = record_for(
		"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
		"2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667", 5u, 962131819u);
	satellite_orbit_t orbit;
	CHECK(satellite_orbit_init(&orbit, &record));
	satellite_pass_search_t search;
	CHECK(satellite_pass_search_begin(&search, &orbit, orbit.epoch_us, 6u, 0, 0, 0));
	CHECK(!search.have_sample);
	CHECK(satellite_pass_search_step(&search, &orbit, 0, 0, 0,
					 SATELLITE_PASS_SCAN_SAMPLES_PER_STEP) == SATELLITE_PASS_PENDING);
	CHECK(search.have_sample);
	uint8_t steps = 0;
	while (satellite_pass_search_step(&search, &orbit, 0, 0, 0, 3u) == SATELLITE_PASS_PENDING)
		steps++;
	CHECK(search.status == SATELLITE_PASS_COMPLETE);
	CHECK(steps < 130u);
	if (search.pass_valid) {
		CHECK(search.pass.aos_us < search.pass.los_us);
		CHECK(search.pass.maximum_elevation_deg >= 0.0);
		CHECK(search.pass.maximum_elevation_us >= search.pass.aos_us);
		CHECK(search.pass.maximum_elevation_us <= search.pass.los_us);
		CHECK(search.path_count > 0u);
		CHECK(search.path_count <= SATELLITE_PASS_PATH_POINTS);
		CHECK(search.cursor_us <= search.pass.los_us + INT64_C(60) * 1000000);
		for (uint8_t i = 0u; i < search.path_count; i++) {
			CHECK(satellite_path_azimuth(search.path[i]) < 360u);
			CHECK(satellite_path_elevation(search.path[i]) <= 90u);
		}
		CHECK(satellite_path_elevation(search.path[0]) == 0u);
		CHECK(satellite_path_elevation(search.path[search.path_count - 1u]) == 0u);
		int64_t visible_start_us = search.pass.maximum_elevation_us;
		CHECK(satellite_pass_search_begin(&search, &orbit, visible_start_us, 1u, 0, 0, 0));
		while (satellite_pass_search_step(&search, &orbit, 0, 0, 0,
						 SATELLITE_PASS_MAX_SAMPLES_PER_STEP) == SATELLITE_PASS_PENDING) {
		}
		CHECK(search.status == SATELLITE_PASS_COMPLETE);
		CHECK(search.pass_valid && search.pass.visible_now);
		CHECK(search.pass.aos_us < visible_start_us && search.pass.los_us > visible_start_us);
		CHECK(visible_start_us - search.pass.aos_us <=
		      (int64_t)SATELLITE_PASS_HISTORY_MAX_MINUTES * 60 * 1000000);
		CHECK(search.path_count > 1u);
		CHECK(satellite_path_elevation(search.path[0]) == 0u);
		CHECK(satellite_path_elevation(search.path[search.path_count - 1u]) == 0u);
	}
	CHECK(satellite_pass_search_begin(&search, &orbit, orbit.epoch_us, 1u, 0, 0, 0));
	CHECK(satellite_pass_search_step(&search, &orbit, 10000000, 0, 0, 1u) == SATELLITE_PASS_INVALID);
	CHECK(!satellite_pass_search_begin(&search, &orbit, orbit.epoch_us, 25u, 0, 0, 0));
	bool found_clipped = false;
	for (uint8_t hour = 0u; hour < 24u && !found_clipped; hour++) {
		int64_t start_us = orbit.epoch_us + (int64_t)hour * 3600000000;
		satellite_observation_t end_observation;
		if (!satellite_orbit_observe(&orbit, start_us + 3600000000, 0, 0, 0, &end_observation) ||
		    end_observation.elevation_deg < 0.0)
			continue;
		CHECK(satellite_pass_search_begin(&search, &orbit, start_us, 1u, 0, 0, 0));
		while (satellite_pass_search_step(&search, &orbit, 0, 0, 0,
							 SATELLITE_PASS_MAX_SAMPLES_PER_STEP) == SATELLITE_PASS_PENDING) {
		}
		CHECK(search.status == SATELLITE_PASS_COMPLETE);
		CHECK(search.pass_valid);
		if (search.pass_valid) {
			CHECK(search.pass.horizon_clipped);
			found_clipped = true;
		}
	}
	CHECK(found_clipped);
}

static void test_doppler(void)
{
	satellite_doppler_targets_t targets;
	CHECK(satellite_doppler_calculate(145000000u, 435000000u, 0.0, 100u, &targets));
	CHECK(targets.rx_hz == 145000000u && targets.tx_hz == 435000000u);
	CHECK(targets.rx_offset_hz == 0 && targets.tx_offset_hz == 0);
	CHECK(satellite_doppler_calculate(145000000u, 435000000u, -1000.0, 10u, &targets));
	CHECK(targets.rx_hz > 145000000u && targets.tx_hz < 435000000u);
	CHECK(satellite_doppler_calculate(145000000u, 435000000u, 1000.0, 10u, &targets));
	CHECK(targets.rx_hz < 145000000u && targets.tx_hz > 435000000u);
	CHECK(satellite_doppler_calculate(0u, 435000000u, 1000.0, 10u, &targets));
	CHECK(targets.rx_hz == 0u && targets.rx_offset_hz == 0 && targets.tx_hz != 0u);
	CHECK(!satellite_doppler_calculate(1u, 1u, NAN, 1u, &targets));
	CHECK(!satellite_doppler_calculate(1u, 1u, 0.0, 0u, &targets));
}

static void test_utc_and_tracking_lifecycle(void)
{
	int64_t utc_us;
	CHECK(satellite_utc_from_fields(290224u, 235959u, &utc_us));
	CHECK(satellite_utc_from_fields(10124u, 10000u, &utc_us));
	CHECK(satellite_utc_from_fields(10124u, 120000u, &utc_us));
	CHECK(!satellite_utc_from_fields(290223u, 235959u, &utc_us));
	CHECK(!satellite_utc_from_fields(101200u, 246000u, &utc_us));
	CHECK(!satellite_utc_from_fields(0u, 0xFFFFFFFFu, &utc_us));
	satellite_record_t record = record_for(
		"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
		"2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667", 5u, 962131819u);
	stored_record = record;
	stored_record_available = true;
	satellite_tracking_t tracking = {0};
	CHECK(!satellite_tracking_start(NULL, 0u, 145000000u, 435000000u));
	CHECK(satellite_tracking_start(&tracking, 0u, 145000000u, 435000000u));
	stored_record_available = false;
	position_fix_t fix = { .lat = 0, .lon = 0, .alt_m = 0 };
	CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED, INT64_C(962131819733568),
					1000u, 10u) != SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(tracking.override_active);
	CHECK(tracking.targets.rx_hz != 0u && tracking.targets.tx_hz != 0u);
	CHECK(tracking.pass_search.status == SATELLITE_PASS_PENDING);
	int64_t first_cursor_us = tracking.pass_search.cursor_us;
	fix.lat = 10000;
	CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED,
					INT64_C(962131820733568), 2000u, 10u) != SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(tracking.pass_search_started);
	CHECK(tracking.pass_search.cursor_us == first_cursor_us);
	CHECK(tracking.pass_search.latitude_e7 == 0);
	CHECK(satellite_tracking_forecast_step(&tracking) == SATELLITE_PASS_PENDING);
	CHECK(tracking.pass_search.cursor_us == first_cursor_us + INT64_C(60000000));
	fix.lat = 20000;
	CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED,
					INT64_C(962131821733568), 3000u, 10u) != SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(tracking.pass_search_started);
	CHECK(tracking.pass_search.start_us == INT64_C(962131819733568));
	CHECK(tracking.pass_search.cursor_us == first_cursor_us + INT64_C(60000000));
	fix.lat = 60000;
	CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED,
					INT64_C(962131822733568), 4000u, 10u) != SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(tracking.pass_search_started);
	CHECK(tracking.pass_search.start_us == INT64_C(962131822733568));
	CHECK(tracking.pass_search.cursor_us == tracking.pass_search.start_us);
	CHECK(satellite_tracking_forecast_step(&tracking) == SATELLITE_PASS_PENDING);
	CHECK(tracking.pass_search.cursor_us == tracking.pass_search.start_us + INT64_C(60000000));
	for (uint16_t step = 1u; step < 1500u && tracking.pass_search.status != SATELLITE_PASS_COMPLETE;
	     step++) {
		satellite_tracking_forecast_step(&tracking);
	}
	CHECK(tracking.pass_search.status == SATELLITE_PASS_COMPLETE);
	CHECK(tracking.pass_forecast_valid == tracking.pass_search.pass_valid);
	CHECK(satellite_tracking_next_pass(&tracking) ==
	      (tracking.pass_forecast_valid ? &tracking.pass_search.pass : NULL));
	CHECK(tracking.pass_forecast_valid);
	if (tracking.pass_forecast_valid) {
		int64_t retained_start_us = tracking.pass_search.start_us;
		int64_t retained_cursor_us = tracking.pass_search.cursor_us;
		fix.lat = 120000;
		CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED,
					tracking.pass_search.pass.maximum_elevation_us,
					200000u, 10u) == SATELLITE_TRACKING_VISIBLE);
		CHECK(tracking.pass_search_started);
		CHECK(tracking.pass_search.start_us == retained_start_us);
		CHECK(tracking.pass_search.cursor_us == retained_cursor_us);
		fix.lat = 60000;
		int64_t previous_los_us = tracking.pass_search.pass.los_us;
		int64_t next_start_us = previous_los_us + INT64_C(60) * INT64_C(1000000);
		CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED, next_start_us,
						100000u, 10u) != SATELLITE_TRACKING_UNAVAILABLE);
		CHECK(tracking.pass_search_started);
		CHECK(tracking.pass_search.start_us == next_start_us);
	}
	CHECK(satellite_tracking_update(&tracking, NULL, POSITION_SRC_NONE, INT64_C(962131819733568),
					101000u, 10u) == SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(!tracking.override_active);
	CHECK(!tracking.pass_forecast_valid && !tracking.pass_search_started);
	CHECK(satellite_tracking_next_pass(&tracking) == NULL);
	satellite_tracking_stop(&tracking);
	CHECK(!tracking.enabled && !tracking.override_active);
	CHECK(tracking.targets.rx_hz == tracking.nominal_rx_hz && tracking.targets.tx_hz == tracking.nominal_tx_hz);
}

int main(void)
{
	test_keplerian_propagation();
	test_circular_kepler_orbit();
	test_invalid_decimal_field();
	test_pass_summary_order();
	test_pass_cache_age();
	test_pass_search();
	test_doppler();
	test_utc_and_tracking_lifecycle();
	if (failures != 0) {
		printf("# satelliteOrbitTest FAILED (%d checks, %d failures)\n", checks, failures);
		return 1;
	}
	printf("# satelliteOrbitTest PASSED (%d checks, 0 failures)\n", checks);
	return 0;
}