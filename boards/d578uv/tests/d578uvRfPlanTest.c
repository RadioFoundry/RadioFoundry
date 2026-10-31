// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT-D578UV's frequency plans outside the RF chips (rf_plan.h, hardware-d578uv.md 5.4, 5.5): the air band LO on
// either side and its VCO sub-band at the stock limits; the PA5 trims' tables from the stock factory defaults, and
// lookups worked out apart from the code, rising and falling tables, at the cells' edges and outside the bands
#include <stdio.h>
#include "rf_plan.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_am_lo(void)
{
	CHECK(am_lo_hz(118000000u, true) == 88750000u);
	CHECK(am_lo_hz(118000000u, false) == 147250000u);
	// The air band's ends with the LO below: sub-band 5 under 85 MHz, 4 above
	CHECK(am_lo_subband(am_lo_hz(108000000u, true)) == 5u);
	CHECK(am_lo_subband(am_lo_hz(114250000u, true)) == 4u);	// LO exactly 85 MHz
	CHECK(am_lo_subband(am_lo_hz(114249990u, true)) == 5u);
	CHECK(am_lo_subband(am_lo_hz(136975000u, true)) == 4u);
	// Above the signal: 4 up to 140 MHz, 3 from there
	CHECK(am_lo_subband(am_lo_hz(110750000u, false)) == 3u);	// LO exactly 140 MHz
	CHECK(am_lo_subband(am_lo_hz(110000000u, false)) == 4u);
	CHECK(am_lo_subband(am_lo_hz(136975000u, false)) == 3u);
}

static void check_table(trim_shape_t sh, const uint8_t p[3], const uint8_t *want, uint8_t n)
{
	for (uint8_t i = 0; i < n; i++) {
		CHECK(trim_cell(sh, p, i) == want[i]);
	}
}

// The stock factory defaults' tables
static void test_trim_tables(void)
{
	static const uint8_t ua[3] = { 0x1E, 0x87, 0xFF }, uc[3] = { 0x1E, 0x91, 0xFF };
	static const uint8_t v[3] = { 0x03, 0x87, 0xFF }, am[3] = { 0x41, 0x6E, 0xB4 };
	static const uint8_t ua_t[13] = { 0x1E, 0x2F, 0x40, 0x51, 0x62, 0x73, 0x87, 0x9B, 0xAF, 0xC3, 0xD7, 0xEB, 0xFF };
	static const uint8_t uc_t[13] = { 0x1E, 0x31, 0x44, 0x57, 0x6A, 0x7D, 0x91, 0xA3, 0xB5, 0xC7, 0xD9, 0xEB, 0xFF };
	static const uint8_t v_t[45] = {
		0x03, 0x06, 0x09, 0x0C, 0x0F, 0x12, 0x15, 0x18, 0x1B, 0x1E, 0x21, 0x24, 0x27, 0x2A, 0x2D, 0x30, 0x33, 0x36,
		0x39, 0x3C, 0x3F, 0x42, 0x45, 0x48, 0x4B, 0x4E, 0x51, 0x54, 0x57, 0x5A, 0x5D, 0x60, 0x63, 0x66, 0x69, 0x6C,
		0x6F, 0x72, 0x87, 0xF5, 0xF7, 0xF9, 0xFB, 0xFD, 0xFF,
	};
	static const uint8_t am_t[38] = {
		0x41, 0x43, 0x45, 0x47, 0x49, 0x4B, 0x4D, 0x4F, 0x51, 0x53, 0x55, 0x57, 0x59, 0x5B, 0x5D, 0x5F, 0x61, 0x63,
		0x65, 0x6E, 0x81, 0x84, 0x87, 0x8A, 0x8D, 0x90, 0x93, 0x96, 0x99, 0x9C, 0x9F, 0xA2, 0xA5, 0xA8, 0xAB, 0xAE,
		0xB1, 0xB4,
	};
	check_table(TRIM_UHF, ua, ua_t, 13);
	check_table(TRIM_UHF, uc, uc_t, 13);
	check_table(TRIM_VHF, v, v_t, 45);
	check_table(TRIM_AM, am, am_t, 38);
}

static void test_trim_lookup(void)
{
	static const struct {
		char kind;	// U UHF, V VHF 136 MHz cells, H VHF 220 MHz cells, A air band
		uint8_t p[3];
		uint32_t f;	// 10 Hz units
		uint8_t want;
	} c[] = {
		{ 'U', { 0x1E, 0x87, 0xFF }, 43537500u, 0x59 }, { 'U', { 0x1E, 0x87, 0xFF }, 46250000u, 0x8B },
		{ 'U', { 0x1E, 0x87, 0xFF }, 40000000u, 0x1E }, { 'U', { 0x1E, 0x87, 0xFF }, 51999990u, 0xFD },
		{ 'U', { 0x1E, 0x87, 0xFF }, 30000010u, 0xFF },	// below 400 MHz: the index wraps, the last cell
		{ 'U', { 0xFF, 0x87, 0x1E }, 40450000u, 0x90 },	// falling: no interpolation down
		{ 'U', { 0xFF, 0x87, 0x1E }, 44000000u, 0xAF }, { 'U', { 0xFF, 0x87, 0x1E }, 47530000u, 0x07 },
		{ 'V', { 0x03, 0x87, 0xFF }, 13600000u, 0x03 }, { 'V', { 0x03, 0x87, 0xFF }, 14552500u, 0x1F },
		{ 'V', { 0x03, 0x87, 0xFF }, 17395000u, 0x78 }, { 'V', { 0x03, 0x87, 0xFF }, 17400000u, 0x87 },
		{ 'V', { 0x03, 0x87, 0xFF }, 15000000u, 0x2D },
		{ 'H', { 0x03, 0x87, 0xFF }, 22000000u, 0xF5 }, { 'H', { 0x03, 0x87, 0xFF }, 22255000u, 0xFA },
		{ 'H', { 0x03, 0x87, 0xFF }, 22500000u, 0xFF },
		{ 'A', { 0x41, 0x6E, 0xB4 }, 10800000u, 0x41 }, { 'A', { 0x41, 0x6E, 0xB4 }, 11835000u, 0x55 },
		{ 'A', { 0x41, 0x6E, 0xB4 }, 12700000u, 0x6E }, { 'A', { 0x41, 0x6E, 0xB4 }, 13697500u, 0x99 },
		{ 'A', { 0x41, 0x6E, 0xB4 }, 14550000u, 0xB4 },
		{ 'A', { 0xB4, 0x30, 0x10 }, 11005000u, 0xA8 }, { 'A', { 0xB4, 0x30, 0x10 }, 13333000u, 0xB5 },
	};
	for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		uint8_t v = c[i].kind == 'A' ? trim_am(c[i].p, c[i].f) : trim_lookup(c[i].p, c[i].f, c[i].kind == 'H');
		if (v != c[i].want) {
			printf("case %u: %c %lu got 0x%02X want 0x%02X\n", (unsigned)i, c[i].kind, (unsigned long)c[i].f, v,
			       c[i].want);
		}
		CHECK(v == c[i].want);
	}
}

static void test_trim_tx(void)
{
	CHECK(trim_tx(F10(435), 0, 0x96, 0x91) == 0x78);
	CHECK(trim_tx(F10(435), 1, 0x96, 0x91) == 0x78);
	CHECK(trim_tx(F10(435), 2, 0x96, 0x91) == 0x82);
	CHECK(trim_tx(F10(435), 3, 0x96, 0x91) == 0x96);
	CHECK(trim_tx(F10(300), 3, 0x96, 0x91) == 0x96);	// 300 MHz is UHF here
	CHECK(trim_tx(F10(145), 2, 0x96, 0x91) == 0x78);
	CHECK(trim_tx(F10(145), 3, 0x96, 0x91) == 0x91);
}

int main(void)
{
	test_am_lo();
	test_trim_tables();
	test_trim_lookup();
	test_trim_tx();
	if (failures) {
		printf("d578uvRfPlanTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("d578uvRfPlanTest: all passed\n");
	return 0;
}
