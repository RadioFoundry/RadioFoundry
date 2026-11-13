// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Installed satellite TLE records, validated and committed by the dedicated flash store.
#ifndef SATELLITE_STORE_H
#define SATELLITE_STORE_H

#include <stdbool.h>
#include <stdint.h>

#define SATELLITE_NAME_MAX 48u
#define SATELLITE_TLE_LINE_LENGTH 69u
#define SATELLITE_STORE_MAX_RECORDS 10u

typedef struct {
	uint32_t catalog_id;
	uint32_t epoch;
	uint8_t name_length;
	char name[SATELLITE_NAME_MAX + 1u];
	char line1[SATELLITE_TLE_LINE_LENGTH + 1u];
	char line2[SATELLITE_TLE_LINE_LENGTH + 1u];
} satellite_record_t;

// Read one validated installed record in catalog-ID order. False for an invalid index, unavailable store or read error.
bool satellite_store_get(uint8_t index, satellite_record_t *record);
uint8_t satellite_store_count(void);
// Seed the built-in TLEs only when all inventory banks are erased; existing or interrupted inventories are preserved.
bool satellite_store_seed_defaults(void);
// Replace the selected record's TLE without changing its catalog ID or name. The validated inventory commits atomically.
bool satellite_store_replace(uint8_t index, const char *line1, const char *line2);

#endif