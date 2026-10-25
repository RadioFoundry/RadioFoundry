// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4 stub's chip facts and its board record check (f4stub.h). Portable: the host tests run it too.
#include "f4stub.h"

const stubeng_run_t f4_sector_runs[3] = {
	{ 0x4000u, 4 },		// sectors 0-3
	{ 0x10000u, 1 },	// 4
	{ 0x20000u, 7 },	// 5-11 (single bank parts, up to 1 MB)
};

// System memory with OTP, the UID and the flash size register; the option bytes; SRAM; CCM
const stubeng_range_t f4_peek_ranges[4] = {
	{ 0x1FFF0000u, 0x1FFF7A30u },
	{ 0x1FFFC000u, 0x1FFFC010u },
	{ 0x20000000u, 0x20020000u },
	{ 0x10000000u, 0x10010000u },
};

stubeng_flash_t f4_flash(uint16_t flash_kb)
{
	stubeng_flash_t f = {
		.base = F4_FLASH_BASE,
		.size = (uint32_t)flash_kb * 1024u,
		.runs = f4_sector_runs,
		.nruns = 3,
		.prog_unit = 4,		// word programming (PSIZE x32)
		.min_app_base = F4_MIN_APP_BASE,
	};
	return f;
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Ports A to I (the largest STM32F405 package has them all)
static bool pin_ok(uint8_t pin)
{
	return (pin >> 4) <= 8u;
}

static bool pin_or_none(uint8_t pin)
{
	return pin == F4_PIN_NONE || pin_ok(pin);
}

bool f4_board_parse(const uint8_t *rec, uint32_t n, f4_board_t *b)
{
	if (n != F4_REC_LEN || rec[F4_REC_BOARD] == 0 || rec[F4_REC_FORMAT] != F4_REC_LAYOUT || rec[F4_REC_ZERO] != 0) {
		return false;
	}
	b->board = rec[F4_REC_BOARD];
	b->spi_bus = rec[F4_REC_SPI_BUS];
	b->spi_mode = rec[F4_REC_SPI_MODE];
	b->af = rec[F4_REC_AF];
	b->hse_hz = get32(rec + F4_REC_HSE_HZ);
	b->app_base = get32(rec + F4_REC_APP_BASE);
	b->spi_hz = get32(rec + F4_REC_SPI_HZ);
	b->sck = rec[F4_REC_SCK];
	b->miso = rec[F4_REC_MISO];
	b->mosi = rec[F4_REC_MOSI];
	b->cs = rec[F4_REC_CS];
	b->idle[0] = rec[F4_REC_IDLE0];
	b->idle[1] = rec[F4_REC_IDLE1];
	if (b->hse_hz < 4000000u || b->hse_hz > 26000000u) {
		return false;		// the STM32F405's HSE range
	}
	if (b->spi_bus == 0) {
		return true;		// no SPI flash: the pins are not used
	}
	if (b->spi_bus > 3u || b->spi_mode > 3u || b->af > 15u || b->spi_hz < 100000u) {
		return false;
	}
	if (!pin_ok(b->sck) || !pin_ok(b->miso) || !pin_ok(b->mosi) || !pin_ok(b->cs) || !pin_or_none(b->idle[0]) ||
	    !pin_or_none(b->idle[1])) {
		return false;
	}
	// The chip select must not be one of the bus pins or a held-high pin
	return b->cs != b->sck && b->cs != b->miso && b->cs != b->mosi && b->cs != b->idle[0] && b->cs != b->idle[1];
}
