// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DWT cycle counter the clocks run on (platform/*/time.c). A debugger that clears TRCENA or CYCCNTENA stops it,
// which would freeze hal_millis and spin every wait on it forever. A stopped counter reads the same value twice; a
// running one never does between two clock reads (a read at exactly one wrap later only restarts a running counter,
// which is harmless). So the equal read is the one test, and a running clock pays a compare, not two register reads.
// Include after the platform's CMSIS header.
#ifndef CORTEXM_DWT_CLOCK_H
#define CORTEXM_DWT_CLOCK_H

#include <stdint.h>

// The counter now, restarted if it stopped; prev is what an earlier read returned
static inline __attribute__((always_inline)) uint32_t dwt_cycles(uint32_t prev)
{
	uint32_t now = DWT->CYCCNT;
	if (now == prev) {
		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	}
	return now;
}

#endif
