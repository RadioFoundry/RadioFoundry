// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT32F43x stub's chip facts and its board record check (at32stub.h). Portable: the host tests run it too.
#include "at32stub.h"

const stubeng_run_t at32_sector_runs[1] = {
	{ AT32_SECTOR_SIZE, 256 },
};

// The flash size word and the unique ID; the user system data (option bytes); the 128 KB of SRAM every EOPB0 setting
// leaves (above that the SRAM may be the flash's zero-wait area, and a read there could fault the stub)
const stubeng_range_t at32_peek_ranges[3] = {
	{ 0x1FFFF7E0u, 0x1FFFF7F4u },
	{ 0x1FFFC000u, 0x1FFFC040u },
	{ 0x20000000u, 0x20020000u },
};

stubeng_flash_t at32_flash(uint16_t flash_kb)
{
	uint32_t size = (uint32_t)flash_kb * 1024u;
	stubeng_flash_t f = {
		.base = AT32_FLASH_START,
		.size = size < AT32_STUB_FLASH_MAX ? size : AT32_STUB_FLASH_MAX,
		.runs = at32_sector_runs,
		.nruns = 1,
		.prog_unit = 4,		// word programming
		.min_app_base = AT32_MIN_APP_BASE,
	};
	return f;
}

uint32_t at32_bank2_start(uint16_t flash_kb)
{
	if (flash_kb > 1024u) {
		return AT32_BANK2_LARGE;
	}
	return flash_kb == 1024u ? AT32_BANK2_1MB : 0;
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool at32_board_parse(const uint8_t *rec, uint32_t n, at32_board_t *b)
{
	if (n != AT32_REC_LEN || rec[AT32_REC_BOARD] == 0 || rec[AT32_REC_FORMAT] != AT32_REC_LAYOUT ||
	    rec[AT32_REC_ZERO] != 0 || rec[AT32_REC_ZERO + 1] != 0) {
		return false;
	}
	b->board = rec[AT32_REC_BOARD];
	b->app_base = get32(rec + AT32_REC_APP_BASE);
	return true;
}
