// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pure Doppler targets derived from configured nominal frequencies.
#include <stddef.h>
#include "satellite/doppler.h"

#define SPEED_OF_LIGHT_CM_S UINT64_C(29979245800)

static uint32_t multiply_divide_u32(uint32_t value, uint32_t multiplier, uint64_t divisor,
				    uint64_t *remainder)
{
	uint64_t rem = 0u;
	uint32_t quotient = 0u;
	for (int8_t bit = 31; bit >= 0; bit--) {
		rem <<= 1;
		if ((value & (UINT32_C(1) << bit)) != 0u)
			rem += multiplier;
		uint32_t quotient_digit = 0u;
		while (rem >= divisor) {
			rem -= divisor;
			quotient_digit++;
		}
		quotient = quotient * 2u + quotient_digit;
	}
	if (remainder != NULL)
		*remainder = (uint32_t)rem;
	return quotient;
}

static bool corrected_frequency(uint32_t nominal_hz, int32_t range_rate_cm_s, bool transmit,
			       uint32_t tuning_step_hz, uint32_t *target_hz)
{
	if (nominal_hz == 0u) {
		*target_hz = 0u;
		return true;
	}
	uint32_t speed = range_rate_cm_s < 0 ?
		(uint32_t)(-(int64_t)range_rate_cm_s) : (uint32_t)range_rate_cm_s;
	uint64_t remainder;
	uint32_t offset = multiply_divide_u32(nominal_hz, speed, SPEED_OF_LIGHT_CM_S, &remainder);
	if (remainder * 2u >= SPEED_OF_LIGHT_CM_S)
		offset++;
	bool increase = (range_rate_cm_s < 0) != transmit;
	if (increase) {
		if (offset > UINT32_MAX - nominal_hz)
			return false;
		nominal_hz += offset;
	} else {
		if (offset >= nominal_hz)
			return false;
		nominal_hz -= offset;
	}

	uint32_t units = nominal_hz / tuning_step_hz;
	uint32_t remainder_hz = nominal_hz % tuning_step_hz;
	if ((uint64_t)remainder_hz * 2u >= tuning_step_hz)
		units++;
	if (units > UINT32_MAX / tuning_step_hz)
		return false;
	*target_hz = units * tuning_step_hz;
	return *target_hz != 0u;
}

bool satellite_doppler_calculate(uint32_t nominal_rx_hz, uint32_t nominal_tx_hz,
				 int32_t range_rate_cm_s, uint32_t tuning_step_hz,
				 satellite_doppler_targets_t *targets)
{
	if (targets == NULL || tuning_step_hz == 0u ||
	    (uint64_t)(range_rate_cm_s < 0 ? -(int64_t)range_rate_cm_s : range_rate_cm_s) >=
		    SPEED_OF_LIGHT_CM_S)
		return false;
	uint32_t rx, tx;
	if (!corrected_frequency(nominal_rx_hz, range_rate_cm_s, false, tuning_step_hz, &rx) ||
	    !corrected_frequency(nominal_tx_hz, range_rate_cm_s, true, tuning_step_hz, &tx))
		return false;
	targets->rx_hz = rx;
	targets->tx_hz = tx;
	targets->rx_offset_hz = nominal_rx_hz == 0u ? 0 : (int64_t)rx - nominal_rx_hz;
	targets->tx_offset_hz = nominal_tx_hz == 0u ? 0 : (int64_t)tx - nominal_tx_hz;
	return true;
}
