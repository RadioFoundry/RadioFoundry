// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pure Doppler targets derived from configured nominal frequencies.
#ifndef SATELLITE_DOPPLER_H
#define SATELLITE_DOPPLER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t rx_hz;
	uint32_t tx_hz;
	int64_t rx_offset_hz;
	int64_t tx_offset_hz;
} satellite_doppler_targets_t;

bool satellite_doppler_calculate(uint32_t nominal_rx_hz, uint32_t nominal_tx_hz, int32_t range_rate_cm_s,
				 uint32_t tuning_step_hz, satellite_doppler_targets_t *targets);

#endif