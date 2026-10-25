// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 calibration point (boards/gd77/cal_gd77.c, docs/spec/hardware-gd77.md 6.4) from synthetic blocks: the node
// layout per range and the interpolation between nodes (both ends, the last node, above it, outside), both power
// levels, both bandwidths, every destination value and its fallback, the band split at 180 MHz, invalid blocks, the
// drive rule's 0xA0 outside its range, and the generic interpolation helper (drivers/cal)
#include <stdio.h>
#include <string.h>
#include "cal_gd77.h"
#include "cal_blocks_gd77.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// A block that passes the check: ranges in 100 kHz units; drive bytes rising (low power) and falling (high power) so
// both directions of the interpolation are covered; distinct values in every other field
static void block(uint8_t *b, uint16_t t0lo, uint16_t t0hi, uint16_t t1lo, uint16_t t1hi)
{
	static const uint8_t mod[8] = { 0x50, 0x58, 0x55, 0x61, 0x61, 0x70, 0x7A, 0x80 };
	static const uint8_t pairs[12] = { 0x40, 0x38, 0x31, 0x30, 0x53, 0x50, 0x3B, 0x36, 0x32, 0x2E, 0x51, 0x4F };
	memset(b, 0, GD77_CAL_BLOCK);
	const uint16_t hdr[4] = { t0lo, t0hi, t1lo, t1hi };
	for (int i = 0; i < 4; i++) {
		b[2 * i] = (uint8_t)hdr[i];
		b[2 * i + 1] = (uint8_t)(hdr[i] >> 8);
	}
	b[0x08] = 0x34; b[0x09] = 0x02; b[0x0A] = 0x7B;	// id 0x40: bias 0x234, offset 0x7B
	for (int k = 0; k < 24; k++) {			// id 0x41
		b[0x0B + 2 * k] = (uint8_t)(0x20 + 3 * k);
		b[0x0B + 2 * k + 1] = (uint8_t)(0xF0 - 5 * k);
	}
	for (int k = 0; k < 8; k++) {			// id 0x43 squelch nodes
		b[0x3F + k] = (uint8_t)(0x30 + 4 * k);
	}
	memcpy(&b[0x47], pairs, sizeof pairs);		// id 0x43 pairs: 25 kHz, then 12.5 kHz
	memcpy(&b[0x55], mod, sizeof mod);		// id 0x01
	b[0x60] = 0x0C; b[0x61] = 0x09;			// id 0x33: 25, 12.5 kHz
	b[0x62] = 0x15; b[0x63] = 0x0E;			// id 0x34
	b[0x64] = 0x0A; b[0x65] = 0x1A; b[0x66] = 0x0C; b[0x67] = 0x05;	// id 0x50
	b[0x68] = 0xC0; b[0x69] = 0x02; b[0x6A] = 0x60; b[0x6B] = 0x01;	// FM deviation 0x2C0 (25), 0x160 (12.5)
	b[0x6C] = 0x0E; b[0x6D] = 0x09;			// id 0x51
}

typedef struct {
	uint32_t hz;
	uint8_t low, high, mod1, sq;
} want_t;

static void check_range(uint16_t lo, uint16_t hi, const want_t *w, int n)
{
	uint8_t b[GD77_CAL_BLOCK];
	block(b, lo, hi, lo, hi);
	for (int i = 0; i < n; i++) {
		gd77_cal_point_t p;
		bool ok = gd77_cal_convert(b, w[i].hz, &p);
		CHECK(ok && p.valid && p.rx_valid && p.sq_valid);
		if (p.power_drive[0] != w[i].low || p.power_drive[1] != w[i].high || p.c6k_mod1 != w[i].mod1 ||
		    p.sq_open != w[i].sq) {
			printf("  range %u-%u at %u Hz: %02X %02X %02X %02X\n", lo, hi, (unsigned)w[i].hz, p.power_drive[0],
			       p.power_drive[1], p.c6k_mod1, p.sq_open);
			CHECK(false);
		}
		CHECK(gd77_cal_drive(b, w[i].hz, 0) == w[i].low && gd77_cal_drive(b, w[i].hz, 1) == w[i].high);
	}
}

static void test_nodes(void)
{
	// 400-470 MHz: drive nodes every 5 MHz from 400 to 460 and the last at 470; modulation and squelch nodes every
	// 10 MHz from 400 to 460, the modulation value carried on above 460
	static const want_t r4700[] = {
		{ 400000000u, 0x20, 0xF0, 0x50, 0x30 },		// the low end: node 0
		{ 402500000u, 0x21, 0xEE, 0x52, 0x30 },
		{ 403370000u, 0x21, 0xED, 0x52, 0x30 },		// truncated toward the lower node's value
		{ 437770000u, 0x36, 0xCB, 0x61, 0x3C },
		{ 460000000u, 0x44, 0xB4, 0x7A, 0x48 },		// the last modulation node
		{ 465000000u, 0x45, 0xB2, 0x7F, 0x48 },		// the 10 MHz last drive segment; modulation above it
		{ 467890000u, 0x46, 0xB1, 0x81, 0x48 },
		{ 470000000u, 0x47, 0xAF, 0x84, 0x48 },		// the high end: the last drive node
		{ 470099999u, 0x47, 0xAF, 0x84, 0x48 },		// still 4700 in 100 kHz units
	};
	check_range(4000, 4700, r4700, (int)(sizeof r4700 / sizeof r4700[0]));
	// 400-480: 16 drive nodes, 8 modulation nodes 10 MHz apart
	static const want_t r4800[] = {
		{ 400000000u, 0x20, 0xF0, 0x50, 0x30 },
		{ 455550000u, 0x41, 0xB9, 0x75, 0x44 },
		{ 470000000u, 0x4A, 0xAA, 0x80, 0x4C },
		{ 475000000u, 0x4B, 0xA8, 0x83, 0x4C },
		{ 479990000u, 0x4C, 0xA6, 0x85, 0x4C },
		{ 480000000u, 0x4D, 0xA5, 0x86, 0x4C },
	};
	check_range(4000, 4800, r4800, (int)(sizeof r4800 / sizeof r4800[0]));
	// 400-520: all 24 drive nodes, 8 modulation nodes 15 MHz apart
	static const want_t r5200[] = {
		{ 400000000u, 0x20, 0xF0, 0x50, 0x30 },
		{ 401230000u, 0x20, 0xEF, 0x50, 0x30 },
		{ 487650000u, 0x54, 0x99, 0x78, 0x44 },
		{ 505000000u, 0x5F, 0x87, 0x80, 0x4C },
		{ 512340000u, 0x62, 0x81, 0x82, 0x4C },
		{ 519990000u, 0x64, 0x7E, 0x85, 0x4C },
		{ 520000000u, 0x65, 0x7D, 0x86, 0x4C },		// bytes 46 and 47 of id 0x41
	};
	check_range(4000, 5200, r5200, (int)(sizeof r5200 / sizeof r5200[0]));
	// 136-174: drive nodes 136.0, 140, 145 .. 165, 174; modulation nodes 136.0, 140 .. 170, carried on to 174
	static const want_t r1740[] = {
		{ 136000000u, 0x20, 0xF0, 0x50, 0x30 },
		{ 137500000u, 0x21, 0xEF, 0x53, 0x30 },		// the first segment is 136.0-140.0
		{ 139990000u, 0x22, 0xEC, 0x57, 0x30 },
		{ 152340000u, 0x2A, 0xDF, 0x61, 0x3C },
		{ 165000000u, 0x32, 0xD2, 0x7A, 0x48 },
		{ 170000000u, 0x33, 0xD0, 0x80, 0x4C },
		{ 171230000u, 0x34, 0xCF, 0x81, 0x4C },
		{ 174000000u, 0x35, 0xCD, 0x86, 0x4C },
		{ 174099999u, 0x35, 0xCD, 0x86, 0x4C },
	};
	check_range(1360, 1740, r1740, (int)(sizeof r1740 / sizeof r1740[0]));
}

static void test_outside(void)
{
	uint8_t b[GD77_CAL_BLOCK];
	gd77_cal_point_t p;
	block(b, 4000, 4700, 4000, 4700);
	// Just outside the TX range on either side: no TX point, the receive side still from the block
	static const uint32_t out[] = { 399999999u, 470100000u, 145000000u, 0u, UINT32_MAX };
	for (unsigned i = 0; i < sizeof out / sizeof out[0]; i++) {
		CHECK(!gd77_cal_convert(b, out[i], &p) && !p.valid && p.rx_valid && !p.sq_valid);
		CHECK(p.power_drive[0] == GD77_CAL_SAFE_DRIVE && p.power_drive[1] == GD77_CAL_SAFE_DRIVE);
		// The block's own drive rule outside its range
		CHECK(gd77_cal_drive(b, out[i], 0) == GD77_CAL_DRIVE_OUTSIDE && gd77_cal_drive(b, out[i], 1) == 0xA0);
	}
	CHECK(gd77_cal_drive(b, 446000000u, 2) == GD77_CAL_DRIVE_OUTSIDE);	// no such power level

	// A TX range the node table doesn't know: passes the block check, but no TX point
	block(b, 4000, 4700, 4000, 4500);
	CHECK(gd77_cal_check(b));
	CHECK(!gd77_cal_convert(b, 446000000u, &p) && !p.valid && p.rx_valid && p.sq_valid);
	CHECK(gd77_cal_drive(b, 446000000u, 0) == GD77_CAL_DRIVE_OUTSIDE);
	// A VHF range other than 136-174
	block(b, 1360, 1740, 1360, 1750);
	CHECK(!gd77_cal_convert(b, 150000000u, &p) && !p.valid);
	// The RX range alone: TX fine, squelch base only where the RX range has the frequency
	block(b, 4300, 4700, 4000, 4700);
	CHECK(gd77_cal_convert(b, 420000000u, &p) && p.valid && !p.sq_valid && p.sq_open == 0);
	CHECK(gd77_cal_convert(b, 446000000u, &p) && p.valid && p.sq_valid && p.sq_open == 0x34);	// node 1 (440)
	// A two-point bias above its limit: no TX point
	block(b, 4000, 4700, 4000, 4700);
	b[0x08] = 0x01; b[0x09] = 0x04;
	CHECK(!gd77_cal_convert(b, 446000000u, &p) && !p.valid && p.rx_valid);
	b[0x08] = 0x00;
	CHECK(gd77_cal_convert(b, 446000000u, &p) && p.valid && p.c6k_bias == 0x400);
}

static void test_fields(void)
{
	uint8_t b[GD77_CAL_BLOCK];
	gd77_cal_point_t p;
	block(b, 4000, 4700, 4000, 4700);
	CHECK(gd77_cal_convert(b, 446000000u, &p) && p.valid);
	CHECK(p.power_drive[0] == 0x3B && p.power_drive[1] == 0xC2);	// low, high
	CHECK(p.c6k_mod1 == 0x6A && p.c6k_bias == 0x234 && p.c6k_offset == 0x7B);
	CHECK(p.pa_drv == 0x0A && p.mic_pga == 0x1A && p.voice_gain == 0x0C && p.gain_tx == 0x05);
	// Index 0 = 12.5 kHz, 1 = 25 kHz (the block keeps 25 kHz first)
	CHECK(p.fm_dev[0] == 0x160 && p.fm_dev[1] == 0x2C0);
	CHECK(p.ctcss_dev[0] == 0x09 && p.ctcss_dev[1] == 0x0C && p.dcs_dev[0] == 0x0E && p.dcs_dev[1] == 0x15);
	CHECK(p.rx_dac_gain == 0x0E && p.rx_volume == 0x09);
	CHECK(p.sq_reg[1][0] == ((0x40 << 7) | 0x38) && p.sq_reg[1][1] == ((0x31 << 7) | 0x30) &&
	      p.sq_reg[1][2] == ((0x53 << 7) | 0x50));
	CHECK(p.sq_reg[0][0] == ((0x3B << 7) | 0x36) && p.sq_reg[0][1] == ((0x32 << 7) | 0x2E) &&
	      p.sq_reg[0][2] == ((0x51 << 7) | 0x4F));
	CHECK(p.sq_open == 0x40);

	// Each byte past its field's range takes the fallback (the check limits id 0x50's first three bytes, so those
	// stay below the check's limits here)
	b[0x64] = 0x10; b[0x65] = 0x20; b[0x67] = 0x10;
	b[0x68] = 0x00; b[0x69] = 0x04;		// FM deviation 0x400 (25 kHz)
	b[0x61] = 0x40; b[0x62] = 0xFF;		// CTCSS 12.5 kHz, DCS 25 kHz
	b[0x6C] = 0x10; b[0x6D] = 0xFF;
	b[0x47] = 0x30;				// 25 kHz first pair: high below low
	b[0x4F] = 0x7F; b[0x50] = 0x00;		// 12.5 kHz second pair: high 0x7F
	b[0x52] = 0x7F;				// 12.5 kHz third pair: low 0x7F
	CHECK(gd77_cal_convert(b, 446000000u, &p) && p.valid);
	CHECK(p.pa_drv == GD77_CAL_DEF_PA_DRV && p.mic_pga == GD77_CAL_DEF_MIC_PGA && p.gain_tx == GD77_CAL_DEF_GAIN_TX);
	CHECK(p.voice_gain == 0x0C);
	CHECK(p.fm_dev[1] == GD77_CAL_DEF_FM_DEV && p.fm_dev[0] == 0x160);
	CHECK(p.ctcss_dev[0] == GD77_CAL_DEF_TONE_DEV && p.ctcss_dev[1] == 0x0C);
	CHECK(p.dcs_dev[1] == GD77_CAL_DEF_TONE_DEV && p.dcs_dev[0] == 0x0E);
	CHECK(p.rx_dac_gain == GD77_CAL_DEF_DAC_GAIN && p.rx_volume == GD77_CAL_DEF_VOLUME);
	CHECK(p.sq_reg[1][0] == GD77_CAL_DEF_SQ_48 && p.sq_reg[1][1] == ((0x31 << 7) | 0x30));
	CHECK(p.sq_reg[0][1] == GD77_CAL_DEF_SQ_60 && p.sq_reg[0][2] == GD77_CAL_DEF_SQ_3F);
	// Equal high and low is a valid pair
	block(b, 4000, 4700, 4000, 4700);
	b[0x48] = 0x40;
	CHECK(gd77_cal_convert(b, 446000000u, &p) && p.sq_reg[1][0] == ((0x40 << 7) | 0x40));

	// Above the last modulation node the carried-on value saturates instead of wrapping
	block(b, 4000, 4700, 4000, 4700);
	b[0x55 + 5] = 0x10; b[0x55 + 6] = 0xF0;
	CHECK(gd77_cal_convert(b, 470000000u, &p) && p.c6k_mod1 == 0xFF);
	CHECK(gd77_cal_convert(b, 460000000u, &p) && p.c6k_mod1 == 0xF0);
	b[0x55 + 5] = 0xF0; b[0x55 + 6] = 0x10;
	CHECK(gd77_cal_convert(b, 470000000u, &p) && p.c6k_mod1 == 0x00);
}

static void test_invalid(void)
{
	uint8_t b[GD77_CAL_BLOCK];
	gd77_cal_point_t p;
	// No block, a blank block, one that fails the check: the safe values, nothing from the block
	memset(&p, 0x55, sizeof p);
	CHECK(!gd77_cal_convert(NULL, 446000000u, &p) && !p.valid && !p.rx_valid && !p.sq_valid);
	CHECK(p.power_drive[0] == GD77_CAL_SAFE_DRIVE && p.power_drive[1] == GD77_CAL_SAFE_DRIVE);
	CHECK(p.rx_volume == GD77_CAL_DEF_VOLUME && p.sq_reg[0][0] == GD77_CAL_DEF_SQ_48);
	memset(b, 0xFF, sizeof b);
	CHECK(!gd77_cal_convert(b, 446000000u, &p) && !p.valid && !p.rx_valid);
	block(b, 4000, 4700, 4000, 4700);
	b[0x66] = 0x10;		// id 0x50 byte 2 at the check's limit
	CHECK(!gd77_cal_convert(b, 446000000u, &p) && !p.valid && !p.rx_valid);
}

// --- Through the loader: the band split ---

static uint8_t eeprom[0x10000];

static bool ee_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
	(void)ctx;
	if (addr + len > sizeof eeprom) {
		return false;
	}
	memcpy(buf, &eeprom[addr], len);
	return true;
}

static void test_band_split(void)
{
	gd77_cal_point_t p;
	memset(eeprom, 0xFF, sizeof eeprom);
	block(&eeprom[GD77_CAL_UHF_ADDR], 4000, 4700, 4000, 4700);
	block(&eeprom[GD77_CAL_VHF_ADDR], 1360, 1740, 1360, 1740);
	eeprom[GD77_CAL_VHF_ADDR + 0x6D] = 0x03;	// tell the blocks apart by their RX volume
	gd77_cal_load(ee_read, NULL);
	CHECK(gd77_cal_at(446000000u, &p) && p.valid && p.rx_volume == 0x09 && p.power_drive[0] == 0x3B);
	CHECK(gd77_cal_at(145000000u, &p) && p.valid && p.rx_volume == 0x03);
	// 179.999999 MHz reads the VHF block, 180 MHz the UHF one; neither range has them
	CHECK(!gd77_cal_at(179999999u, &p) && !p.valid && p.rx_valid && p.rx_volume == 0x03);
	CHECK(!gd77_cal_at(180000000u, &p) && !p.valid && p.rx_valid && p.rx_volume == 0x09);
	// A VHF block that fails the check: VHF gets the safe values, UHF is unaffected
	eeprom[GD77_CAL_VHF_ADDR + 0x64] = 0x20;
	gd77_cal_load(ee_read, NULL);
	CHECK(!gd77_cal_at(145000000u, &p) && !p.valid && !p.rx_valid && p.rx_volume == GD77_CAL_DEF_VOLUME);
	CHECK(gd77_cal_at(446000000u, &p) && p.valid);
	// Nothing loaded at all
	memset(eeprom, 0xFF, sizeof eeprom);
	gd77_cal_load(ee_read, NULL);
	CHECK(!gd77_cal_at(446000000u, &p) && !p.valid && !p.rx_valid);
}

static void test_lerp(void)
{
	CHECK(cal_lerp(10, 20, 15, 100, 200) == 150);
	CHECK(cal_lerp(10, 20, 13, 100, 105) == 101);		// 101.5 truncated toward y0
	CHECK(cal_lerp(10, 20, 13, 105, 100) == 104);		// 103.5 truncated toward y0
	CHECK(cal_lerp(10, 20, 5, 100, 200) == 100 && cal_lerp(10, 20, 25, 100, 200) == 200);	// outside: the ends
	CHECK(cal_lerp(10, 20, 10, 7, 9) == 7 && cal_lerp(10, 20, 20, 7, 9) == 9);
	CHECK(cal_lerp(10, 10, 10, 7, 9) == 7 && cal_lerp(20, 10, 15, 7, 9) == 7);	// no span
	CHECK(cal_lerp(0, 65535, 65534, 0, 65535) == 65534);	// the full 16-bit span without overflow
}

int main(void)
{
	test_nodes();
	test_outside();
	test_fields();
	test_invalid();
	test_band_split();
	test_lerp();
	if (failures) {
		printf("gd77CalTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("gd77CalTest: all passed\n");
	return 0;
}
