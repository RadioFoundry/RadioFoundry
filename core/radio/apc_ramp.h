// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The PA control ramp at key-up and key-down: the drive level at step 0..steps of a ramp to target, a smoothstep
// 3x^2 - 2x^3 of x = step / steps, so the level has no jump at either end. Ramping down runs the steps backwards.
// The boards that drive a PA control DAC step through it with their own step count and time
#ifndef RADIO_APC_RAMP_H
#define RADIO_APC_RAMP_H
#include <stdint.h>

static inline uint16_t apc_ramp_level(uint16_t target, uint16_t step, uint16_t steps)
{
	if (step >= steps) {
		return target;
	}
	uint64_t n = steps;
	uint64_t x = step;
	return (uint16_t)((uint64_t)target * x * x * (3u * n - 2u * x) / (n * n * n));
}

#endif
