// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stock block map over an SPI NAND (drivers/flash/spi_nand/spi_nand_lut.c, docs/spec/spi-nand-lut.md)
#include <stdio.h>
#include <string.h>
#include "flash/spi_nand/spi_nand_lut.h"

#define BLOCKS	2048u

static int fails;
static uint8_t table[SPI_NAND_LUT_LEN(BLOCKS)];

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL %s\n", what);
		fails++;
	}
}

static void fresh(void)
{
	memcpy(table, SPI_NAND_LUT_MAGIC, 4);
	memset(table + 4, 0xFF, sizeof table - 4);
	table[4] = 0xFE;			// block 0 holds the table
}

static void bad(uint32_t block)
{
	table[4 + block / 8] &= (uint8_t)~(1u << (block % 8));
}

static uint32_t map(uint32_t logical)
{
	uint32_t p = 0xFFFFFFFFu;
	return spi_nand_lut_map(table, sizeof table, BLOCKS, logical, &p) ? p : 0xFFFFFFFFu;
}

int main(void)
{
	// No bad blocks: block 0 alone moves everything up one (the calibration page, logical block 20, is block 21)
	fresh();
	check(map(0) == 1 && map(20) == 21, "block 0 shifts by one");
	// A table as one radio's chip holds it (bad 186, 1536, 1537): nothing below 21 changes, 186 and up shift again
	bad(186); bad(1536); bad(1537);
	check(map(20) == 21, "bad blocks above leave the page");
	check(map(184) == 185 && map(185) == 187, "a bad block is skipped");
	check(map(1533) == 1535 && map(1534) == 1538, "two bad blocks in a row");
	// The pool: the 10 highest usable blocks are out of the logical space
	fresh();
	check(map(BLOCKS - 12) == BLOCKS - 11, "the last block below the pool");
	check(map(BLOCKS - 11) == 0xFFFFFFFFu, "the pool is refused");
	bad(BLOCKS - 1);			// a bad block up there moves the pool down one
	check(map(BLOCKS - 12) == 0xFFFFFFFFu, "a bad top block grows the pool's reach");
	// A bad block below the page moves it
	fresh();
	bad(5);
	check(map(20) == 22, "a bad block below shifts the page");
	// No table, a short one
	fresh();
	table[0] = 0xFF;
	check(map(20) == 0xFFFFFFFFu, "no magic, no map");
	fresh();
	uint32_t p;
	check(!spi_nand_lut_map(table, 100, BLOCKS, 20, &p), "short table refused");
	if (fails) {
		printf("spiNandLutTest: %d failures\n", fails);
		return 1;
	}
	printf("spiNandLutTest: all passed\n");
	return 0;
}
