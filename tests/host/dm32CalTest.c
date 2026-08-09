// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV factory calibration (boards/dm32/calibration_dm32.c) on a synthetic factory page in a fake data flash:
// page search, the three pieces, BCD frequency tables, K values, modulation balance, PA drive interpolation and
// power levels, and the fallbacks (docs/spec/hardware-dm32.md 11)
#include <stdio.h>
#include <string.h>
#include "dm32.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- Fake data flash: one factory page and an optional OpenDM32 copy, 0xFF elsewhere ---

#define PAGE_ADDR	0x5000u
#define COPY_ADDR	0x710000u

static uint8_t page[0x1000];
static bool have_page;
static uint8_t copy[DM32_CAL_LEN];
static bool have_copy;

static bool fake_read(uint32_t addr, void *buf, uint32_t len)
{
	uint8_t *b = buf;
	for (uint32_t i = 0; i < len; i++) {
		uint32_t a = addr + i;
		uint8_t v = 0xFF;
		if (have_page && a >= PAGE_ADDR && a < PAGE_ADDR + sizeof page) {
			v = page[a - PAGE_ADDR];
		} else if (have_copy && a >= COPY_ADDR && a < COPY_ADDR + sizeof copy) {
			v = copy[a - COPY_ADDR];
		}
		b[i] = v;
	}
	return true;
}

hal_flash_t dm32_flash = { .size = 16u << 20, .erase_size = 4096, .read = fake_read };

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void build_page(void)
{
	static const uint8_t header[8] = { 0x00, 0x40, 0x00, 0x47, 0xFF, 0xFF, 0xFF, 0xFF };
	static const uint32_t uhf_bcd[5] = { 0x40000000, 0x41750000, 0x43500000, 0x45250000, 0x47000000 };
	static const uint32_t vhf_bcd[5] = { 0x13600000, 0x14550000, 0x15500000, 0x16450000, 0x17400000 };
	memset(page, 0xFF, sizeof page);
	memcpy(page, header, 8);
	page[0x16] = 0x85;				// TX center
	for (int i = 0; i < 5; i++) {
		page[0x17 + i] = 77;			// UHF modulation points
		page[0xA6 + i] = 100;			// VHF modulation points
		page[0x40 + i] = (uint8_t)(10 + i);	// UHF low / mid / high
		page[0x45 + i] = (uint8_t)(50 + i);
		page[0x4A + i] = (uint8_t)(100 + i);
		page[0xD0 + i] = (uint8_t)(20 + i);	// VHF low / mid / high
		page[0xD5 + i] = (uint8_t)(60 + i);
		page[0xDA + i] = (uint8_t)(120 + i);
		put32(&page[0x300 + 0 * 20 + 4 * i], uhf_bcd[i]);
		put32(&page[0x300 + 2 * 20 + 4 * i], vhf_bcd[i]);
	}
	page[0x200] = 0x5A;				// first byte of the +0x200 piece
	page[0xFFF] = 2;				// logical block ID of the calibration page
}

static void test_parse(void)
{
	uint8_t img[DM32_CAL_LEN];
	build_page();
	CHECK(dm32_cal_parse(page, sizeof page, img));
	CHECK(img[0x108] == 0x5A);
	CHECK(get32(&img[0x11D]) == 40000000u);			// table 0, BCD decoded to 10 Hz units
	CHECK(get32(&img[0x11D + 4 * 4]) == 47000000u);
	CHECK(get32(&img[0x145 + 4]) == 14550000u);		// table 2 point 1: 145.5 MHz
	CHECK(get32(&img[0x16D]) == 15480u);			// K_VHF = (136+145+155+164+174) x 100 / 5
	CHECK(get32(&img[0x171]) == 33479u);			// K_UHF = (400+417+435+452+470) x 77 / 5
	CHECK(!dm32_cal_parse(page, 0x200, img));		// too short for the frequency tables
}

static void test_values(void)
{
	dm32_power_points_t p;
	CHECK(dm32_cal_valid());
	CHECK(dm32_cal_tx_center() == 0x85);
	CHECK(dm32_cal_modcal(435000000u) == 0x4D4D);		// 33479 / 435 = 76.96 -> 77
	CHECK(dm32_cal_modcal(145000000u) == 0x6B6B);		// 15480 / 145 = 106.8 -> 107
	CHECK(dm32_cal_power(435000000u, &p) && p.low == 192 && p.mid == 832 && p.high == 1632 && p.very_low == 172);
	CHECK(dm32_cal_power(426250000u, &p) && p.low == 184);	// halfway between points 1 and 2
	CHECK(dm32_cal_power(470000000u, &p) && p.low == 224);	// the last point
	CHECK(dm32_cal_power(480000000u, &p) && p.low == 233);	// past it: 2 v4 - v3, 10 of 17.5 MHz
	CHECK(dm32_cal_power(350000000u, &p) && p.low == 160);	// UHF below 400 MHz: the first point, not the last
	CHECK(dm32_cal_power(145500000u, &p) && p.low == 336 && p.high == 1936);
	CHECK(dm32_cal_power(130000000u, &p) && p.low == 320 && p.mid == 960);	// below 136 MHz: VHF point 0
	CHECK(dm32_cal_power_level(435000000u, 0) == 172);
	CHECK(dm32_cal_power_level(435000000u, 1) == 192);
	CHECK(dm32_cal_power_level(435000000u, 2) == 192 + 640 * 430 / 1000);
	CHECK(dm32_cal_power_level(435000000u, 4) == 832);
	CHECK(dm32_cal_power_level(435000000u, 5) == 832 + 800 * 270 / 1000);
	CHECK(dm32_cal_power_level(435000000u, 8) == 1632);
	CHECK(dm32_cal_power_level(145000000u, 3) > dm32_cal_power_level(145000000u, 2));
	CHECK(dm32_cal_byte(0x16) == 0x85 && dm32_cal_byte(DM32_CAL_LEN) == 0);
}

int main(void)
{
	// Nothing in flash: no calibration, fallbacks
	CHECK(!dm32_cal_load());
	CHECK(strcmp(dm32_cal_source(), "none") == 0);
	CHECK(dm32_cal_tx_center() == 128 && dm32_cal_modcal(145000000u) == 0x4D4D);
	CHECK(dm32_cal_power_level(145000000u, 8) == 1408);
	dm32_power_points_t p;
	CHECK(!dm32_cal_power(145000000u, &p));

	test_parse();
	// Factory page found by its block ID
	have_page = true;
	CHECK(dm32_cal_load() && strcmp(dm32_cal_source(), "factory") == 0);
	test_values();

	// No factory page: OpenDM32's decoded copy, only with the right header
	have_page = false;
	uint8_t img[DM32_CAL_LEN];
	dm32_cal_parse(page, sizeof page, img);
	memcpy(copy, img, sizeof copy);
	have_copy = true;
	CHECK(dm32_cal_load() && strcmp(dm32_cal_source(), "copy") == 0);
	test_values();
	copy[1] = 0x41;
	CHECK(!dm32_cal_load() && strcmp(dm32_cal_source(), "none") == 0);

	// No flash at all
	have_copy = false;
	dm32_flash.size = 0;
	CHECK(!dm32_cal_load());

	if (failures) {
		printf("dm32CalTest: %d failures\n", failures);
		return 1;
	}
	printf("dm32CalTest: all passed\n");
	return 0;
}
