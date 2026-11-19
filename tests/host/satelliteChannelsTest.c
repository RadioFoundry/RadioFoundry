// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host tests for codeplug channels created from the installed satellite inventory.
#include <stdio.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/zone_edit.h"
#include "radio/radio.h"
#include "satellite/channels.h"
#include "satellite/frequency.h"
#include "satellite/store.h"
#include "storage/storage.h"

static satellite_record_t records[SATELLITE_STORE_MAX_RECORDS];
static uint8_t record_count;
static zone_t zones[4];
static uint16_t zone_count;
static channel_t channels[16];
static bool channel_used[16];
static uint16_t channel_writes;
static channel_t profile;
static storage_job_fn pending_job;
static void *pending_job_ctx;
static bool block_job_post;
static unsigned checks, failures;

#define CHECK(condition) do { \
	checks++; \
	if (!(condition)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
	} \
} while (0)

bool storage_job_post(storage_job_fn fn, void *ctx)
{
	if (block_job_post || pending_job != NULL)
		return false;
	pending_job = fn;
	pending_job_ctx = ctx;
	return true;
}

uint8_t satellite_store_count(void)
{
	return record_count;
}

bool satellite_store_get(uint8_t index, satellite_record_t *record)
{
	if (index >= record_count)
		return false;
	*record = records[index];
	return true;
}

bool satellite_store_summary_get(uint8_t index, satellite_record_summary_t *summary)
{
	if (index >= record_count || summary == NULL)
		return false;
	summary->catalog_id = records[index].catalog_id;
	summary->name_length = records[index].name_length;
	memcpy(summary->name, records[index].name, sizeof(summary->name));
	return true;
}

bool satellite_store_profile_selected(uint32_t catalog_id)
{
	return catalog_id == 25544u;
}

bool satellite_store_profile_get(uint32_t catalog_id, satellite_frequency_profile_t *frequency)
{
	if (frequency == NULL || !satellite_store_profile_selected(catalog_id))
		return false;
	for (uint8_t i = 0u; i < satellite_frequency_profile_catalog_count(); i++) {
		if (satellite_frequency_profile_catalog_get(i, frequency) && frequency->catalog_id == catalog_id)
			return true;
	}
	return false;
}

void radio_get_channel_profile(channel_t *channel)
{
	*channel = profile;
}

uint16_t codeplug_zone_count(void)
{
	return zone_count;
}

bool codeplug_zone(uint16_t index, zone_t *zone)
{
	if (index >= zone_count)
		return false;
	*zone = zones[index];
	return true;
}

bool codeplug_channel(uint16_t number, channel_t *channel)
{
	if (number == 0u || number >= sizeof(channel_used) / sizeof(channel_used[0]) || !channel_used[number])
		return false;
	*channel = channels[number];
	return true;
}

bool codeplug_channel_names_each(codeplug_name_fn callback, void *ctx, bool names)
{
	for (uint16_t i = 1u; i < sizeof(channel_used) / sizeof(channel_used[0]); i++) {
		if (channel_used[i] && !callback(i, names ? channels[i].name : "", ctx))
			break;
	}
	return true;
}

uint16_t codeplug_zone_capacity(void)
{
	return CODEPLUG_ZONE_MAX;
}

uint16_t codeplug_free_channel(void)
{
	for (uint16_t i = 1u; i < sizeof(channel_used) / sizeof(channel_used[0]); i++) {
		if (!channel_used[i])
			return i;
	}
	return 0u;
}

zone_edit_result_t zone_edit_new(const char *name, uint16_t *index)
{
	if (zone_count >= sizeof(zones) / sizeof(zones[0]))
		return ZONE_EDIT_ZONES_FULL;
	*index = zone_count;
	memset(&zones[zone_count], 0, sizeof(zones[zone_count]));
	strncpy(zones[zone_count].name, name, CODEPLUG_NAME_LEN);
	zone_count++;
	return ZONE_EDIT_OK;
}

zone_edit_result_t zone_edit_add_new(uint16_t zone, uint16_t pos, uint16_t number, const channel_t *channel)
{
	if (zone >= zone_count || pos > zones[zone].count || number == 0u ||
	    number >= sizeof(channel_used) / sizeof(channel_used[0]) || channel_used[number])
		return ZONE_EDIT_FAILED;
	for (uint16_t i = zones[zone].count; i > pos; i--)
		zones[zone].channels[i] = zones[zone].channels[i - 1u];
	zones[zone].channels[pos] = number;
	zones[zone].count++;
	channels[number] = *channel;
	channel_used[number] = true;
	channel_writes++;
	return ZONE_EDIT_OK;
}

zone_edit_result_t zone_edit_insert(uint16_t zone, uint16_t pos, uint16_t number)
{
	if (zone >= zone_count || pos > zones[zone].count || zones[zone].count >= CODEPLUG_ZONE_MAX)
		return ZONE_EDIT_FAILED;
	for (uint16_t i = zones[zone].count; i > pos; i--)
		zones[zone].channels[i] = zones[zone].channels[i - 1u];
	zones[zone].channels[pos] = number;
	zones[zone].count++;
	return ZONE_EDIT_OK;
}

static void add_record(uint32_t catalog_id, const char *name)
{
	satellite_record_t *record = &records[record_count++];
	memset(record, 0, sizeof(*record));
	record->catalog_id = catalog_id;
	record->name_length = (uint8_t)strlen(name);
	memcpy(record->name, name, record->name_length + 1u);
}

static void reset(void)
{
	memset(records, 0, sizeof(records));
	memset(zones, 0, sizeof(zones));
	memset(channels, 0, sizeof(channels));
	memset(channel_used, 0, sizeof(channel_used));
	zone_count = record_count = 0u;
	channel_writes = 0u;
	pending_job = NULL;
	pending_job_ctx = NULL;
	block_job_post = false;
	memset(&profile, 0, sizeof(profile));
	profile.rx_hz = 145000000u;
	profile.tx_hz = 146000000u;
	profile.mode = RADIO_MODE_DMR;
	profile.contact = 4u;
}

static void test_sync_job_reports_completion_and_failure(void)
{
	reset();
	CHECK(satellite_channels_sync_status() == SATELLITE_CHANNELS_SYNC_IDLE);
	CHECK(satellite_channels_sync_schedule());
	CHECK(satellite_channels_sync_status() == SATELLITE_CHANNELS_SYNC_RUNNING);
	CHECK(pending_job != NULL);
	storage_job_fn job = pending_job;
	void *ctx = pending_job_ctx;
	pending_job = NULL;
	job(ctx);
	CHECK(satellite_channels_sync_status() == SATELLITE_CHANNELS_SYNC_COMPLETE);

	reset();
	add_record(25544u, "ISS");
	zone_count = sizeof(zones) / sizeof(zones[0]);
	CHECK(satellite_channels_sync_schedule());
	job = pending_job;
	ctx = pending_job_ctx;
	pending_job = NULL;
	job(ctx);
	CHECK(satellite_channels_sync_status() == SATELLITE_CHANNELS_SYNC_FAILED);

	reset();
	block_job_post = true;
	CHECK(!satellite_channels_sync_schedule());
	CHECK(satellite_channels_sync_status() == SATELLITE_CHANNELS_SYNC_IDLE);
}

static void test_sync_creates_zone_and_safe_editable_channels(void)
{
	reset();
	satellite_frequency_profile_t frequency_profile;
	CHECK(satellite_frequency_profile(25544u, &frequency_profile));
	CHECK(frequency_profile.rx_hz == 437800000u && frequency_profile.tx_hz == 145990000u &&
	      frequency_profile.tx_tone_tenths == 670u);
	CHECK(!satellite_frequency_profile(20580u, &frequency_profile));
	CHECK(satellite_channels_sync());
	CHECK(zone_count == 0u);
	add_record(25544u, "ISS (ZARYA)");
	add_record(20580u, "HUBBLE SPACE TELESCOPE");
	CHECK(satellite_channels_sync());
	CHECK(zone_count == 1u && strcmp(zones[0].name, "Satellites") == 0);
	CHECK(zones[0].count == 2u && channel_writes == 2u);
	channel_t iss, hubble;
	CHECK(codeplug_channel(zones[0].channels[0], &iss));
	CHECK(codeplug_channel(zones[0].channels[1], &hubble));
	CHECK(strcmp(iss.name, "ISS (ZARYA)") == 0);
	CHECK(strcmp(hubble.name, "HUBBLE SPACE TEL") == 0);
	CHECK(iss.rx_hz == 437800000u && iss.tx_hz == 145990000u);
	CHECK(iss.tx_tone.type == TONE_CTCSS && iss.tx_tone.value == 670u);
	CHECK(hubble.rx_hz == profile.rx_hz && hubble.tx_hz == profile.tx_hz);
	CHECK(iss.mode == RADIO_MODE_FM && !iss.rx_only && iss.contact == 0u);
}

static void test_repeat_sync_preserves_channel_edits_and_adds_new_records(void)
{
	reset();
	add_record(25544u, "ISS");
	CHECK(satellite_channels_sync());
	uint16_t number = zones[0].channels[0];
	channels[number].rx_hz = 437800000u;
	channels[number].tx_hz = 145990000u;
	channels[number].tx_tone = (subtone_t){ 1u, 670u };
	CHECK(satellite_channels_sync());
	CHECK(channel_writes == 1u && zones[0].count == 1u);
	CHECK(channels[number].rx_hz == 437800000u && channels[number].tx_hz == 145990000u);
	CHECK(channels[number].tx_tone.value == 670u);
	add_record(20580u, "HUBBLE");
	CHECK(satellite_channels_sync());
	CHECK(zones[0].count == 2u && channel_writes == 2u);
}

static void test_sync_reuses_existing_channel(void)
{
	reset();
	add_record(25544u, "ISS");
	strcpy(channels[4].name, "ISS");
	channels[4].rx_hz = 437800000u;
	channels[4].tx_hz = 145990000u;
	channel_used[4] = true;
	CHECK(satellite_channels_sync());
	CHECK(zone_count == 1u && zones[0].count == 1u && zones[0].channels[0] == 4u);
	CHECK(channel_writes == 0u);
}

int main(void)
{
	test_sync_creates_zone_and_safe_editable_channels();
	test_repeat_sync_preserves_channel_edits_and_adds_new_records();
	test_sync_reuses_existing_channel();
	test_sync_job_reports_completion_and_failure();
	printf("satelliteChannelsTest: %u checks, %u failures\n", checks, failures);
	return failures != 0u;
}