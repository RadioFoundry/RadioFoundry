// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stdio.h>
#include <string.h>
#include "position/position.h"
#include "satellite/service.h"

static int failures, checks;
static bool fix_available, time_available, record_available, summary_available;
static uint8_t installed_count;
static uint32_t summary_reads, list_reads;
static satellite_record_t stored_record;
static satellite_record_summary_t stored_summary;
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

bool satellite_store_get(uint8_t index, satellite_record_t *record)
{
	if (index != 0u || !record_available || record == NULL)
		return false;
	*record = stored_record;
	return true;
}

bool satellite_store_summary_get(uint8_t index, satellite_record_summary_t *summary)
{
	summary_reads++;
	if (index != 0u || !summary_available || summary == NULL)
		return false;
	*summary = stored_summary;
	return true;
}

uint8_t satellite_store_count(void)
{
	return installed_count;
}

bool satellite_store_list(uint16_t start, uint8_t limit, satellite_record_summary_t *records, uint8_t *count)
{
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
	summary_available = false;
	satellite_service_tracking_request(&service, true, 0u, 2200u, &channel, &update);
	CHECK(!update.tracking && update.status == SATELLITE_TRACKING_OFF);
}

static void test_service_init_caches_summaries(void)
{
	satellite_service_t service;
	installed_count = 1u;
	list_reads = 0u;
	satellite_service_init(&service);
	CHECK(service.scan_total == 1u && list_reads == 1u);
	satellite_record_summary_t summary;
	CHECK(satellite_service_summary(&service, 0u, &summary));
	CHECK(summary.catalog_id == stored_summary.catalog_id && strcmp(summary.name, "ISS") == 0);
	installed_count = 0u;
}

static void test_scan_cache_and_summary(void)
{
	satellite_service_t service;
	installed_count = 0u;
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
	CHECK(list_reads == 1u);
	satellite_record_summary_t summary;
	CHECK(satellite_service_summary(&service, 0u, &summary));
	CHECK(summary.catalog_id == stored_summary.catalog_id && strcmp(summary.name, "ISS") == 0);

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
	CHECK(list_reads == 1u && summary_reads == 0u);
	CHECK(satellite_service_summary(&service, 0u, &summary));
	CHECK(summary.catalog_id == stored_summary.catalog_id && strcmp(summary.name, "ISS") == 0);
	CHECK(summary_reads == 0u);
	satellite_service_scan_step(&service, 302001u);
	CHECK(service.scan_search_started);
	CHECK(!satellite_service_scan_request(&service, false, 302002u, NULL));
	satellite_service_scan_state(&service, &status, NULL, NULL, NULL, NULL, NULL);
	CHECK(status == SATELLITE_SERVICE_SCAN_IDLE);
}

int main(void)
{
	setup_record();
	test_service_init_caches_summaries();
	test_tracking_request_lifecycle();
	test_scan_cache_and_summary();
	if (failures != 0) {
		printf("satelliteServiceTest FAILED (%d checks, %d failures)\n", checks, failures);
		return 1;
	}
	printf("satelliteServiceTest PASSED (%d checks, 0 failures)\n", checks);
	return 0;
}
