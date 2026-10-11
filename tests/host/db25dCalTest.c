// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D PRO factory calibration (boards/db25d/calibration_db25d.c) on synthetic "TUNE" pages: the choice between the
// two copies, band halves, interpolation and its edges, the PA and delay limits, squelch segments
// (docs/spec/hardware-db25d.md 6)
#include <stdio.h>
#include <string.h>
#include "db25d.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint8_t flash_page[DB25D_CAL_LEN], nand_page[DB25D_CAL_LEN];

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

// Per-point u16 table at off in one half: value = base + step x point (0-based)
static void table16(uint8_t *pg, uint32_t off, uint8_t stride, uint16_t base, int16_t step)
{
	for (int p = 0; p < 8; p++) {
		put16(pg + off + 1 + stride * p, (uint16_t)(base + step * p));
	}
}

static void build(uint8_t *pg, uint32_t count)
{
	memset(pg, 0xFF, DB25D_CAL_LEN);
	memcpy(pg, "TUNE", 4);
	put32(pg + 4, count);
	for (uint32_t h = 0; h <= 0x400; h += 0x400) {
		uint16_t band = h ? 1000 : 0;			// VHF values 1000 above UHF
		table16(pg, h + 0x02C, 2, (uint16_t)(2000 + band), 10);	// RX tune
		table16(pg, h + 0x045, 2, (uint16_t)(1500 + band), 100);	// PA high
		table16(pg, h + 0x056, 2, (uint16_t)(900 + band), -20);	// PA low, falling
		table16(pg, h + 0x0AE, 4, 100, 50);		// TX on delay
		table16(pg, h + 0x0AE + 2, 4, 1800, 100);	// TX off delay: points 4..7 over 2000
		for (int p = 0; p < 8; p++) {
			pg[h + 0x067 + 1 + 2 * p] = (uint8_t)(40 + p);
			pg[h + 0x067 + 2 + 2 * p] = 0xEE;
		}
		for (int seg = 0; seg < 8; seg++) {
			for (int lvl = 1; lvl <= 9; lvl++) {
				// v = 120 + segment - level, high byte noise that must be ignored
				put16(pg + h + 0x0CF + 1 + 18 * seg + 2 * (lvl - 1),
					(uint16_t)(0xAB00 | (uint8_t)(120 + seg - lvl + (h ? 10 : 0))));
			}
		}
	}
	pg[0x43F] = 0x90;
}

// The NAND as the board reads it: nand_page at DB25D_CAL_NAND_ADDR; reads longer than fail_above fail
static uint32_t fail_above = 0xFFFFFFFFu, nand_reads;
static bool nand_reader(uint32_t addr, void *buf, uint32_t len)
{
	nand_reads++;
	if (len > fail_above || addr < DB25D_CAL_NAND_ADDR || addr + len > DB25D_CAL_NAND_ADDR + DB25D_CAL_LEN) {
		return false;
	}
	memcpy(buf, nand_page + (addr - DB25D_CAL_NAND_ADDR), len);
	return true;
}

static void test_nand_load(void)
{
	// NAND newer: read whole, straight from the reader
	build(flash_page, 3);
	build(nand_page, 4);
	put16(nand_page + 0x045 + 1, 1234);
	nand_reads = 0;
	CHECK(db25d_cal_load(flash_page, nand_reader));
	CHECK(db25d_cal_source() == DB25D_CAL_NAND && db25d_cal_counter() == 4 && nand_reads == 2);
	CHECK(db25d_cal_pa(403175000, true) == 1234);
	// Flash newer: only the NAND header is read
	put32(flash_page + 4, 9);
	nand_reads = 0;
	CHECK(db25d_cal_load(flash_page, nand_reader));
	CHECK(db25d_cal_source() == DB25D_CAL_FLASH && db25d_cal_counter() == 9 && nand_reads == 1);
	CHECK(db25d_cal_pa(403175000, true) == 1500);
	// NAND wins on its header but the page read fails: the flash copy, or nothing without one
	put32(flash_page + 4, 2);
	fail_above = 8;
	CHECK(db25d_cal_load(flash_page, nand_reader));
	CHECK(db25d_cal_source() == DB25D_CAL_FLASH && db25d_cal_counter() == 2);
	CHECK(!db25d_cal_load(NULL, nand_reader));
	CHECK(db25d_cal_source() == DB25D_CAL_NONE && !db25d_cal_valid());
	// The header read fails too: as if there were no NAND
	fail_above = 0;
	CHECK(db25d_cal_load(flash_page, nand_reader) && db25d_cal_source() == DB25D_CAL_FLASH);
	CHECK(!db25d_cal_load(NULL, nand_reader));
	fail_above = 0xFFFFFFFFu;
	// No NAND at all
	CHECK(!db25d_cal_load(NULL, NULL));
}

static void test_choice(void)
{
	build(flash_page, 5);
	build(nand_page, 7);
	CHECK(db25d_cal_choose(flash_page, nand_page) == DB25D_CAL_NAND);
	put32(flash_page + 4, 8);
	CHECK(db25d_cal_choose(flash_page, nand_page) == DB25D_CAL_FLASH);
	CHECK(db25d_cal_choose(flash_page, NULL) == DB25D_CAL_FLASH);
	CHECK(db25d_cal_choose(NULL, nand_page) == DB25D_CAL_NAND);
	CHECK(db25d_cal_choose(NULL, NULL) == DB25D_CAL_NONE);
	flash_page[0] = 'X';
	CHECK(db25d_cal_choose(flash_page, nand_page) == DB25D_CAL_NAND);
	nand_page[3] = 0xFF;
	CHECK(db25d_cal_choose(flash_page, nand_page) == DB25D_CAL_NONE);

	CHECK(!db25d_cal_load(flash_page, nand_reader));
	CHECK(!db25d_cal_valid() && db25d_cal_source() == DB25D_CAL_NONE && db25d_cal_counter() == 0);
	CHECK(db25d_cal_pa(446000000, true) == 0 && db25d_cal_rx_tune(446000000) == 0);
	CHECK(db25d_cal_tx_center() == 0x80 && db25d_cal_squelch_db(446000000, 5) == 136);

	build(flash_page, 12);
	CHECK(db25d_cal_load(flash_page, NULL));
	CHECK(db25d_cal_valid() && db25d_cal_source() == DB25D_CAL_FLASH && db25d_cal_counter() == 12);
	// The page is copied: later changes to the source don't show
	put16(flash_page + 0x045 + 1, 7);
	CHECK(db25d_cal_pa(403175000, true) == 1500);
}

static void test_interpolation(void)
{
	build(flash_page, 1);
	db25d_cal_load(flash_page, NULL);
	// UHF: at and below point 1 flat, exact points, midpoint, above point 8 flat at point 8
	CHECK(db25d_cal_pa(400000000, true) == 1500);
	CHECK(db25d_cal_pa(403175000, true) == 1500);
	CHECK(db25d_cal_pa(414175000, true) == 1600);
	CHECK(db25d_cal_pa(408675000, true) == 1550);
	CHECK(db25d_cal_pa(478825000, true) == 2200);
	CHECK(db25d_cal_pa(480000000, true) == 2200);
	// Points 3 -> 4 are 11.45 MHz apart: a quarter of the way is 25.0, a third 33.33 -> 33
	CHECK(db25d_cal_pa(425175000 + 2862500, true) == 1725);
	CHECK(db25d_cal_pa(425175000 + 3816667, true) == 1733);
	// Rounding half up on a falling table: 900 -> 880 over 11 MHz, at 0.55 MHz -1.0, at 0.275 MHz -0.5 -> 900
	CHECK(db25d_cal_pa(403175000 + 550000, false) == 899);
	CHECK(db25d_cal_pa(403175000 + 275000, false) == 900);
	CHECK(db25d_cal_pa(403175000 + 275001, false) == 899);
	// VHF half
	CHECK(db25d_cal_pa(136000000, true) == 2500);
	CHECK(db25d_cal_pa(144125000, true) == 2650);
	CHECK(db25d_cal_pa(174000000, false) == 1760);
	CHECK(db25d_cal_rx_tune(146125000) == 3020);
	CHECK(db25d_cal_rx_tune(436625000) == 2030);
	// Modulation: the first byte of each pair
	CHECK(db25d_cal_mod(403175000) == 40 && db25d_cal_mod(478825000) == 47);
	CHECK(db25d_cal_mod(419675000) == 42);	// halfway 41 -> 42 rounds up
	CHECK(db25d_cal_tx_center() == 0x90);
}

static void test_limits(void)
{
	build(flash_page, 1);
	put16(flash_page + 0x045 + 1 + 2 * 3, 0xFFFF);	// point 4 erased
	put16(flash_page + 0x056 + 1 + 2 * 0, 0);
	db25d_cal_load(flash_page, NULL);
	CHECK(db25d_cal_pa(436625000, true) == 0);
	CHECK(db25d_cal_pa(400000000, false) == 0);
	CHECK(db25d_cal_pa(414175000, true) == 1600);

	uint16_t on, off;
	db25d_cal_tx_delays(403175000, &on, &off);
	CHECK(on == 100 && off == 1800);
	db25d_cal_tx_delays(425175000, &on, &off);
	CHECK(on == 200 && off == 2000);
	db25d_cal_tx_delays(436625000, &on, &off);
	CHECK(on == 250 && off == 0);	// 2100 > 2000
	db25d_cal_tx_delays(146125000, &on, &off);
	CHECK(on == 200 && off == 2000);
}

static void test_squelch(void)
{
	build(flash_page, 1);
	db25d_cal_load(flash_page, NULL);
	CHECK(db25d_cal_squelch_db(446000000, 0) == 136);
	CHECK(db25d_cal_squelch_db(446000000, 10) == 136);
	CHECK(db25d_cal_squelch_db(405000000, 1) == 119);	// segment 1
	CHECK(db25d_cal_squelch_db(409999999, 9) == 111);
	CHECK(db25d_cal_squelch_db(410000000, 1) == 120);	// an edge belongs to the next segment
	CHECK(db25d_cal_squelch_db(469999999, 3) == 123);	// segment 7
	CHECK(db25d_cal_squelch_db(470000000, 3) == 124);	// segment 8
	CHECK(db25d_cal_squelch_db(479000000, 9) == 118);
	CHECK(db25d_cal_squelch_db(141999999, 1) == 129);	// VHF segment 1
	CHECK(db25d_cal_squelch_db(142000000, 1) == 130);
	CHECK(db25d_cal_squelch_db(173000000, 2) == 135);	// VHF segment 8

}

int main(void)
{
	test_choice();
	test_nand_load();
	test_interpolation();
	test_limits();
	test_squelch();
	if (failures) {
		printf("db25dCalTest: %d failures\n", failures);
		return 1;
	}
	printf("db25dCalTest: all passed\n");
	return 0;
}
