// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The memory the NXDN and YSF engines run in (radio_air.h)
#include "config.h"
#if CONFIG_NXDN || CONFIG_YSF	// the whole file
#include "board.h"
#include "radio/radio_air.h"
#if CONFIG_P25 && !defined(HAL_VOCODER_LEND_BYTES)
#include "radio/radio_p25.h"

// P25's memory is idle while another air mode runs: no block of our own
void *radio_air_engine_mem(size_t *len)
{
	return radio_p25_idle_mem(len);
}
#else
#if CONFIG_NXDN
#include "nxdn/nxdn_engine.h"
#endif
#if CONFIG_YSF
#include "ysf/ysf_engine.h"
#endif

// One block for whichever of them runs, 8-aligned as their users check (a uint64_t member is 4-aligned on some
// cores: C-SKY's ABI, where the engines then refused the block)
typedef union {
#if CONFIG_NXDN
	nxdn_engine_t nxdn;
#endif
#if CONFIG_YSF
	ysf_engine_t ysf;
#endif
	uint64_t u64;
} engine_mem_t;
static engine_mem_t mem __attribute__((aligned(8)));
_Static_assert(_Alignof(engine_mem_t) <= 8, "the block is aligned to 8 below its members' needs");

void *radio_air_engine_mem(size_t *len)
{
	*len = sizeof mem;
	return &mem;
}
#endif
#endif	// CONFIG_NXDN || CONFIG_YSF
