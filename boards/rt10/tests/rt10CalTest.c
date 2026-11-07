// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 factory calibration (boards/rt10/calibration_rt10.c) on a synthetic sector: the frequency steps and their
// edges, the high and low TX plans, the PA gain limits, unfilled entries, an erased or foreign sector, a failed read
// (docs/spec/hardware-rt10.md 4)
#include <stdio.h>
#include <string.h>
#include "rt10.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define SECTOR		0x1FF000u
#define SECTOR_LEN	0x1000u

static uint8_t sector[SECTOR_LEN];
static bool read_fails;

static bool read_sector(uint32_t addr, void *buf, uint32_t len)
{
	if (read_fails || addr < SECTOR || addr + len > SECTOR + SECTOR_LEN) {
		return false;
	}
	memcpy(buf, sector + (addr - SECTOR), len);
	return true;
}

// Fifteen entries at off: base + step x entry
static void table(uint32_t off, uint8_t base, int8_t step)
{
	for (int i = 0; i < 15; i++) {
		sector[off + i] = (uint8_t)(base + step * i);
	}
}

static void build(void)
{
	memset(sector, 0xFF, sizeof(sector));
	sector[0x470] = 90;		// 900 MHz is step 0
	sector[0x1A9] = 0x6D;		// TX center
	table(0x51D, 0x60, 1);		// DAC high
	table(0x52C, 0x40, 1);		// DAC low
	table(0x5B5, 5, 1);		// PA high: 5, 6, 7, then over the limit
	table(0x5C4, 3, 0);		// PA low
	table(0x5D3, 20, 1);		// key-up delay high, / 10 us
	table(0x5F1, 10, 1);		// key-up delay low
	table(0x600, 4, 0);		// key-down delay
	for (int i = 0; i < 15; i++) {
		sector[0x559 + 2 * i] = (uint8_t)(0x30 + i);	// the modulation byte
		sector[0x55A + 2 * i] = 0xEE;			// its pair's second byte, not used
	}
}

static void test_steps(void)
{
	build();
	CHECK(rt10_cal_load(read_sector));
	CHECK(rt10_cal_valid());
	CHECK(rt10_cal_step(902000000u) == 0);
	CHECK(rt10_cal_step(909999999u) == 0);
	CHECK(rt10_cal_step(910000000u) == 1);
	CHECK(rt10_cal_step(927500000u) == 2);
	CHECK(rt10_cal_step(850000000u) == 0);		// below the base: the first entry
	CHECK(rt10_cal_step(1200000000u) == 14);	// past the table: the last
	CHECK(rt10_cal_tx_center() == 0x6D);
	CHECK(rt10_cal_mod(927500000u) == 0x32);
}

static void test_plans(void)
{
	build();
	CHECK(rt10_cal_load(read_sector));
	rt10_tx_plan_t p;
	CHECK(rt10_cal_tx_plan(905000000u, true, &p));
	CHECK(p.dac == 0x600 && p.pa_gain == 5 && p.on_us == 200 && p.off_us == 40);
	CHECK(rt10_cal_tx_plan(927500000u, false, &p));
	CHECK(p.dac == 0x420 && p.pa_gain == 3 && p.on_us == 120 && p.off_us == 40);
	// The high set's gain stops at 7
	CHECK(rt10_cal_tx_plan(927500000u, true, &p) && p.pa_gain == 7);
	sector[0x5B5 + 2] = 12;
	CHECK(rt10_cal_load(read_sector));
	CHECK(rt10_cal_tx_plan(927500000u, true, &p) && p.pa_gain == 7);
	// The low set's is the field's three bits
	sector[0x5C4 + 2] = 0x0B;
	CHECK(rt10_cal_load(read_sector));
	CHECK(rt10_cal_tx_plan(927500000u, false, &p) && p.pa_gain == 3);
	// An unfilled delay is none; an unfilled level or gain refuses the step only
	sector[0x5F1 + 2] = 0xFF;
	sector[0x600 + 2] = 0xFF;
	sector[0x52C + 1] = 0xFF;
	CHECK(rt10_cal_load(read_sector));
	CHECK(rt10_cal_tx_plan(927500000u, false, &p) && p.on_us == 0 && p.off_us == 0);
	CHECK(!rt10_cal_tx_plan(915000000u, false, &p));
	CHECK(rt10_cal_tx_plan(915000000u, true, &p));
	sector[0x5B5 + 1] = 0xFF;
	CHECK(rt10_cal_load(read_sector));
	CHECK(!rt10_cal_tx_plan(915000000u, true, &p));
	// An unfilled TX center or modulation byte: the defaults
	sector[0x1A9] = 0xFF;
	sector[0x559 + 4] = 0xFF;
	CHECK(rt10_cal_load(read_sector));
	CHECK(rt10_cal_tx_center() == RT10_TX_CENTER_DEFAULT);
	CHECK(rt10_cal_mod(927500000u) == 0);
}

static void test_no_calibration(void)
{
	rt10_tx_plan_t p;
	// Erased
	memset(sector, 0xFF, sizeof(sector));
	CHECK(!rt10_cal_load(read_sector));
	CHECK(!rt10_cal_valid());
	CHECK(!rt10_cal_tx_plan(927500000u, false, &p));
	CHECK(rt10_cal_tx_center() == RT10_TX_CENTER_DEFAULT);
	CHECK(rt10_cal_mod(927500000u) == 0);
	// A base outside 800-950 MHz: another layout
	build();
	sector[0x470] = 40;
	CHECK(!rt10_cal_load(read_sector));
	sector[0x470] = RT10_CAL_BASE_MAX + 1;
	CHECK(!rt10_cal_load(read_sector));
	sector[0x470] = RT10_CAL_BASE_MIN;
	CHECK(rt10_cal_load(read_sector));
	// A read that fails, after a good load: nothing left valid
	build();
	CHECK(rt10_cal_load(read_sector));
	read_fails = true;
	CHECK(!rt10_cal_load(read_sector));
	read_fails = false;
	CHECK(!rt10_cal_valid());
	CHECK(!rt10_cal_tx_plan(927500000u, false, &p));
}

// DMR burst timing (hardware-rt10.md 7.1): the shortest lead that has the PA up (2.7 ms + the plan's key-up delay)
// when the first symbol goes out 0.85 ms after the lead
static void test_dmr_lead(void)
{
	CHECK(rt10_dmr_rf_tx_lead(450) == 25);		// the low set's 450 us key-up delay: 2.5 ms, not the stock's 2.6
	CHECK(rt10_dmr_rf_tx_lead(100) == 22);
	CHECK(rt10_dmr_rf_tx_lead(1000) == 31);
	for (uint16_t on = 0; on <= 2550; on += 10) {	// the calibration's byte x 10 us
		uint8_t lead = rt10_dmr_rf_tx_lead(on);
		CHECK(lead * 100u + HRC_RFBURST_SYMBOL_AFTER_LEAD_US >= RT10_DMR_PA_UP_US + on);	// PA up in time
		CHECK(lead * 100u + HRC_RFBURST_SYMBOL_AFTER_LEAD_US < RT10_DMR_PA_UP_US + on + 100u);	// and no earlier
		CHECK(lead <= 0x3Fu);		// fits RF_CONTROL b13:8
	}
}

int main(void)
{
	test_steps();
	test_plans();
	test_no_calibration();
	test_dmr_lead();
	if (failures) {
		printf("rt10CalTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("rt10CalTest: all passed\n");
	return 0;
}
