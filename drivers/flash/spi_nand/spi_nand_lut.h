// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The block map a radio's stock firmware keeps over its SPI NAND (docs/spec/spi-nand-lut.md). Its partitions are
// logical addresses: block 0 holds a table ("LUT0", then one bit per block, 1 = usable), and logical block L is the
// (L+1)-th usable block, counted from the bottom, below the reserved pool at the top. A reader of a stock partition
// (the factory calibration page) maps the address through the table; it never writes the table or the partitions.
#ifndef SPI_NAND_LUT_H
#define SPI_NAND_LUT_H

#include <stdint.h>
#include <stdbool.h>

#define SPI_NAND_LUT_MAGIC	"LUT0"
#define SPI_NAND_LUT_POOL	10u	// the highest usable blocks, held back as the stock firmware's spare pool

// Bytes of block 0 the table takes on a chip of `blocks` blocks
#define SPI_NAND_LUT_LEN(blocks)	(4u + ((blocks) + 7u) / 8u)

// The physical block that holds logical block `logical`, from the start of block 0 (`len` bytes). False if the table
// is absent or short, or the block maps past the usable blocks.
bool spi_nand_lut_map(const uint8_t *table, uint32_t len, uint32_t blocks, uint32_t logical, uint32_t *physical);

#endif
