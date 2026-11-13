// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pure Doppler targets derived from configured nominal frequencies.
#include <math.h>
#include <stddef.h>
#include "satellite/doppler.h"

#define SPEED_OF_LIGHT_M_S 299792458.0

static bool target_frequency(uint32_t nominal_hz, double factor, uint32_t step_hz, uint32_t *target_hz)
{
	if (nominal_hz == 0u) {
		*target_hz = 0u;
		return true;
	}
	double corrected = (double)nominal_hz * factor;
	if (!isfinite(corrected) || corrected <= 0.0 || corrected > (double)UINT32_MAX)
		return false;
	double units = corrected / (double)step_hz;
	double rounded = floor(units + 0.5) * (double)step_hz;
	if (rounded <= 0.0 || rounded > (double)UINT32_MAX)
		return false;
	*target_hz = (uint32_t)rounded;
	return true;
}

bool satellite_doppler_calculate(uint32_t nominal_rx_hz, uint32_t nominal_tx_hz, double range_rate_m_s,
				 uint32_t tuning_step_hz, satellite_doppler_targets_t *targets)
{
	if (targets == NULL || tuning_step_hz == 0u || !isfinite(range_rate_m_s) ||
	    fabs(range_rate_m_s) >= SPEED_OF_LIGHT_M_S)
		return false;
	double ratio = range_rate_m_s / SPEED_OF_LIGHT_M_S;
	uint32_t rx, tx;
	if (!target_frequency(nominal_rx_hz, 1.0 - ratio, tuning_step_hz, &rx) ||
	    !target_frequency(nominal_tx_hz, 1.0 + ratio, tuning_step_hz, &tx))
		return false;
	targets->rx_hz = rx;
	targets->tx_hz = tx;
	targets->rx_offset_hz = nominal_rx_hz == 0u ? 0 : (int64_t)rx - nominal_rx_hz;
	targets->tx_offset_hz = nominal_tx_hz == 0u ? 0 : (int64_t)tx - nominal_tx_hz;
	return true;
}