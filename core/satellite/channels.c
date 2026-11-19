// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/zone_edit.h"
#include "radio/radio.h"
#include "satellite/channels.h"
#include "satellite/frequency.h"
#include "satellite/store.h"
#include "storage/storage.h"

#define SATELLITE_ZONE_NAME "Satellites"

static void apply_fm_profile(uint32_t catalog_id, channel_t *channel)
{
	satellite_frequency_profile_t profile;
	if (!satellite_frequency_profile(catalog_id, &profile))
		return;
	channel->rx_hz = profile.rx_hz;
	channel->tx_hz = profile.tx_hz;
	if (profile.tx_tone_tenths != 0u)
		channel->tx_tone = (subtone_t){ TONE_CTCSS, profile.tx_tone_tenths };
}

static uint8_t channel_name(const satellite_record_summary_t *record, char out[CODEPLUG_NAME_LEN + 1])
{
	uint8_t length = record->name_length;
	if (length > CODEPLUG_NAME_LEN)
		length = CODEPLUG_NAME_LEN;
	while (length > 0u && ((uint8_t)record->name[length] & 0xC0u) == 0x80u)
		length--;
	memcpy(out, record->name, length);
	out[length] = '\0';
	return length;
}

static int satellite_zone(void)
{
	zone_t zone;
	for (uint16_t i = 0; i < codeplug_zone_count(); i++) {
		if (codeplug_zone(i, &zone) && strcmp(zone.name, SATELLITE_ZONE_NAME) == 0)
			return i;
	}
	return -1;
}

static bool has_channel(const zone_t *zone, const char *name)
{
	channel_t channel;
	for (uint16_t i = 0; i < zone->count; i++) {
		if (codeplug_channel(zone->channels[i], &channel) && strcmp(channel.name, name) == 0)
			return true;
	}
	return false;
}

typedef struct {
	const char *name;
	uint16_t number;
} channel_lookup_t;

static bool find_channel(uint16_t number, const char *name, void *ctx)
{
	channel_lookup_t *lookup = ctx;
	if (name != NULL && strcmp(name, lookup->name) == 0) {
		lookup->number = number;
		return false;
	}
	return true;
}

bool satellite_channels_sync(void)
{
	uint8_t count = satellite_store_count();
	if (count == 0u)
		return true;
	int index = satellite_zone();
	if (index < 0) {
		uint16_t created;
		if (zone_edit_new(SATELLITE_ZONE_NAME, &created) != ZONE_EDIT_OK)
			return false;
		index = created;
	}
	zone_t zone;
	if (!codeplug_zone((uint16_t)index, &zone))
		return false;
	for (uint8_t i = 0; i < count; i++) {
		satellite_record_summary_t record;
		char name[CODEPLUG_NAME_LEN + 1];
		if (!satellite_store_summary_get(i, &record) || channel_name(&record, name) == 0u)
			return false;
		if (has_channel(&zone, name))
			continue;
		channel_lookup_t lookup = { .name = name };
		if (!codeplug_channel_names_each(find_channel, &lookup, true))
			return false;
		if (lookup.number != 0u) {
			if (zone_edit_insert((uint16_t)index, zone.count, lookup.number) != ZONE_EDIT_OK ||
			    !codeplug_zone((uint16_t)index, &zone))
				return false;
			continue;
		}
		channel_t channel;
		radio_get_channel_profile(&channel);
		if (channel.rx_hz == 0u || channel.tx_hz == 0u || channel.rx_hz >= 1000000000u ||
		    channel.tx_hz >= 1000000000u || zone.count >= codeplug_zone_capacity())
			return false;
		strcpy(channel.name, name);
		channel.mode = RADIO_MODE_FM;
		channel.bw = RADIO_BW_12K5;
		channel.rx_tone = (subtone_t){ 0 };
		channel.tx_tone = (subtone_t){ 0 };
		apply_fm_profile(record.catalog_id, &channel);
		channel.rx_only = false;
		channel.contact = 0u;
		channel.listen_list = 0u;
		channel.dmr_id = 0u;
		channel.alias_tx = 0u;
		uint16_t number = codeplug_free_channel();
		if (number == 0u || zone_edit_add_new((uint16_t)index, zone.count, number, &channel) != ZONE_EDIT_OK)
			return false;
		if (!codeplug_zone((uint16_t)index, &zone))
			return false;
	}
	return true;
}

static volatile uint8_t sync_status;

static void satellite_channels_sync_job(void *ctx)
{
	(void)ctx;
	sync_status = satellite_channels_sync() ? SATELLITE_CHANNELS_SYNC_COMPLETE :
		SATELLITE_CHANNELS_SYNC_FAILED;
}

bool satellite_channels_sync_schedule(void)
{
	if (sync_status == SATELLITE_CHANNELS_SYNC_RUNNING)
		return false;
	sync_status = SATELLITE_CHANNELS_SYNC_RUNNING;
	if (!storage_job_post(satellite_channels_sync_job, NULL)) {
		sync_status = SATELLITE_CHANNELS_SYNC_IDLE;
		return false;
	}
	return true;
}

satellite_channels_sync_status_t satellite_channels_sync_status(void)
{
	return (satellite_channels_sync_status_t)sync_status;
}
