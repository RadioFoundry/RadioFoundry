// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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

static uint64_t integer_sqrt(uint64_t value)
{
	uint64_t result = 0u;
	uint64_t bit = UINT64_C(1) << 62;
	while (bit > value)
		bit >>= 2;
	while (bit != 0u) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}
	return result;
}

static void set_checksum(char line[70])
{
	unsigned sum = 0u;
	for (uint8_t i = 0u; i < 68u; i++) {
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

static satellite_record_t record_for(const char *line1, const char *line2, uint32_t catalog_id,
				     uint32_t epoch)
{
	satellite_record_t record = { .catalog_id = catalog_id, .epoch = epoch };
	set_line(record.line1, line1);
	set_line(record.line2, line2);
	return record;
}

static satellite_record_t test_record(void)
{
	return record_for(
		"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
		"2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667",
		5u, 962131819u);
}

static void check_sgp4_reference(const char *line1, const char *line2, uint32_t catalog_id,
				 const int32_t expected_m[3][3])
{
	static const uint8_t days[] = { 1u, 3u, 7u };
	satellite_record_t record = record_for(line1, line2, catalog_id, 0u);
	int64_t epoch_s;
	uint16_t fraction_q16;
	if (!satellite_tle_epoch(record.line1, &epoch_s, &fraction_q16) ||
	    epoch_s < 0 || (uint64_t)epoch_s > UINT32_MAX) {
		CHECK(false);
		return;
	}
	record.epoch = (uint32_t)epoch_s;
	satellite_orbit_t orbit;
	if (!satellite_orbit_init(&orbit, &record)) {
		CHECK(false);
		return;
	}
	for (uint8_t sample = 0u; sample < sizeof(days); sample++) {
		satellite_orbit_state_t state;
		int64_t sample_s = epoch_s + (int64_t)days[sample] * 86400;
		if (!satellite_orbit_at(&orbit, sample_s, &state)) {
			CHECK(false);
			continue;
		}
		int64_t error_squared_m = 0;
		for (uint8_t axis = 0u; axis < 3u; axis++) {
			int64_t error_m = (int64_t)state.position_m[axis] - expected_m[sample][axis];
			error_squared_m += error_m * error_m;
		}
		CHECK(error_squared_m <= INT64_C(25000) * 25000);
	}
}

static void test_sgp4_reference_vectors(void)
{
	/* Vallado verification TLEs from
	 * https://github.com/brandon-rhodes/python-sgp4/blob/master/sgp4/SGP4-VER.TLE.
	 * SGP4 positions use the MIT-licensed python-sgp4 2.27 at epoch +1, +3, and +7 days.
	 * The 25 km bound allows for the firmware's simplified Kepler/J2 model.
	 */
	static const int32_t vanguard_m[3][3] = {
		{ -938559, -6268187, -4294029 },
		{ -9060474, 4658710, 813687 },
		{ -2591256, -6292441, -4558812 },
	};
	static const int32_t delta_1_deb_m[3][3] = {
		{ -2777147, -5663160, -2462549 },
		{ 946926, -3781660, -5557692 },
		{ 6320808, 2422330, -610513 },
	};
	static const int32_t cbers_2_m[3][3] = {
		{ 688161, 4124876, 5794560 },
		{ -2543092, -6454421, 1740216 },
		{ 1254184, 6290291, 3160757 },
	};
	static const int32_t iss_high_drag_m[3][3] = {
		{ 1899939, 6527508, -193309 },
		{ 2730303, 6204851, -548542 },
		{ 4104022, 5252472, -1351052 },
	};
	check_sgp4_reference(
		"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
		"2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667",
		5u, vanguard_m);
	check_sgp4_reference(
		"1 06251U 62025E   06176.82412014  .00008885  00000-0  12808-3 0  3985",
		"2 06251  58.0579  54.0425 0030035 139.1568 221.1854 15.56387291  6774",
		6251u, delta_1_deb_m);
	check_sgp4_reference(
		"1 28057U 03049A   06177.78615833  .00000060  00000-0  35940-4 0  1836",
		"2 28057  98.4283 247.6961 0000884  88.1964 271.9322 14.35478080140550",
		28057u, cbers_2_m);
	/* CelesTrak ISS TLE (fetched 2026-11-18, B* = 2.3055e-4); SGP4 vectors use python-sgp4 2.27:
	 * https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE
	 */
	check_sgp4_reference(
		"1 25544U 98067A   26322.30200954  .00012483  00000+0  23055-3 0  9993",
		"2 25544  51.6316 257.4339 0007387  38.9132 321.2387 15.49454822595647",
		25544u, iss_high_drag_m);
}

static void test_keplerian_propagation(void)
{
	satellite_record_t record = test_record();
	satellite_orbit_t orbit;
	CHECK(satellite_orbit_init(&orbit, &record));
	CHECK(satellite_orbit_is_fresh(&orbit, 962131819));
	CHECK(!satellite_orbit_is_fresh(&orbit, 962131819 + INT64_C(31) * 86400));
	CHECK(orbit.semi_major_axis_m > 8600000u && orbit.semi_major_axis_m < 8700000u);

	satellite_orbit_state_t state, after_one_second;
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_s, &state));
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_s + 1, &after_one_second));
	/* Kepler/J2 reference position at epoch_s + 30 days. */
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_s + INT64_C(30) * 86400, &state));
	CHECK(state.position_m[0] >= -1318535 && state.position_m[0] <= -1318335);
	CHECK(state.position_m[1] >= -8520125 && state.position_m[1] <= -8519925);
	CHECK(state.position_m[2] >= 447429 && state.position_m[2] <= 447629);
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_s, &state));
	uint64_t radius_squared = 0u, speed_squared = 0u;
	for (uint8_t axis = 0u; axis < 3u; axis++) {
		int64_t position = state.position_m[axis];
		int64_t velocity = state.velocity_cm_s[axis];
		radius_squared += (uint64_t)(position * position);
		speed_squared += (uint64_t)(velocity * velocity);
		int32_t displacement_cm = (after_one_second.position_m[axis] - state.position_m[axis]) * 100;
		CHECK(displacement_cm - state.velocity_cm_s[axis] < 3000);
		CHECK(state.velocity_cm_s[axis] - displacement_cm < 3000);
	}
	uint32_t radius_m = (uint32_t)integer_sqrt(radius_squared);
	uint32_t speed_cm_s = (uint32_t)integer_sqrt(speed_squared);
	uint32_t perigee_m = (uint32_t)(((uint64_t)orbit.semi_major_axis_m *
		(UINT32_C(1) << 30) - (uint64_t)orbit.semi_major_axis_m * orbit.eccentricity_q30) >> 30);
	uint32_t apogee_m = (uint32_t)(((uint64_t)orbit.semi_major_axis_m *
		(UINT32_C(1) << 30) + (uint64_t)orbit.semi_major_axis_m * orbit.eccentricity_q30) >> 30);
	CHECK(radius_m > perigee_m - 20000u && radius_m < apogee_m + 20000u);
	CHECK(speed_cm_s > 500000u && speed_cm_s < 900000u);

	satellite_observation_t observation;
	CHECK(satellite_orbit_observe(&orbit, orbit.epoch_s, 0, 0, 0, &observation));
	CHECK(observation.azimuth_tenths_deg < 3600u);
	CHECK(observation.elevation_tenths_deg >= -900 && observation.elevation_tenths_deg <= 900);
	CHECK(observation.range_m > 0u);
	CHECK(observation.range_rate_cm_s > -1000000 && observation.range_rate_cm_s < 1000000);
	CHECK(!satellite_orbit_observe(&orbit, orbit.epoch_s, 900000001, 0, 0, &observation));
	record.line1[68] = record.line1[68] == '9' ? '0' : (char)(record.line1[68] + 1);
	CHECK(!satellite_orbit_init(&orbit, &record));
}

static void test_tle_age_days(void)
{
	uint16_t age_days = 0u;
	CHECK(satellite_tle_age_days(1000u, 1000 + INT64_C(13) * 86400 + 86399, &age_days) &&
	      age_days == 13u);
	CHECK(satellite_tle_age_days(1000u, 1000 + INT64_C(14) * 86400, &age_days) &&
	      age_days == 14u);
	CHECK(satellite_tle_age_days(1000u, 999, &age_days) && age_days == 0u);
	CHECK(satellite_tle_age_days(0u, INT64_MAX, &age_days) && age_days == UINT16_MAX);
	CHECK(!satellite_tle_age_days(1000u, 1000, NULL));
}

static void test_circular_kepler_orbit(void)
{
	satellite_record_t record = record_for(
		"1 00005U 58002B   00179.00000000  .00000000  00000-0  00000-0 0  4753",
		"2 00005   0.0000   0.0000 0000000   0.0000   0.0000 15.00000000 00000",
		5u, 962064000u);
	satellite_orbit_t orbit;
	CHECK(satellite_orbit_init(&orbit, &record));
	satellite_orbit_state_t state;
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_s, &state));
	CHECK(state.position_m[0] > (int32_t)orbit.semi_major_axis_m - 10);
	CHECK(state.position_m[0] < (int32_t)orbit.semi_major_axis_m + 10);
	CHECK(state.position_m[1] > -10 && state.position_m[1] < 10);
	CHECK(state.position_m[2] > -10 && state.position_m[2] < 10);
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_s + 1440, &state));
	CHECK(state.position_m[0] > -50000 && state.position_m[0] < 50000);
	CHECK(state.position_m[1] > (int32_t)orbit.semi_major_axis_m - 50000);
	CHECK(state.position_m[1] < (int32_t)orbit.semi_major_axis_m + 50000);
	CHECK(state.position_m[2] > -50000 && state.position_m[2] < 50000);

	record = record_for(
		"1 00005U 58002B   00179.78495062  .00000000  00000-0  00000-0 0  4753",
		"2 00005  34.2682 348.7242 5000000 331.7664  19.3264 10.82419157 00000",
		5u, 962131819u);
	CHECK(satellite_orbit_init(&orbit, &record));
	CHECK(satellite_orbit_at(&orbit, orbit.epoch_s, &state));
}

static void test_invalid_decimal_field(void)
{
	satellite_record_t record = test_record();
	satellite_orbit_t orbit;
	record.line2[8] = 'x';
	set_checksum(record.line2);
	CHECK(!satellite_orbit_init(&orbit, &record));
	record = test_record();
	memset(record.line2 + 8u, ' ', 8u);
	set_checksum(record.line2);
	CHECK(!satellite_orbit_init(&orbit, &record));
}

static void test_orbit_parser_fuzz_loop(void)
{
	satellite_record_t base = test_record();
	uint32_t random_state = UINT32_C(0x54A7E11E);
	for (uint16_t iteration = 0u; iteration < 4096u; iteration++) {
		satellite_record_t record = base;
		random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
		uint8_t mutations = (uint8_t)((random_state >> 29) + 1u);
		for (uint8_t i = 0u; i < mutations; i++) {
			random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
			char *line = (random_state & 1u) != 0u ? record.line1 : record.line2;
			uint8_t start = line == record.line1 ? 18u : 7u;
			uint8_t span = line == record.line1 ? 45u : 56u;
			uint8_t offset = (uint8_t)(start + ((random_state >> 8) % span));
			line[offset] = (char)(32u + ((random_state >> 16) % 95u));
		}
		set_checksum(record.line1);
		set_checksum(record.line2);
		satellite_orbit_t orbit;
		(void)satellite_orbit_init(&orbit, &record);
	}
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
	CHECK(satellite_pass_eta_minutes(3601, 0) == 61u);
	CHECK(satellite_pass_eta_minutes(3600, 0) == 60u);
	CHECK(satellite_pass_eta_minutes(0, 1) == 0u);
	CHECK(satellite_pass_eta_minutes(90000, 0) == 1440u);
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
	CHECK(!satellite_pass_cache_fresh(true,
		(uint32_t)(UINT32_MAX - 100u + SATELLITE_PASS_CACHE_MS), UINT32_MAX - 100u));
}

static void test_pass_search(void)
{
	satellite_record_t record = test_record();
	satellite_orbit_t orbit;
	CHECK(satellite_orbit_init(&orbit, &record));
	satellite_pass_search_t search;
	CHECK(satellite_pass_search_begin(&search, &orbit, orbit.epoch_s, 6u, 0, 0, 0));
	CHECK(!search.have_sample);
	CHECK(satellite_pass_search_step(&search, &orbit, 0, 0, 0,
					 SATELLITE_PASS_SCAN_SAMPLES_PER_STEP) == SATELLITE_PASS_PENDING);
	CHECK(search.have_sample);
	uint8_t steps = 0u;
	while (satellite_pass_search_step(&search, &orbit, 0, 0, 0, 3u) == SATELLITE_PASS_PENDING)
		steps++;
	CHECK(search.status == SATELLITE_PASS_COMPLETE);
	CHECK(steps < 130u);
	if (search.pass_valid) {
		/* Kepler/J2 reference pass at the equator, within one 60-second sample. */
		CHECK(search.pass.aos_s >= orbit.epoch_s + 4320 &&
		      search.pass.aos_s <= orbit.epoch_s + 4440);
		CHECK(search.pass.maximum_elevation_s >= orbit.epoch_s + 5520 &&
		      search.pass.maximum_elevation_s <= orbit.epoch_s + 5640);
		CHECK(search.pass.los_s >= orbit.epoch_s + 6256 &&
		      search.pass.los_s <= orbit.epoch_s + 6376);
		CHECK(search.pass.aos_s < search.pass.los_s);
		CHECK(search.pass.maximum_elevation_tenths_deg > 0u);
		CHECK(search.pass.maximum_elevation_s >= search.pass.aos_s);
		CHECK(search.pass.maximum_elevation_s <= search.pass.los_s);
		CHECK(search.path_count > 0u && search.path_count <= SATELLITE_PASS_PATH_POINTS);
		CHECK(search.cursor_s <= search.pass.los_s + SATELLITE_PASS_STEP_SECONDS);
		for (uint8_t i = 0u; i < search.path_count; i++) {
			CHECK(satellite_path_azimuth(search.path[i]) < 360u);
			CHECK(satellite_path_elevation(search.path[i]) <= 90u);
		}
		CHECK(satellite_path_elevation(search.path[0]) == 0u);
		CHECK(satellite_path_elevation(search.path[search.path_count - 1u]) == 0u);
		int64_t visible_start_s = search.pass.maximum_elevation_s;
		CHECK(satellite_pass_search_begin(&search, &orbit, visible_start_s, 1u, 0, 0, 0));
		while (satellite_pass_search_step(&search, &orbit, 0, 0, 0,
						 SATELLITE_PASS_MAX_SAMPLES_PER_STEP) == SATELLITE_PASS_PENDING) {
		}
		CHECK(search.status == SATELLITE_PASS_COMPLETE);
		CHECK(search.pass_valid && search.pass.visible_now);
		CHECK(search.pass.aos_s < visible_start_s && search.pass.los_s > visible_start_s);
		CHECK(visible_start_s - search.pass.aos_s <=
		      (int64_t)SATELLITE_PASS_HISTORY_MAX_MINUTES * 60);
		CHECK(search.path_count > 1u);
		CHECK(satellite_path_elevation(search.path[0]) == 0u);
		CHECK(satellite_path_elevation(search.path[search.path_count - 1u]) == 0u);
	}
	CHECK(satellite_pass_search_begin(&search, &orbit, orbit.epoch_s, 1u, 0, 0, 0));
	CHECK(satellite_pass_search_step(&search, &orbit, 10000000, 0, 0, 1u) == SATELLITE_PASS_INVALID);
	CHECK(!satellite_pass_search_begin(&search, &orbit, orbit.epoch_s, 25u, 0, 0, 0));
	satellite_record_t geostationary_record = record_for(
		"1 00005U 58002B   00179.78495062  .00000000  00000-0  00000-0 0  4753",
		"2 00005   0.0000   0.0000 0000000   0.0000   0.0000  1.00270000 00000",
		5u, 962131819u);
	CHECK(satellite_orbit_init(&orbit, &geostationary_record));
	CHECK(satellite_pass_search_begin(&search, &orbit, orbit.epoch_s, 1u,
					  0, 900000000, 0));
	while (satellite_pass_search_step(&search, &orbit, 0, 900000000, 0,
					 SATELLITE_PASS_MAX_SAMPLES_PER_STEP) == SATELLITE_PASS_PENDING) {
	}
	CHECK(search.status == SATELLITE_PASS_COMPLETE);
	CHECK(search.pass_valid);
	CHECK(search.pass.horizon_clipped);
}

static void test_doppler(void)
{
	satellite_doppler_targets_t targets;
	CHECK(satellite_doppler_calculate(145000000u, 435000000u, 0, 100u, &targets));
	CHECK(targets.rx_hz == 145000000u && targets.tx_hz == 435000000u);
	CHECK(targets.rx_offset_hz == 0 && targets.tx_offset_hz == 0);
	CHECK(satellite_doppler_calculate(145000000u, 435000000u, -100000, 10u, &targets));
	CHECK(targets.rx_hz > 145000000u && targets.tx_hz < 435000000u);
	CHECK(satellite_doppler_calculate(145000000u, 435000000u, 100000, 10u, &targets));
	CHECK(targets.rx_hz == 144999520u && targets.tx_hz == 435001450u);
	CHECK(satellite_doppler_calculate(0u, 435000000u, 100000, 10u, &targets));
	CHECK(targets.rx_hz == 0u && targets.rx_offset_hz == 0 && targets.tx_hz != 0u);
	CHECK(!satellite_doppler_calculate(UINT32_MAX, UINT32_MAX, INT32_MAX, 1u, &targets));
	CHECK(!satellite_doppler_calculate(1u, 1u, 0, 0u, &targets));
}

static void test_utc_and_tracking_lifecycle(void)
{
	int64_t utc_s;
	CHECK(satellite_utc_from_fields(290224u, 235959u, &utc_s));
	CHECK(utc_s == INT64_C(1709251199));
	CHECK(satellite_utc_from_fields(10124u, 10000u, &utc_s));
	CHECK(satellite_utc_from_fields(10124u, 120000u, &utc_s));
	CHECK(!satellite_utc_from_fields(290223u, 235959u, &utc_s));
	CHECK(!satellite_utc_from_fields(101200u, 246000u, &utc_s));
	CHECK(!satellite_utc_from_fields(0u, 0xFFFFFFFFu, &utc_s));
	stored_record = test_record();
	stored_record_available = true;
	satellite_tracking_t tracking = {0};
	CHECK(!satellite_tracking_start(NULL, 0u, 145000000u, 435000000u));
	CHECK(satellite_tracking_start(&tracking, 0u, 145000000u, 435000000u));
	stored_record_available = false;
	position_fix_t fix = { .lat = 0, .lon = 0, .alt_m = 0 };
	CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED, 962131819,
					1000u, 10u) != SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(tracking.override_active);
	CHECK(tracking.targets.rx_hz != 0u && tracking.targets.tx_hz != 0u);
	CHECK(tracking.pass_search.status == SATELLITE_PASS_PENDING);
	int64_t first_cursor_s = tracking.pass_search.cursor_s;
	fix.lat = 10000;
	CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED,
					962131820, 2000u, 10u) != SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(tracking.pass_search_started);
	CHECK(tracking.pass_search.cursor_s == first_cursor_s);
	CHECK(tracking.pass_search.latitude_e7 == 0);
	CHECK(satellite_tracking_forecast_step(&tracking) == SATELLITE_PASS_PENDING);
	CHECK(tracking.pass_search.cursor_s == first_cursor_s + SATELLITE_PASS_STEP_SECONDS);
	fix.lat = 20000;
	CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED,
					962131821, 3000u, 10u) != SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(tracking.pass_search_started);
	CHECK(tracking.pass_search.start_s == 962131819);
	CHECK(tracking.pass_search.cursor_s == first_cursor_s + SATELLITE_PASS_STEP_SECONDS);
	fix.lat = 60000;
	CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED,
					962131822, 4000u, 10u) != SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(tracking.pass_search_started);
	CHECK(tracking.pass_search.start_s == 962131822);
	CHECK(tracking.pass_search.cursor_s == tracking.pass_search.start_s);
	CHECK(satellite_tracking_forecast_step(&tracking) == SATELLITE_PASS_PENDING);
	CHECK(tracking.pass_search.cursor_s == tracking.pass_search.start_s + SATELLITE_PASS_STEP_SECONDS);
	for (uint16_t step = 1u; step < 1500u &&
	     tracking.pass_search.status != SATELLITE_PASS_COMPLETE; step++) {
		(void)satellite_tracking_forecast_step(&tracking);
	}
	CHECK(tracking.pass_search.status == SATELLITE_PASS_COMPLETE);
	CHECK(tracking.pass_forecast_valid == tracking.pass_search.pass_valid);
	CHECK(satellite_tracking_next_pass(&tracking) ==
	      (tracking.pass_forecast_valid ? &tracking.pass_search.pass : NULL));
	CHECK(tracking.pass_forecast_valid);
	if (tracking.pass_forecast_valid) {
		int64_t retained_start_s = tracking.pass_search.start_s;
		int64_t retained_cursor_s = tracking.pass_search.cursor_s;
		fix.lat = 120000;
		CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED,
					tracking.pass_search.pass.maximum_elevation_s,
					200000u, 10u) == SATELLITE_TRACKING_VISIBLE);
		CHECK(tracking.pass_search_started);
		CHECK(tracking.pass_search.start_s == retained_start_s);
		CHECK(tracking.pass_search.cursor_s == retained_cursor_s);
		fix.lat = 60000;
		int64_t next_start_s = tracking.pass_search.pass.los_s + 60;
		CHECK(satellite_tracking_update(&tracking, &fix, POSITION_SRC_FIXED, next_start_s,
						100000u, 10u) != SATELLITE_TRACKING_UNAVAILABLE);
		CHECK(tracking.pass_search_started);
		CHECK(tracking.pass_search.start_s == next_start_s);
	}
	CHECK(satellite_tracking_update(&tracking, NULL, POSITION_SRC_NONE, 962131819,
					101000u, 10u) == SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(!tracking.override_active);
	CHECK(!tracking.pass_forecast_valid && !tracking.pass_search_started);
	CHECK(satellite_tracking_next_pass(&tracking) == NULL);
	satellite_tracking_stop(&tracking);
	CHECK(!tracking.enabled && !tracking.override_active);
	CHECK(tracking.targets.rx_hz == tracking.nominal_rx_hz &&
	      tracking.targets.tx_hz == tracking.nominal_tx_hz);
}

int main(void)
{
	test_tle_age_days();
	test_sgp4_reference_vectors();
	test_keplerian_propagation();
	test_circular_kepler_orbit();
	test_invalid_decimal_field();
	test_orbit_parser_fuzz_loop();
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
