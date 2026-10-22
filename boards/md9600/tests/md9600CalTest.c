// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// drivers/cal/calblock and the MD-9600's calibration and option block reader (boards/md9600/cal_md9600.c) on
// synthetic blocks: loading from pieces, read failures, blank blocks and rows, the row layout and interpolation of
// docs/spec/hardware-md9600.md 11, the band edges and the backlight bit. No real calibration data.
#include <stdio.h>
#include <string.h>
#include "cal/calblock/calblock.h"
#include "md9600.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// ---- The security registers the board reads (flash_md9600.c's md9600_flash_read_security) ----

static uint8_t sec[3][256];
static int fail_reg;	// a register whose read fails, 0 = none
static unsigned reads;

bool md9600_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len)
{
	reads++;
	if (reg < 1 || reg > 3 || offset + len > 256 || reg == fail_reg) {
		return false;
	}
	memcpy(buf, &sec[reg - 1][offset], len);
	return true;
}

// ---- calblock ----

static uint8_t src[64];
static bool src_read(uint32_t addr, void *buf, uint32_t len)
{
	if (addr == 0xBAD) {
		return false;
	}
	memcpy(buf, &src[addr], len);
	return true;
}

static void test_calblock(void)
{
	uint8_t buf[24];
	calblock_t b = { .data = buf, .size = sizeof buf };
	for (unsigned i = 0; i < sizeof src; i++) {
		src[i] = (uint8_t)(0x10 + i);
	}
	// Two pieces back to back, the rest of the buffer erased
	static const calblock_part_t two[] = { { 0, 8 }, { 32, 8 } };
	CHECK(calblock_load(&b, src_read, two, 2) == CALBLOCK_OK);
	CHECK(buf[0] == 0x10 && buf[7] == 0x17 && buf[8] == 0x30 && buf[15] == 0x37 && buf[16] == 0xFF);
	CHECK(calblock_byte(&b, 8) == 0x30 && calblock_byte(&b, 24) == 0xFF && calblock_u16le(&b, 0) == 0x1110);
	CHECK(strcmp(calblock_state_name(&b), "factory") == 0);
	// A failing piece: nothing kept
	static const calblock_part_t bad[] = { { 0, 8 }, { 0xBAD, 8 } };
	CHECK(calblock_load(&b, src_read, bad, 2) == CALBLOCK_READ_FAILED);
	CHECK(buf[0] == 0xFF && calblock_byte(&b, 0) == 0xFF && strcmp(calblock_state_name(&b), "unreadable") == 0);
	// Pieces longer than the buffer: refused, not overrun
	static const calblock_part_t big[] = { { 0, 16 }, { 16, 16 } };
	CHECK(calblock_load(&b, src_read, big, 2) == CALBLOCK_READ_FAILED);
	CHECK(calblock_load(&b, NULL, two, 2) == CALBLOCK_READ_FAILED);
	// Blank blocks: erased and zeroed
	memset(buf, 0xFF, sizeof buf);
	CHECK(calblock_check(&b) == CALBLOCK_BLANK);
	memset(buf, 0x00, sizeof buf);
	CHECK(calblock_check(&b) == CALBLOCK_BLANK && strcmp(calblock_state_name(&b), "blank") == 0);

	// Rows: 3 points from 100 Hz every 10 Hz at offset 4
	memset(buf, 0x55, sizeof buf);
	buf[4] = 10;
	buf[5] = 20;
	buf[6] = 40;
	CHECK(calblock_check(&b) == CALBLOCK_OK);
	calblock_row_t r = { .offset = 4, .points = 3, .flags = CAL_CURVE_EXTEND, .first_hz = 100, .step_hz = 10 };
	int32_t v = -1;
	CHECK(calblock_row_at(&b, &r, 100, &v) && v == 10);
	CHECK(calblock_row_at(&b, &r, 50, &v) && v == 10);		// below: the first point
	CHECK(calblock_row_at(&b, &r, 105, &v) && v == 15);
	CHECK(calblock_row_at(&b, &r, 115, &v) && v == 30);
	CHECK(calblock_row_at(&b, &r, 130, &v) && v == 60);		// past the end: the last slope
	r.flags = 0;
	CHECK(calblock_row_at(&b, &r, 130, &v) && v == 40);		// or the last value
	// Blank and out-of-range rows give no value and leave the output alone
	buf[4] = buf[5] = buf[6] = 0xFF;
	v = 1234;
	CHECK(calblock_row_blank(&b, &r) && !calblock_row_at(&b, &r, 110, &v) && v == 1234);
	calblock_row_t past = { .offset = 22, .points = 3, .first_hz = 1, .step_hz = 1 };
	CHECK(calblock_row_blank(&b, &past) && !calblock_row_at(&b, &past, 1, &v));
	calblock_row_t many = { .offset = 0, .points = CAL_CURVE_MAX_POINTS + 1, .first_hz = 1, .step_hz = 1 };
	CHECK(!calblock_row_at(&b, &many, 1, &v));

	// BCD
	uint32_t d = 0;
	CHECK(calblock_bcd(0x1450, 4, &d) && d == 1450);
	CHECK(calblock_bcd(0x00400025, 8, &d) && d == 400025);
	CHECK(!calblock_bcd(0x14A0, 4, &d) && !calblock_bcd(0x1, 0, &d) && !calblock_bcd(0x1, 9, &d));
}

// ---- The MD-9600 layout ----

static uint8_t cal[512], opt[64];

static void put_row(uint16_t off, const uint8_t *p, unsigned n)
{
	memcpy(&cal[off], p, n);
}

static void put_edge(unsigned i, uint16_t bcd)
{
	opt[0x14 + 2 * i] = (uint8_t)bcd;
	opt[0x14 + 2 * i + 1] = (uint8_t)(bcd >> 8);
}

static void synthetic(void)
{
	memset(cal, 0xFF, sizeof cal);
	memset(opt, 0xFF, sizeof opt);
	static const uint8_t u_tune[9] = { 100, 110, 120, 130, 140, 150, 160, 170, 180 };
	static const uint8_t v_tune[5] = { 50, 60, 70, 80, 90 };
	static const uint8_t u_high[9] = { 200, 201, 202, 203, 204, 205, 206, 207, 208 };
	static const uint8_t v_sq9o[5] = { 31, 32, 33, 34, 35 };
	put_row(0x30, u_tune, 9);
	put_row(0x39, v_tune, 5);
	put_row(0x10, u_high, 9);
	put_row(0xE0, v_sq9o, 5);
	cal[0x04] = 40;		// RSSI at -120 / -70 dBm
	cal[0x05] = 140;
	cal[0x09] = 128 + 5;	// UHF reference +5
	cal[0x0C] = 128 - 7;	// VHF reference -7
	// Band edges in a rotated order, as some radios store them
	put_edge(0, 0x4000);
	put_edge(1, 0x4800);
	put_edge(2, 0x1360);
	put_edge(3, 0x1740);
	opt[0x1D] = 0x00;
}

static void test_md9600_rows(void)
{
	int32_t v;
	synthetic();
	md9600_cal_set(cal, opt);
	CHECK(md9600_cal_valid() && strcmp(md9600_cal_source(), "factory") == 0);
	// Front-end tuning: byte x 16, by 10 MHz from 400 MHz (UHF) and 9.5 MHz from 136 MHz (VHF)
	CHECK(md9600_cal_rx_tune(400000000u) == 1600);
	CHECK(md9600_cal_rx_tune(405000000u) == 1680);
	CHECK(md9600_cal_rx_tune(480000000u) == 2880);
	CHECK(md9600_cal_rx_tune(490000000u) == 3040);		// the last slope continued
	CHECK(md9600_cal_rx_tune(136000000u) == 800);
	CHECK(md9600_cal_rx_tune(140750000u) == 880);
	CHECK(md9600_cal_rx_tune(174000000u) == 1440);
	CHECK(md9600_cal_rx_tune(100000000u) == 800);		// below the band: the first point
	// Other rows by their offsets; a blank one has no value
	CHECK(md9600_cal_row(MD9600_CAL_POWER_HIGH, 440000000u, &v) && v == 204);
	CHECK(md9600_cal_row(MD9600_CAL_SQ9_OPEN, 155000000u, &v) && v == 33);
	CHECK(!md9600_cal_row(MD9600_CAL_POWER_HIGH, 150000000u, &v));		// VHF high power row blank
	CHECK(!md9600_cal_row(MD9600_CAL_SQ9_OPEN, 440000000u, &v));		// UHF squelch row blank
	CHECK(!md9600_cal_row(MD9600_CAL_ROW_COUNT, 440000000u, &v));
	// Fixed bytes
	int8_t ref = 0;
	uint8_t lo = 0, hi = 0;
	CHECK(md9600_cal_ref_tune(true, &ref) && ref == 5);
	CHECK(md9600_cal_ref_tune(false, &ref) && ref == -7);
	CHECK(md9600_cal_rssi(&lo, &hi) && lo == 40 && hi == 140);
	CHECK(md9600_cal_byte(0x30) == 100 && md9600_cal_byte(512) == 0xFF);
	// A row whose points tune to past the DAC's range clamps
	static const uint8_t steep[9] = { 200, 220, 240, 250, 252, 253, 254, 254, 254 };
	put_row(0x30, steep, 9);
	md9600_cal_set(cal, opt);
	CHECK(md9600_cal_rx_tune(480000000u) == 4064 && md9600_cal_rx_tune(470000000u) == 4064);
	CHECK(md9600_cal_rx_tune(420000000u) == 3840);
	// No front-end row: mid scale
	memset(&cal[0x30], 0xFF, 9);
	md9600_cal_set(cal, opt);
	CHECK(md9600_cal_rx_tune(440000000u) == 2048 && md9600_cal_rx_tune(140750000u) == 880);
}

static void test_md9600_absent(void)
{
	int32_t v;
	int8_t ref;
	uint8_t lo, hi;
	md9600_band_edges_t e;
	// Unreadable
	md9600_cal_set(NULL, NULL);
	CHECK(!md9600_cal_valid() && strcmp(md9600_cal_source(), "unreadable") == 0);
	CHECK(md9600_cal_rx_tune(440000000u) == 2048 && !md9600_cal_row(MD9600_CAL_RX_TUNE, 440000000u, &v));
	CHECK(!md9600_cal_ref_tune(true, &ref) && !md9600_cal_rssi(&lo, &hi) && md9600_cal_byte(0) == 0xFF);
	CHECK(!md9600_opt_band_edges(&e) && !md9600_opt_backlight_switched() && md9600_opt_byte(0x1D) == 0xFF);
	// Erased (a radio never calibrated, or registers wiped)
	memset(cal, 0xFF, sizeof cal);
	memset(opt, 0xFF, sizeof opt);
	md9600_cal_set(cal, opt);
	CHECK(strcmp(md9600_cal_source(), "blank") == 0 && strcmp(md9600_opt_source(), "blank") == 0);
	CHECK(md9600_cal_rx_tune(440000000u) == 2048 && !md9600_opt_backlight_switched());
}

static void test_md9600_options(void)
{
	md9600_band_edges_t e;
	synthetic();
	md9600_cal_set(cal, opt);
	CHECK(strcmp(md9600_opt_source(), "factory") == 0);
	CHECK(md9600_opt_band_edges(&e));
	CHECK(e.vhf_lo_hz == 136000000u && e.vhf_hi_hz == 174000000u);
	CHECK(e.uhf_lo_hz == 400000000u && e.uhf_hi_hz == 480000000u);
	CHECK(!md9600_opt_backlight_switched());
	opt[0x1D] = 0x10;
	md9600_cal_set(cal, opt);
	CHECK(md9600_opt_backlight_switched());
	// Edges that don't make two bands, or don't decode, give none
	put_edge(1, 0x1450);
	md9600_cal_set(cal, opt);
	CHECK(!md9600_opt_band_edges(&e));
	synthetic();
	put_edge(2, 0x13A0);
	md9600_cal_set(cal, opt);
	CHECK(!md9600_opt_band_edges(&e));
	synthetic();
	put_edge(3, 0x1360);	// both VHF edges the same
	md9600_cal_set(cal, opt);
	CHECK(!md9600_opt_band_edges(&e));
}

static void test_md9600_load(void)
{
	// md9600_cal_load reads registers 1 and 2 as one 512-byte block and register 3's first 64 bytes
	synthetic();
	memcpy(sec[0], cal, 256);
	memcpy(sec[1], cal + 256, 256);
	memcpy(sec[2], opt, 64);
	memset(sec[2] + 64, 0xA5, 192);
	fail_reg = 0;
	reads = 0;
	md9600_cal_load();
	CHECK(reads == 3 && md9600_cal_valid() && md9600_cal_rx_tune(400000000u) == 1600);
	md9600_band_edges_t e;
	CHECK(md9600_opt_band_edges(&e) && e.uhf_hi_hz == 480000000u);
	// Register 2 unreadable: no calibration at all (half a table is not used), the options still read
	fail_reg = 2;
	md9600_cal_load();
	CHECK(!md9600_cal_valid() && strcmp(md9600_cal_source(), "unreadable") == 0);
	CHECK(md9600_cal_rx_tune(400000000u) == 2048 && md9600_opt_band_edges(&e));
	fail_reg = 3;
	md9600_cal_load();
	CHECK(md9600_cal_valid() && strcmp(md9600_opt_source(), "unreadable") == 0);
}

int main(void)
{
	test_calblock();
	test_md9600_rows();
	test_md9600_absent();
	test_md9600_options();
	test_md9600_load();
	printf("# md9600CalTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
