// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Installed satellite TLE records, validated and committed by the dedicated flash store.
#ifndef SATELLITE_STORE_H
#define SATELLITE_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "satellite/frequency.h"

#define SATELLITE_NAME_MAX 48u
#define SATELLITE_TLE_LINE_LENGTH 69u
#define SATELLITE_STORE_MAX_RECORDS 10u
#define SATELLITE_PROFILE_MAX 10u
#define SATELLITE_PROFILE_RX_MIN_HZ 10000000u
#define SATELLITE_PROFILE_RX_MAX_HZ 1000000000u
#define SATELLITE_PROFILE_TX_MIN_HZ 10000000u
#define SATELLITE_PROFILE_TX_MAX_HZ 1000000000u
#define SATELLITE_PROFILE_TONE_MAX_TENTHS 2600u
#define SATELLITE_RECORD_NEEDS_ARM 0x01u

typedef struct {
	char line1[SATELLITE_TLE_LINE_LENGTH + 1u];
	char line2[SATELLITE_TLE_LINE_LENGTH + 1u];
} satellite_tle_lines_t;

typedef struct {
	uint32_t catalog_id;
	uint32_t epoch;
	uint8_t name_length;
	uint8_t flags;
	char name[SATELLITE_NAME_MAX + 1u];
	char line1[SATELLITE_TLE_LINE_LENGTH + 1u];
	char line2[SATELLITE_TLE_LINE_LENGTH + 1u];
} satellite_record_t;

typedef struct {
	uint32_t catalog_id;
	uint32_t epoch;
	uint8_t name_length;
	uint8_t flags;
	char name[SATELLITE_NAME_MAX + 1u];
} satellite_record_summary_t;

#define SATELLITE_RECORD_BRIEF_NAME_MAX 16u
typedef struct {
	uint32_t catalog_id;
	uint32_t epoch;
	uint8_t name_length;
	uint8_t flags;
	char name[SATELLITE_RECORD_BRIEF_NAME_MAX + 1u];
} satellite_record_brief_t;

typedef struct {
	uint32_t catalog_id;
	uint32_t epoch;
} satellite_record_id_t;

typedef enum {
	SATELLITE_STORE_TRANSFER_OK,
	SATELLITE_STORE_TRANSFER_BAD_ARG,
	SATELLITE_STORE_TRANSFER_REFUSED,
	SATELLITE_STORE_TRANSFER_BUSY,
} satellite_store_transfer_status_t;

// Read one validated installed record in catalog-ID order. False for an invalid index, unavailable store or read error.
bool satellite_store_get(uint8_t index, satellite_record_t *record);
// Read the catalog ID and name without copying the TLE lines.
bool satellite_store_summary_get(uint8_t index, satellite_record_summary_t *summary);
uint8_t satellite_store_count(void);
bool satellite_store_list(uint16_t start, uint8_t limit, satellite_record_summary_t *records, uint8_t *count);
bool satellite_store_list_ids(uint16_t start, uint8_t limit, satellite_record_id_t *records, uint8_t *count);
uint32_t satellite_store_generation(void);
// Replace the selected record's TLE without changing its catalog ID or name. The validated inventory commits atomically.
bool satellite_store_replace(uint8_t index, const char *line1, const char *line2);
bool satellite_store_profiles_get(uint8_t *count, uint32_t ids[SATELLITE_PROFILE_MAX]);
bool satellite_store_profiles_set(uint8_t count, const uint32_t ids[SATELLITE_PROFILE_MAX]);
bool satellite_store_frequency_profiles_get(uint8_t *count,
					    satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX]);
bool satellite_store_frequency_profiles_set(uint8_t count,
					    const satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX]);
bool satellite_store_profile_get(uint32_t catalog_id, satellite_frequency_profile_t *profile);
bool satellite_store_profile_selected(uint32_t catalog_id);

// Staged update operations used by the debug-link protocol. Each call is safe to invoke without holding storage_lock().
bool satellite_store_available(void);
satellite_store_transfer_status_t satellite_store_transfer_begin(uint16_t count, uint16_t bytes, uint32_t crc,
								 bool replace, uint16_t *id);
satellite_store_transfer_status_t satellite_store_transfer_data(uint16_t id, uint16_t offset,
								const uint8_t *data, uint16_t length,
								uint16_t *next_offset);
satellite_store_transfer_status_t satellite_store_transfer_profiles(uint16_t id, uint8_t count,
								    const satellite_frequency_profile_t *profiles);
satellite_store_transfer_status_t satellite_store_transfer_profiles_chunk(
	uint16_t id, uint8_t total, uint8_t start, uint8_t count, const satellite_frequency_profile_t *profiles);
satellite_store_transfer_status_t satellite_store_transfer_commit(uint16_t id, uint32_t crc);
satellite_store_transfer_status_t satellite_store_transfer_abort(uint16_t id);

#endif