// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Satellite inventory command protocol. Flash persistence lives in core/satellite/store.c.
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "satellite/channels.h"
#include "satellite/store.h"

#if CONFIG_SATELLITE_TLE
#define SAT_MAX_RECORDS SATELLITE_STORE_MAX_RECORDS
#define SAT_PROFILE_CHUNK_MAX 4u
#define SAT_PROTOCOL_VERSION 7u

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}
#endif

static size_t status(uint8_t *out, uint8_t value)
{
	out[0] = value;
	return 1u;
}

#if CONFIG_SATELLITE_TLE
static uint8_t transfer_status(satellite_store_transfer_status_t result)
{
	switch (result) {
	case SATELLITE_STORE_TRANSFER_OK:
		return DL_ST_OK;
	case SATELLITE_STORE_TRANSFER_BAD_ARG:
		return DL_ST_BAD_ARG;
	case SATELLITE_STORE_TRANSFER_BUSY:
		return DL_ST_BUSY;
	case SATELLITE_STORE_TRANSFER_REFUSED:
	default:
		return DL_ST_REFUSED;
	}
}

static void read_profile(const uint8_t *p, satellite_frequency_profile_t *profile)
{
	profile->catalog_id = get32(p);
	profile->rx_hz = get32(p + 4u);
	profile->tx_hz = get32(p + 8u);
	profile->tx_tone_tenths = get16(p + 12u);
	profile->name = NULL;
}

static __attribute__((noinline)) size_t satellite_list(const uint8_t *req, uint16_t len, uint8_t *out,
						       size_t cap)
{
	if (len != 3u || cap < 2u)
		return status(out, DL_ST_BAD_ARG);
	satellite_record_id_t list_ids[SAT_MAX_RECORDS];
	uint8_t count;
	if (!satellite_store_list_ids(get16(req + 1u), SAT_MAX_RECORDS, list_ids, &count) ||
	    cap < 2u + (size_t)count * 8u)
		return status(out, DL_ST_REFUSED);
	out[0] = DL_ST_OK;
	out[1] = count;
	for (uint8_t i = 0u; i < count; i++) {
		put32(out + 2u + (size_t)i * 8u, list_ids[i].catalog_id);
		put32(out + 6u + (size_t)i * 8u, list_ids[i].epoch);
	}
	return 2u + (size_t)count * 8u;
}

static __attribute__((noinline)) size_t satellite_profile_stage(const uint8_t *req, uint16_t len,
								uint8_t *out)
{
	if (len < 4u || req[3] > SATELLITE_PROFILE_MAX ||
	    len != 4u + (uint16_t)req[3] * 14u)
		return status(out, DL_ST_BAD_ARG);
	satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX] = { 0 };
	for (uint8_t i = 0u; i < req[3]; i++)
		read_profile(req + 4u + (uint16_t)i * 14u, &profiles[i]);
	return status(out, transfer_status(satellite_store_transfer_profiles(
		get16(req + 1u), req[3], profiles)));
}

static __attribute__((noinline)) size_t satellite_profile_values_get(uint8_t *out, size_t cap)
{
	uint8_t count;
	satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX];
	if (!satellite_store_frequency_profiles_get(&count, profiles))
		return status(out, DL_ST_REFUSED);
	if (cap < 2u + (size_t)count * 14u)
		return status(out, DL_ST_BAD_ARG);
	out[0] = DL_ST_OK;
	out[1] = count;
	for (uint8_t i = 0u; i < count; i++) {
		uint8_t *p = out + 2u + (uint16_t)i * 14u;
		put32(p, profiles[i].catalog_id);
		put32(p + 4u, profiles[i].rx_hz);
		put32(p + 8u, profiles[i].tx_hz);
		put16(p + 12u, profiles[i].tx_tone_tenths);
	}
	return 2u + (size_t)count * 14u;
}
#endif

size_t debuglink_cmd_satellite(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (cap == 0u)
		return 0u;
#if CONFIG_SATELLITE_TLE
	if (len == 0u)
		return status(out, DL_ST_BAD_ARG);
	if (!satellite_store_available())
		return status(out, DL_ST_UNAVAILABLE);
	switch (req[0]) {
	case DL_SAT_OP_INFO: {
		if (len != 1u || cap < 6u)
			return status(out, DL_ST_BAD_ARG);
		out[0] = DL_ST_OK;
		out[1] = SAT_PROTOCOL_VERSION;
		put16(out + 2u, SAT_MAX_RECORDS);
		put16(out + 4u, satellite_store_count());
		return 6u;
	}
	case DL_SAT_OP_LIST:
		return satellite_list(req, len, out, cap);
	case DL_SAT_OP_BEGIN:
	case DL_SAT_OP_REPLACE_BEGIN: {
		if (len != 9u || cap < 3u)
			return status(out, DL_ST_BAD_ARG);
		uint16_t id;
		satellite_store_transfer_status_t result =
			satellite_store_transfer_begin(get16(req + 1u), get16(req + 3u), get32(req + 5u),
						       req[0] == DL_SAT_OP_REPLACE_BEGIN, &id);
		if (result != SATELLITE_STORE_TRANSFER_OK)
			return status(out, transfer_status(result));
		out[0] = DL_ST_OK;
		put16(out + 1u, id);
		return 3u;
	}
	case DL_SAT_OP_DATA: {
		if (len < 6u || len > 64u || cap < 3u)
			return status(out, DL_ST_BAD_ARG);
		uint16_t next_offset;
		satellite_store_transfer_status_t result =
			satellite_store_transfer_data(get16(req + 1u), get16(req + 3u), req + 5u,
						      (uint16_t)(len - 5u), &next_offset);
		if (result != SATELLITE_STORE_TRANSFER_OK)
			return status(out, transfer_status(result));
		out[0] = DL_ST_OK;
		put16(out + 1u, next_offset);
		return 3u;
	}
	case DL_SAT_OP_PROFILE_STAGE:
		return satellite_profile_stage(req, len, out);
	case DL_SAT_OP_PROFILE_STAGE_CHUNK: {
		if (len < 20u || req[3] == 0u || req[3] > SATELLITE_PROFILE_MAX ||
		    req[5] == 0u || req[5] > SAT_PROFILE_CHUNK_MAX || len != 6u + (uint16_t)req[5] * 14u ||
		    req[4] > req[3] || req[5] > req[3] - req[4])
			return status(out, DL_ST_BAD_ARG);
		satellite_frequency_profile_t profiles[SAT_PROFILE_CHUNK_MAX] = { 0 };
		for (uint8_t i = 0u; i < req[5]; i++)
			read_profile(req + 6u + (uint16_t)i * 14u, &profiles[i]);
		return status(out, transfer_status(satellite_store_transfer_profiles_chunk(
			get16(req + 1u), req[3], req[4], req[5], profiles)));
	}
	case DL_SAT_OP_COMMIT:
		if (len != 7u)
			return status(out, DL_ST_BAD_ARG);
		return status(out, transfer_status(satellite_store_transfer_commit(get16(req + 1u), get32(req + 3u))));
	case DL_SAT_OP_ABORT:
		if (len != 3u)
			return status(out, DL_ST_BAD_ARG);
		return status(out, transfer_status(satellite_store_transfer_abort(get16(req + 1u))));
	case DL_SAT_OP_SYNC_CHANNELS:
		if (len != 1u)
			return status(out, DL_ST_BAD_ARG);
		return status(out, satellite_channels_sync_schedule() ? DL_ST_OK : DL_ST_BUSY);
	case DL_SAT_OP_PROFILE_GET: {
		if (len != 1u || cap < 2u + SATELLITE_PROFILE_MAX * 4u)
			return status(out, DL_ST_BAD_ARG);
		uint8_t count;
		uint32_t ids[SATELLITE_PROFILE_MAX];
		if (!satellite_store_profiles_get(&count, ids))
			return status(out, DL_ST_REFUSED);
		out[0] = DL_ST_OK;
		out[1] = count;
		for (uint8_t i = 0u; i < count; i++)
			put32(out + 2u + (uint16_t)i * 4u, ids[i]);
		return 2u + (size_t)count * 4u;
	}
	case DL_SAT_OP_PROFILE_SET: {
		if (len < 2u || req[1] > SATELLITE_PROFILE_MAX ||
		    len != 2u + (uint16_t)req[1] * 4u)
			return status(out, DL_ST_BAD_ARG);
		uint32_t ids[SATELLITE_PROFILE_MAX] = { 0 };
		for (uint8_t i = 0u; i < req[1]; i++)
			ids[i] = get32(req + 2u + (uint16_t)i * 4u);
		return status(out, satellite_store_profiles_set(req[1], ids) ? DL_ST_OK : DL_ST_REFUSED);
	}
	case DL_SAT_OP_PROFILE_VALUES_GET:
		return satellite_profile_values_get(out, cap);
	case DL_SAT_OP_SYNC_CHANNELS_STATUS:
		if (len != 1u || cap < 2u)
			return status(out, DL_ST_BAD_ARG);
		out[0] = DL_ST_OK;
		out[1] = (uint8_t)satellite_channels_sync_status();
		return 2u;
	default:
		return status(out, DL_ST_BAD_ARG);
	}
#else
	(void)req;
	(void)len;
	return status(out, DL_ST_UNAVAILABLE);
#endif
}
