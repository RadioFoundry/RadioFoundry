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
	// 32-bit only: the ramp runs a step every few microseconds, and a 64-bit divide is a library call that costs
	// more than the step on a CPU without one. x in Q15; every product stays below 2^32
	uint32_t x = ((uint32_t)step << 15) / steps;
	uint32_t s = (x * x) >> 15;
	s = (s * (3u * 32768u - 2u * x)) >> 15;
	return (uint16_t)(((uint32_t)target * s + 16384u) >> 15);
}

#endif
