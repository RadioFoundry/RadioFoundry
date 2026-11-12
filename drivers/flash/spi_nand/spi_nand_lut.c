// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stock block map over an SPI NAND (spi_nand_lut.h, docs/spec/spi-nand-lut.md)
#include <string.h>
#include "spi_nand_lut.h"

static bool usable(const uint8_t *bits, uint32_t block)
{
	return block != 0 && (bits[block / 8u] >> (block % 8u) & 1u);
}

bool spi_nand_lut_map(const uint8_t *table, uint32_t len, uint32_t blocks, uint32_t logical, uint32_t *physical)
{
	if (table == NULL || blocks < 3u || len < SPI_NAND_LUT_LEN(blocks) || memcmp(table, SPI_NAND_LUT_MAGIC, 4) != 0) {
		return false;
	}
	const uint8_t *bits = table + 4;
	// The pool: the SPI_NAND_LUT_POOL highest usable blocks, from the top down to block 2. Every block from its lowest
	// one up is out of the logical space
	uint32_t pool = blocks, found = 0;
	for (uint32_t b = blocks - 1u; b >= 2u && found < SPI_NAND_LUT_POOL; b--) {
		if (usable(bits, b)) {
			pool = b;
			found++;
		}
	}
	// Each block left out at or below the running position pushes it up one
	uint32_t p = logical;
	for (uint32_t b = 0; b < blocks && b <= p; b++) {
		if (!usable(bits, b) || b >= pool) {
			p++;
		}
	}
	if (p >= pool) {
		return false;
	}
	*physical = p;
	return true;
}
