// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef SATELLITE_FREQUENCY_H
#define SATELLITE_FREQUENCY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t catalog_id;
	const char *name;
	uint32_t rx_hz;
	uint32_t tx_hz;
	uint16_t tx_tone_tenths;
} satellite_frequency_profile_t;

uint8_t satellite_frequency_profile_catalog_count(void);
bool satellite_frequency_profile_catalog_get(uint8_t index, satellite_frequency_profile_t *profile);
bool satellite_frequency_profile(uint32_t catalog_id, satellite_frequency_profile_t *profile);
bool satellite_frequency_needs_arm(uint32_t catalog_id);

#endif