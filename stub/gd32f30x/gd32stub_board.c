// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GD32F30x stub's chip facts and its board record check (gd32stub.h). Portable: the host tests run it too.
#include "gd32stub.h"

const stubeng_run_t gd32_sector_runs[1] = {
	{ GD32_SECTOR_SIZE, 256 },
};

// The flash and SRAM size words and the unique ID; the option bytes; SRAM up to the largest part's 96 KB
const stubeng_range_t gd32_peek_ranges[3] = {
	{ 0x1FFFF7E0u, 0x1FFFF7F4u },
	{ 0x1FFFF800u, 0x1FFFF810u },
	{ 0x20000000u, 0x20018000u },
};

stubeng_flash_t gd32_flash(uint16_t flash_kb)
{
	uint32_t size = (uint32_t)flash_kb * 1024u;
	stubeng_flash_t f = {
		.base = GD32_FLASH_START,
		.size = size < GD32_STUB_FLASH_MAX ? size : GD32_STUB_FLASH_MAX,
		.runs = gd32_sector_runs,
		.nruns = 1,
		.prog_unit = 4,		// word programming
		.min_app_base = GD32_MIN_APP_BASE,
	};
	return f;
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool gd32_board_parse(const uint8_t *rec, uint32_t n, gd32_board_t *b)
{
	if (n != GD32_REC_LEN || rec[GD32_REC_BOARD] == 0 || rec[GD32_REC_FORMAT] != GD32_REC_LAYOUT ||
	    rec[GD32_REC_ZERO] != 0 || rec[GD32_REC_ZERO + 1] != 0) {
		return false;
	}
	b->board = rec[GD32_REC_BOARD];
	b->app_base = get32(rec + GD32_REC_APP_BASE);
	return true;
}
