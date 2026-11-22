// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radio tuning (core/radio/radio_tune.c) on simulated NOR: records built as the Radio Tuning page builds them, both
// copies, a write cut before its commit word, stale and foreign records, interpolation per band, the clamps, names and
// the live RAM table
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_tune.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures, checks;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                  \
	} while (0)

void storage_lock(void) { }
void storage_unlock(void) { }
void storage_quick_lock(void) { }
void storage_quick_unlock(void) { }
uint32_t hal_millis(void) { return 1000; }

#define FLASH_SIZE	0x20000u
#define TUNE		0x1C000u
static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x2000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x20000u, .tune_addr = TUNE,
};

// Two transmit bands; each level's drive rises 2 per MHz across a band, plus "scale" to fake a new calibration
static const uint16_t base[5] = { 400, 800, 1200, 1800, 2600 };
static int scale;
static uint32_t uid0 = 0x1234u;

static uint16_t level_drive(uint32_t hz, uint8_t level)
{
	uint32_t lo = hz >= 400000000u ? 400000000u : 136000000u;
	if (level > 4u || (hz < 136000000u || (hz > 174000000u && hz < 400000000u) || hz > 480000000u)) {
		return 0;
	}
	return (uint16_t)(base[level] + (hz - lo) / 500000u + scale);
}

static bool chip(hal_chip_t *c)
{
	c->uid[0] = uid0;
	c->uid[1] = 7;
	c->uid[2] = 9;
	return true;
}

static hal_radio_t radio = { .tx_drive_level = level_drive };
static const hal_system_t sys = { .chip = chip };
static const band_t bands[] = {
	{ 136000000u, 174000000u, true }, { 400000000u, 480000000u, true }, { 108000000u, 135999999u, false },
};
static const char *const names[] = { "100mW", "500mW", "1W", "2.5W", "5W" };
const board_t board = {
	.name = "tune-test", .bands = bands, .band_count = 3, .power_count = 5, .power_names = names,
	.radio = &radio, .system = &sys, .flash = &sim_flash,
};

// --- a record, as the page builds it (docs/spec/radio-tune.md 3) ---

static uint8_t rec[RADIO_TUNE_HDR + RADIO_TUNE_ENTRIES * RADIO_TUNE_ENTRY + 8u];
static uint8_t n_entries;

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

static uint32_t owner(void)
{
	hal_chip_t c;
	memset(&c, 0, sizeof(c));
	chip(&c);
	return storage_crc32(storage_crc32(0, board.name, (uint32_t)strlen(board.name)), c.uid, sizeof(c.uid));
}

static void start(void)
{
	memset(rec, 0, sizeof(rec));
	n_entries = 0;
}

static void trim(uint8_t kind, uint8_t sel, uint32_t khz, int16_t v)
{
	uint8_t *e = rec + RADIO_TUNE_HDR + n_entries++ * RADIO_TUNE_ENTRY;
	e[0] = (uint8_t)(kind << 4 | sel);
	e[1] = 0;
	put16(e + 2, (uint16_t)(khz / 25u));
	put16(e + 4, (uint16_t)v);
	put16(e + 6, kind == RADIO_TUNE_DRIVE ? level_drive(khz * 1000u, sel) : 0u);
}

static void name(uint8_t band, uint8_t level, const char *s)
{
	uint8_t *e = rec + RADIO_TUNE_HDR + n_entries++ * RADIO_TUNE_ENTRY;
	memset(e, 0, RADIO_TUNE_ENTRY);
	e[0] = (uint8_t)(RADIO_TUNE_NAME << 4 | level);
	e[1] = band;
	memcpy(e + 2, s, strlen(s));
}

// Copy (0 A, 1 B) with seq; commit false leaves the commit word erased, as a write cut before its end does
static void write_copy(uint8_t copy, uint32_t seq, bool commit)
{
	uint32_t a = TUNE + copy * STORAGE_SECTOR, n = RADIO_TUNE_HDR + n_entries * RADIO_TUNE_ENTRY;
	static const uint8_t zero[4] = { 0 };
	put32(rec, RADIO_TUNE_MAGIC);
	put32(rec + 4, seq);
	rec[8] = RADIO_TUNE_VERSION;
	rec[9] = n_entries;
	put32(rec + 12, owner());
	put32(rec + n, storage_crc32(0, rec, n));
	CHECK(sim_flash.erase(a) && sim_flash.write(a, rec, n + 4u));
	if (commit) {
		CHECK(sim_flash.write(a + n + 4u, zero, 4u));
	}
}

static int tuned(uint32_t khz, uint8_t level)
{
	uint32_t hz = khz * 1000u;
	return (int)radio_tune_drive(hz, level, level_drive(hz, level)) - (int)level_drive(hz, level);
}

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	scale = 0;
	uid0 = 0x1234u;
	CHECK(radio_tune_load() == 0);
	start();
}

static void test_interpolation(void)
{
	fresh();
	CHECK(tuned(446000, 2) == 0);
	trim(RADIO_TUNE_DRIVE, 2, 430000, 10);
	trim(RADIO_TUNE_DRIVE, 2, 446000, 30);
	write_copy(0, 1, true);
	CHECK(radio_tune_load() == 1);
	CHECK(tuned(446000, 2) == 30);
	CHECK(tuned(470000, 2) == 30);		// flat past the outer trim
	CHECK(tuned(438000, 2) == 20);		// halfway
	CHECK(tuned(420000, 2) == 10);
	CHECK(tuned(146000, 2) == 0);		// the other band
	CHECK(tuned(446000, 1) == 0);		// the other levels
}

static void test_limits(void)
{
	fresh();
	trim(RADIO_TUNE_DRIVE, 4, 446000, 200);		// the top level up: held at its factory drive
	trim(RADIO_TUNE_DRIVE, 0, 446000, -200);	// the lowest level down: held at its factory drive
	trim(RADIO_TUNE_DRIVE, 3, 446000, 4000);	// a middle level: up to the top level's drive
	trim(RADIO_TUNE_DEV, RADIO_MODE_FM, 446000, 900);
	trim(RADIO_TUNE_FREQ, RADIO_MODE_DMR, 446000, -3000);
	write_copy(0, 1, true);
	radio_tune_load();
	CHECK(tuned(446000, 4) == 0);
	CHECK(tuned(446000, 0) == 0);
	CHECK(radio_tune_drive(446000000u, 3, level_drive(446000000u, 3)) == level_drive(446000000u, 4));
	CHECK(radio_tune_dev(RADIO_MODE_FM, 446000000u) == RADIO_TUNE_DEV_MAX);
	CHECK(radio_tune_dev(RADIO_MODE_DMR, 446000000u) == 0);
	CHECK(radio_tune_freq(RADIO_MODE_DMR, 446000000u) == -RADIO_TUNE_FREQ_MAX);
	CHECK(radio_tune_freq(RADIO_MODE_FM, 446000000u) == 0);
	CHECK(radio_tune_drive(446000000u, 2, 0) == 0);	// a level that doesn't transmit there stays off
}

static void test_copies(void)
{
	fresh();
	trim(RADIO_TUNE_DRIVE, 2, 446000, 10);
	write_copy(0, 5, true);
	n_entries = 0;
	trim(RADIO_TUNE_DRIVE, 2, 446000, 20);
	write_copy(1, 6, true);
	CHECK(radio_tune_load() == 3);
	CHECK(tuned(446000, 2) == 20);		// the newer copy
	// A write of A with seq 7 cut before its commit word: B stays in force
	n_entries = 0;
	trim(RADIO_TUNE_DRIVE, 2, 446000, 40);
	write_copy(0, 7, false);
	CHECK(radio_tune_load() == 2);
	CHECK(tuned(446000, 2) == 20);
	// A flipped bit: the copy is ignored
	write_copy(0, 7, true);
	CHECK(radio_tune_load() == 3 && tuned(446000, 2) == 40);
	uint8_t b;
	CHECK(sim_flash.read(TUNE + RADIO_TUNE_HDR + 4u, &b, 1));
	b = (uint8_t)(b & 0xF7u);		// 40 = 0x28: bit 3 cleared, as NOR can
	CHECK(sim_flash.write(TUNE + RADIO_TUNE_HDR + 4u, &b, 1));
	CHECK(radio_tune_load() == 2 && tuned(446000, 2) == 20);
	// The sequence wraps
	n_entries = 0;
	trim(RADIO_TUNE_DRIVE, 2, 446000, 50);
	write_copy(0, 0xFFFFFFFFu, true);
	n_entries = 0;
	trim(RADIO_TUNE_DRIVE, 2, 446000, 60);
	write_copy(1, 1, true);
	CHECK(radio_tune_load() == 3 && tuned(446000, 2) == 60);
}

static void test_foreign_and_stale(void)
{
	fresh();
	trim(RADIO_TUNE_DRIVE, 2, 446000, 10);
	trim(RADIO_TUNE_DEV, RADIO_MODE_FM, 446000, 50);
	write_copy(0, 1, true);
	uid0 = 0x9999u;				// another radio's chip
	CHECK(radio_tune_load() == 0 && tuned(446000, 2) == 0 && radio_tune_dev(RADIO_MODE_FM, 446000000u) == 0);
	uid0 = 0x1234u;
	scale = 5;				// a new factory drive: the drive trim is stale, the rest still holds
	CHECK(radio_tune_load() == 1 && tuned(446000, 2) == 0 && radio_tune_dev(RADIO_MODE_FM, 446000000u) == 50);
	scale = 0;
	CHECK(radio_tune_load() == 1 && tuned(446000, 2) == 10);
	// A trim in no transmit band, a level the radio doesn't have: left out
	start();
	trim(RADIO_TUNE_DRIVE, 6, 446000, 10);
	trim(RADIO_TUNE_DEV, RADIO_MODE_FM, 120000, 10);
	write_copy(0, 2, true);
	uint8_t nw, nn;
	radio_tune_load();
	radio_tune_counts(&nw, &nn);
	CHECK(nw == 0 && nn == 0);
}

static void test_names(void)
{
	fresh();
	CHECK(strcmp(radio_tune_name(4, 446000000u), "5W") == 0);
	name(1, 4, "4.5W");
	name(1, 1, "LowLow");
	name(2, 0, "RX");			// a receive-only band: left out
	name(1, 7, "Bad");			// no such level
	write_copy(0, 1, true);
	radio_tune_load();
	CHECK(strcmp(radio_tune_name(4, 446000000u), "4.5W") == 0);
	CHECK(strcmp(radio_tune_name(1, 446000000u), "LowLow") == 0);
	CHECK(strcmp(radio_tune_name(4, 146000000u), "5W") == 0);	// the other band
	uint8_t nw, nn;
	radio_tune_counts(&nw, &nn);
	CHECK(nn == 2);
}

static void test_live(void)
{
	fresh();
	uint8_t w[8];
	put32(w, RADIO_TUNE_WORD(446000u, RADIO_TUNE_DRIVE, 2u, (uint32_t)-15));
	put32(w + 4, RADIO_TUNE_WORD(146000u, RADIO_TUNE_FREQ, RADIO_MODE_FM, 120u));
	CHECK(radio_tune_set(0, 2, w, 2));
	CHECK(tuned(446000, 2) == -15);
	CHECK(radio_tune_freq(RADIO_MODE_FM, 146000000u) == 120);
	CHECK(!radio_tune_set(0, RADIO_TUNE_WORDS + 1u, w, 2));
	CHECK(!radio_tune_set(1, 2, w, 2));		// past total
	CHECK(radio_tune_set(0, 0, w, 0));		// empty
	CHECK(tuned(446000, 2) == 0);
	// The stored record back
	trim(RADIO_TUNE_DRIVE, 2, 446000, 7);
	write_copy(1, 1, true);
	CHECK(radio_tune_load() == 2 && tuned(446000, 2) == 7);
}

static void test_battery(void)
{
	fresh();
	CHECK(radio_tune_batt(RADIO_TUNE_BATT_GAIN) == 0);
	trim(RADIO_TUNE_BATT, RADIO_TUNE_BATT_GAIN, 0, 21);
	trim(RADIO_TUNE_BATT, RADIO_TUNE_BATT_FULL, 0, -2500);	// past the clamp
	trim(RADIO_TUNE_BATT, 5, 0, 10);				// no such battery trim
	write_copy(0, 1, true);
	uint8_t nw, nn;
	CHECK(radio_tune_load() == 1);
	radio_tune_counts(&nw, &nn);
	CHECK(nw == 2);
	CHECK(radio_tune_batt(RADIO_TUNE_BATT_GAIN) == 21 && radio_tune_batt(RADIO_TUNE_BATT_EMPTY) == 0);
	CHECK(radio_tune_batt(RADIO_TUNE_BATT_FULL) == -RADIO_TUNE_BATT_MAX);
	CHECK(tuned(446000, 2) == 0 && radio_tune_dev(RADIO_MODE_FM, 446000000u) == 0);	// no other trim
	// Live: the page's word
	uint8_t w[4];
	put32(w, RADIO_TUNE_WORD(0u, RADIO_TUNE_W_BATT, RADIO_TUNE_BATT_GAIN, (uint32_t)-400));
	CHECK(radio_tune_set(0, 1, w, 1));
	CHECK(radio_tune_batt(RADIO_TUNE_BATT_GAIN) == -RADIO_TUNE_GAIN_MAX);
}

int main(void)
{
	test_interpolation();
	test_limits();
	test_copies();
	test_foreign_and_stale();
	test_names();
	test_live();
	test_battery();
	printf("radioTuneTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
