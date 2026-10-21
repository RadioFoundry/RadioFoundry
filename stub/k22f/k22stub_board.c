// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The K22F stub's chip facts and its board record check (k22stub.h). Portable: the host tests run it too.
#include "k22stub.h"

// 256 sectors of 2 KB reach 512 KB; the engine cuts the map at the part's size
const stubeng_run_t k22_sector_runs[1] = {
	{ K22_SECTOR, 256 },
};

// SRAM_L and SRAM_U (one span: the engine reads byte by byte, so a read may cross 0x20000000); the SIM from SDID to
// the unique ID (identity, flash size, clock gates and dividers); the FTFA's status, security, option and protection
// registers. Reading any of these has no side effect
const stubeng_range_t k22_peek_ranges[3] = {
	{ 0x1FFF0000u, 0x20010000u },
	{ 0x40048024u, 0x40048064u },
	{ 0x40020000u, 0x40020014u },
};

stubeng_flash_t k22_flash(uint16_t flash_kb)
{
	stubeng_flash_t f = {
		.base = K22_FLASH_BASE,
		.size = (uint32_t)flash_kb * 1024u,
		.runs = k22_sector_runs,
		.nruns = 1,
		.prog_unit = K22_PROG_UNIT,
		.min_app_base = K22_MIN_APP_BASE,
	};
	return f;
}

uint16_t k22_flash_kb(uint32_t pfsize)
{
	switch (pfsize) {
	case 0x7u: return 128u;
	case 0x9u: return 256u;
	case 0xBu: return 512u;
	default: return 0u;
	}
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Ports A to E, pins 0 to 31
static bool pin_ok(uint8_t pin)
{
	return pin < 5u * 32u;
}

bool k22_board_parse(const uint8_t *rec, uint32_t n, k22_board_t *b)
{
	if (n != K22_REC_LEN || rec[K22_REC_BOARD] == 0 || rec[K22_REC_FORMAT] != K22_REC_LAYOUT) {
		return false;
	}
	for (uint32_t i = K22_REC_ZERO; i < K22_REC_LEN; i++) {
		if (rec[i] != 0) {
			return false;
		}
	}
	b->board = rec[K22_REC_BOARD];
	b->xflash = rec[K22_REC_XFLASH];
	b->cs_released = rec[K22_REC_CS_MODE] == 1u;
	b->app_base = get32(rec + K22_REC_APP_BASE);
	b->app_end = get32(rec + K22_REC_APP_END);
	b->xfloor = get32(rec + K22_REC_XFLOOR);
	b->sck = rec[K22_REC_SCK];
	b->mosi = rec[K22_REC_MOSI];
	b->miso = rec[K22_REC_MISO];
	b->cs = rec[K22_REC_CS];
	if (b->xflash > 1u || rec[K22_REC_CS_MODE] > 1u || b->app_end <= b->app_base || b->xfloor % STUBENG_BLOCK != 0) {
		return false;
	}
	if (b->xflash == 0) {
		return true;		// no SPI flash: the pins are not used
	}
	if (!pin_ok(b->sck) || !pin_ok(b->mosi) || !pin_ok(b->miso) || !pin_ok(b->cs)) {
		return false;
	}
	// Four different pins
	return b->sck != b->mosi && b->sck != b->miso && b->sck != b->cs && b->mosi != b->miso && b->mosi != b->cs &&
	       b->miso != b->cs;
}
