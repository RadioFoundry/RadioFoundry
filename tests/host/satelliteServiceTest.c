// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stdio.h>
#include <string.h>
#include "position/position.h"
#include "satellite/service.h"

static int failures, checks;
static bool fix_available, time_available, record_available, summary_available;
static uint8_t installed_count;
static uint32_t summary_reads, list_reads, store_generation;
static bool change_generation_on_summary, change_generation_on_record;
static satellite_record_t stored_record;
static satellite_record_summary_t stored_summary;
static satellite_record_summary_t second_summary;
static bool service_locked;
static satellite_service_t *store_reader;
static const satellite_frequency_profile_t stored_profile = {
	.catalog_id = 5u, .rx_hz = 145800000u, .tx_hz = 437800000u,
};

#define CHECK(condition)                                                                                           \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(condition)) {                                                                                \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                 \
		}                                                                                                  \
	} while (0)

static void set_service_locked(bool locked)
{
	CHECK(service_locked != locked);
	service_locked = locked;
}

bool satellite_store_get(uint8_t index, satellite_record_t *record)
{
	CHECK(!service_locked);
	if (index != 0u || !record_available || record == NULL)
		return false;
	*record = stored_record;
	if (change_generation_on_record) {
		change_generation_on_record = false;
		store_generation++;
	}
	if (store_reader != NULL) {
		satellite_path_view_t path;
		satellite_service_live_view_t live;
		set_service_locked(true);
		satellite_service_path_view(store_reader, &path);
		satellite_service_live_view(store_reader, &live);
		set_service_locked(false);
		CHECK(path.count == 0u);
		CHECK(!live.pass_valid && live.elevation_tenths == 0);
	}
	return true;
}

bool satellite_store_summary_get(uint8_t index, satellite_record_summary_t *summary)
{
	CHECK(!service_locked);
	summary_reads++;
	if (index > 1u || (index == 1u && installed_count < 2u) || !summary_available || summary == NULL)
		return false;
	*summary = index == 0u ? stored_summary : second_summary;
	if (change_generation_on_summary) {
		change_generation_on_summary = false;
		store_generation++;
	}
	return true;
}

uint8_t satellite_store_count(void)
{
	CHECK(!service_locked);
	return installed_count;
}

uint32_t satellite_store_generation(void)
{
	CHECK(!service_locked);
	return store_generation;
}

bool satellite_store_list(uint16_t start, uint8_t limit, satellite_record_summary_t *records, uint8_t *count)
{
	CHECK(!service_locked);
	list_reads++;
	if (count == NULL || (limit != 0u && records == NULL))
		return false;
	*count = 0u;
	if (start == 0u && installed_count != 0u && limit != 0u) {
		records[0] = stored_summary;
		*count = 1u;
	}
	return true;
}

bool satellite_store_profile_get(uint32_t catalog_id, satellite_frequency_profile_t *profile)
{
	CHECK(!service_locked);
	if (catalog_id != stored_profile.catalog_id || profile == NULL)
		return false;
	*profile = stored_profile;
	return true;
}

uint8_t position_fix_for_satellite(position_fix_t *out, uint32_t now_ms)
{
	(void)now_ms;
	if (!fix_available || out == NULL)
		return POSITION_SRC_NONE;
	*out = (position_fix_t){ .lat = 0, .lon = 0, .alt_m = 0 };
	return POSITION_SRC_FIXED;
}

bool position_utc_fields(uint32_t *date_ddmmyy, uint32_t *utc_hms, uint32_t now_ms)
{
	(void)now_ms;
	if (!time_available || date_ddmmyy == NULL || utc_hms == NULL)
		return false;
	*date_ddmmyy = 270600u;
	*utc_hms = 185019u;
	return true;
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
	line[69] = '\0';
}

static void set_line(char out[70], const char *text)
{
	memset(out, ' ', 69u);
	memcpy(out, text, strlen(text));
	set_checksum(out);
}

static void setup_record(void)
{
	stored_record = (satellite_record_t){ .catalog_id = 5u, .name_length = 3u };
	memcpy(stored_record.name, "ISS", 4u);
	set_line(stored_record.line1,
		 "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753");
	set_line(stored_record.line2,
		 "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667");
	uint16_t fraction_q16;
	int64_t epoch_s;
	if (satellite_tle_epoch(stored_record.line1, &epoch_s, &fraction_q16))
		stored_record.epoch = (uint32_t)epoch_s;
	stored_summary = (satellite_record_summary_t){
		.catalog_id = 5u, .epoch = stored_record.epoch, .name_length = 3u,
	};
	memcpy(stored_summary.name, "ISS", 4u);
}

static void test_tracking_request_lifecycle(void)
{
	satellite_service_t service;
	satellite_service_update_t update;
	satellite_service_channel_t channel = { .channel_number = 1u };
	satellite_service_init(&service);
	store_reader = &service;
	record_available = true;
	summary_available = true;
	satellite_service_tracking_request(&service, true, 0u, 100u, &channel, &update);
	CHECK(update.tracking && update.record_index == 0u);
	CHECK(update.status == SATELLITE_TRACKING_UNAVAILABLE);
	CHECK(update.nominal_rx_hz == stored_profile.rx_hz && update.nominal_tx_hz == stored_profile.tx_hz);
	CHECK(update.tx_tone_tenths == stored_profile.tx_tone_tenths);
	time_available = false;
	CHECK(satellite_service_tick(&service, 1100u, &channel, &update));
	CHECK(update.tracking && update.status == SATELLITE_TRACKING_UNAVAILABLE);
	channel.channel_number++;
	CHECK(satellite_service_tick(&service, 2100u, &channel, &update));
	CHECK(!update.tracking && update.status == SATELLITE_TRACKING_OFF);
	store_reader = NULL;
	summary_available = false;
	satellite_service_tracking_request(&service, true, 0u, 2200u, &channel, &update);
	CHECK(!update.tracking && update.status == SATELLITE_TRACKING_OFF);
}

static void test_service_init_caches_summaries(void)
{
	satellite_service_t service;
	installed_count = 1u;
	summary_available = true;
	summary_reads = 0u;
	list_reads = 0u;
	satellite_service_init(&service);
	CHECK(service.scan_total == 1u && summary_reads == 1u && list_reads == 0u);
	satellite_record_brief_t summary;
	CHECK(satellite_service_summary(&service, 0u, &summary));
	uint32_t epoch;
	CHECK(summary.catalog_id == 0u &&
	      strcmp(summary.name, "ISS") == 0 && summary_reads == 1u);
	CHECK(satellite_service_oldest_tle_epoch(&service, &epoch) && epoch == stored_summary.epoch);
	installed_count = 0u;
}

static void test_oldest_tle_epoch(void)
{
	satellite_service_t service;
	uint32_t epoch;
	installed_count = 2u;
	summary_available = true;
	second_summary = stored_summary;
	second_summary.epoch = stored_summary.epoch - 86400u;
	satellite_service_init(&service);
	CHECK(satellite_service_oldest_tle_epoch(&service, &epoch) && epoch == second_summary.epoch);
	second_summary.epoch = stored_summary.epoch + 86400u;
	store_generation++;
	satellite_service_scan_request(&service, false, 100u, NULL);
	CHECK(satellite_service_oldest_tle_epoch(&service, &epoch) && epoch == stored_summary.epoch);
	installed_count = 0u;
	store_generation++;
	satellite_service_scan_request(&service, false, 200u, NULL);
	CHECK(!satellite_service_oldest_tle_epoch(&service, &epoch));
}

static void test_scan_cache_and_summary(void)
{
	satellite_service_t service;
	installed_count = 0u;
	summary_available = true;
	summary_reads = 0u;
	list_reads = 0u;
	satellite_service_init(&service);
	uint8_t status, completed, total, passes;
	int64_t start_utc_s;
	uint32_t start_ms;
	installed_count = 1u;
	list_reads = 0u;
	fix_available = false;
	time_available = false;
	CHECK(!satellite_service_scan_request(&service, true, 1000u, NULL));
	satellite_service_scan_state(&service, &status, &completed, &total, &passes, &start_utc_s, &start_ms);
	CHECK(status == SATELLITE_SERVICE_SCAN_UNAVAILABLE && total == 1u);
	CHECK(list_reads == 0u && summary_reads == 1u);
	satellite_record_brief_t summary;
	CHECK(satellite_service_summary(&service, 0u, &summary));
	CHECK(strcmp(summary.name, "ISS") == 0);

	fix_available = true;
	time_available = true;
	installed_count = 0u;
	CHECK(!satellite_service_scan_request(&service, true, 2000u, NULL));
	satellite_service_scan_state(&service, &status, &completed, &total, &passes, &start_utc_s, &start_ms);
	CHECK(status == SATELLITE_SERVICE_SCAN_COMPLETE && total == 0u && passes == 0u && start_ms == 2000u);
	CHECK(!satellite_service_scan_request(&service, true, 3000u, NULL));
	satellite_service_scan_state(&service, &status, &completed, &total, &passes, NULL, &start_ms);
	CHECK(status == SATELLITE_SERVICE_SCAN_COMPLETE && start_ms == 2000u);

	installed_count = 1u;
	summary_reads = 0u;
	list_reads = 0u;
	CHECK(!satellite_service_scan_request(&service, true, 302001u, NULL));
	satellite_service_scan_state(&service, &status, &completed, &total, &passes, NULL, NULL);
	CHECK(status == SATELLITE_SERVICE_SCAN_RUNNING && total == 1u);
	CHECK(list_reads == 0u && summary_reads == 1u);
	satellite_service_scan_step(&service, 302001u);
	CHECK(service.scan_search_started);
	CHECK(!satellite_service_scan_request(&service, false, 302002u, NULL));
	satellite_service_scan_state(&service, &status, NULL, NULL, NULL, NULL, NULL);
	CHECK(status == SATELLITE_SERVICE_SCAN_IDLE);
}

static void test_store_generation_invalidates_cached_inventory(void)
{
	satellite_service_t service;
	installed_count = 0u;
	store_generation = 0u;
	summary_available = true;
	fix_available = true;
	time_available = true;
	list_reads = 0u;
	summary_reads = 0u;
	satellite_service_init(&service);
	CHECK(!satellite_service_scan_request(&service, true, 100u, NULL));
	CHECK(service.scan_cache_valid && service.scan_total == 0u);

	installed_count = 1u;
	store_generation++;
	CHECK(!satellite_service_scan_request(&service, true, 200u, NULL));
	CHECK(service.scan_status == SATELLITE_SERVICE_SCAN_RUNNING && service.scan_total == 1u);
	CHECK(list_reads == 0u && summary_reads == 1u);
	satellite_record_brief_t summary;
	CHECK(satellite_service_summary(&service, 0u, &summary));
	CHECK(summary.catalog_id == 0u && strcmp(summary.name, "ISS") == 0);
}

static void test_inventory_failure_and_generation_race(void)
{
	satellite_service_t service;
	installed_count = 1u;
	summary_available = true;
	fix_available = true;
	time_available = true;
	satellite_service_init(&service);
	store_generation++;
	summary_available = false;
	CHECK(!satellite_service_scan_request(&service, true, 100u, NULL));
	CHECK(service.scan_status == SATELLITE_SERVICE_SCAN_UNAVAILABLE && service.scan_total == 1u);

	summary_available = true;
	CHECK(!satellite_service_scan_request(&service, true, 200u, NULL));
	CHECK(service.scan_status == SATELLITE_SERVICE_SCAN_RUNNING && service.scan_total == 1u);
	CHECK(!satellite_service_scan_request(&service, false, 201u, NULL));
	store_generation++;
	change_generation_on_summary = true;
	CHECK(!satellite_service_scan_request(&service, true, 300u, NULL));
	CHECK(service.scan_status == SATELLITE_SERVICE_SCAN_UNAVAILABLE && service.scan_total == 1u);
	CHECK(!satellite_service_scan_request(&service, true, 400u, NULL));
	CHECK(service.scan_status == SATELLITE_SERVICE_SCAN_RUNNING && service.scan_total == 1u);
	record_available = true;
	change_generation_on_record = true;
	store_reader = &service;
	satellite_service_scan_step(&service, 400u);
	store_reader = NULL;
	CHECK(service.scan_status == SATELLITE_SERVICE_SCAN_UNAVAILABLE && service.scan_result_count == 0u);
}

static void test_tracking_rejects_bad_record(void)
{
	satellite_service_t service;
	satellite_service_update_t update;
	satellite_service_channel_t channel = { .channel_number = 1u };
	installed_count = 1u;
	summary_available = true;
	record_available = true;
	satellite_service_init(&service);
	char original = stored_record.line1[0];
	stored_record.line1[0] = 'X';
	store_reader = &service;
	satellite_service_tracking_request(&service, true, 0u, 100u, &channel, &update);
	store_reader = NULL;
	stored_record.line1[0] = original;
	CHECK(!service.tracking_requested && !update.tracking && update.status == SATELLITE_TRACKING_OFF);
	change_generation_on_record = true;
	satellite_service_tracking_request(&service, true, 0u, 200u, &channel, &update);
	CHECK(!service.tracking_requested && !update.tracking);
	satellite_service_tracking_request(&service, true, 0u, 300u, &channel, &update);
	CHECK(service.tracking_requested && update.tracking);
}

static void test_scan_result_keeps_rank_and_record_index_together(void)
{
	satellite_service_t service = { 0 };
	service.scan_total = 2u;
	service.scan_result_count = 2u;
	service.scan_results[0] = satellite_pass_summary_pack(1u, 34u, 40u, false);
	service.scan_results[1] = satellite_pass_summary_pack(0u, 17u, 50u, false);
	uint32_t packed;
	int64_t aos_s;
	service.scan_results[0] = satellite_pass_summary_set_aos_remainder(service.scan_results[0], 20u);
	service.scan_results[1] = satellite_pass_summary_set_aos_remainder(service.scan_results[1], 40u);
	CHECK(satellite_service_scan_result(&service, 0u, &packed, &aos_s));
	CHECK(satellite_pass_summary_index(packed) == 1u && aos_s == 2000);
	CHECK(satellite_service_scan_result(&service, 1u, &packed, &aos_s));
	CHECK(satellite_pass_summary_index(packed) == 0u && aos_s == 1000);
	service.scan_results[0] = satellite_pass_summary_set_aos_remainder(
		satellite_pass_summary_pack(1u, 1440u, 40u, false), 30u);
	CHECK(satellite_service_scan_result(&service, 0u, &packed, &aos_s) && aos_s == 86370);
}

int main(void)
{
	satellite_service_set_lock(set_service_locked);
	setup_record();
	test_service_init_caches_summaries();
	test_oldest_tle_epoch();
	test_tracking_request_lifecycle();
	test_scan_cache_and_summary();
	test_store_generation_invalidates_cached_inventory();
	test_inventory_failure_and_generation_race();
	test_tracking_rejects_bad_record();
	test_scan_result_keeps_rank_and_record_index_together();
	if (failures != 0) {
		printf("satelliteServiceTest FAILED (%d checks, %d failures)\n", checks, failures);
		return 1;
	}
	printf("satelliteServiceTest PASSED (%d checks, 0 failures)\n", checks);
	return 0;
}
