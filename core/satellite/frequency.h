// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef SATELLITE_FREQUENCY_H
#define SATELLITE_FREQUENCY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t catalog_id;
	uint32_t rx_hz;
	uint32_t tx_hz;
	uint16_t tx_tone_tenths;
} satellite_frequency_profile_t;

bool satellite_frequency_profile(uint32_t catalog_id, satellite_frequency_profile_t *profile);

#endif