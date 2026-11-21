// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Power tuning (core/radio/power_tune.c, core/debuglink/debuglink_tune.c) on simulated NOR: offsets interpolated per
// band, the limits, both copies, power cuts during a save, stale and foreign records, names, the preview, the debug link
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "radio/power_tune.h"
#include "radio/radio.h"
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

static uint32_t now_ms = 1000;
uint32_t hal_millis(void) { return now_ms; }
static radio_state_t state = RS_IDLE;
radio_state_t radio_get_state(void) { return state; }

#define FLASH_SIZE	0x20000u
#define TUNE		0x1C000u
static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x2000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x20000u, .tune_addr = TUNE,
};

// Two transmit bands; each level's drive rises 2 per MHz across a band, plus "scale" to fake a new calibration
static const uint16_t base[5] = { 400, 800, 1200, 1800, 2600 };
static int scale;
static uint16_t floor_drive;
static uint32_t uid0 = 0x1234u;

static uint16_t level_drive(uint32_t hz, uint8_t level)
{
	uint32_t lo = hz >= 400000000u ? 400000000u : 136000000u;
	if (level > 4u || (hz < 136000000u || (hz > 174000000u && hz < 400000000u) || hz > 480000000u)) {
		return 0;
	}
	return (uint16_t)(base[level] + (hz - lo) / 500000u + scale);
}

static uint16_t floor_at(uint32_t hz)
{
	(void)hz;
	return floor_drive;
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

static power_tune_result_t put(uint32_t khz, uint8_t level, int16_t delta)
{
	power_tune_edit_t e = { .op = POWER_TUNE_PUT_POINT, .level = level, .delta = delta, .khz = khz, .mw = 1000 };
	return power_tune_edit(&e);
}

static power_tune_result_t label(uint32_t khz, uint8_t level, const char *name, uint32_t mw)
{
	power_tune_edit_t e = { .op = POWER_TUNE_PUT_LABEL, .level = level, .khz = khz, .mw = mw };
	strcpy(e.name, name);
	return power_tune_edit(&e);
}

static int tuned(uint32_t khz, uint8_t level)
{
	uint32_t hz = khz * 1000u;
	return (int)power_tune_drive(hz, level, level_drive(hz, level)) - (int)level_drive(hz, level);
}

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	scale = 0;
	floor_drive = 0;
	uid0 = 0x1234u;
	radio.tx_drive_floor = NULL;
	power_tune_preview(0xFF, 0, now_ms);
	power_tune_init();
}

static void test_points(void)
{
	fresh();
	CHECK(tuned(446000, 2) == 0);
	CHECK(put(446000, 2, 30) == POWER_TUNE_OK);
	CHECK(tuned(446000, 2) == 30);
	CHECK(tuned(470000, 2) == 30);		// flat past the outer point
	CHECK(tuned(146000, 2) == 0);		// the other band
	CHECK(tuned(446000, 1) == 0);		// the other levels
	CHECK(put(430000, 2, 10) == POWER_TUNE_OK);
	CHECK(tuned(438000, 2) == 20);		// halfway
	CHECK(tuned(420000, 2) == 10);
	CHECK(put(430000, 2, -10) == POWER_TUNE_OK);	// replaced, not added
	power_tune_info_t i;
	power_tune_info(&i, now_ms);
	CHECK(i.points == 2 && i.copies == 3 && i.store);
	CHECK(tuned(438000, 2) == 10);
	// A restart reads it back
	power_tune_init();
	CHECK(tuned(438000, 2) == 10);
	power_tune_edit_t del = { .op = POWER_TUNE_DEL_POINT, .level = 2, .khz = 430000 };
	CHECK(power_tune_edit(&del) == POWER_TUNE_OK);
	CHECK(tuned(420000, 2) == 30);
	power_tune_edit_t clr = { .op = POWER_TUNE_CLEAR };
	CHECK(power_tune_edit(&clr) == POWER_TUNE_OK);
	CHECK(tuned(446000, 2) == 0);
	CHECK(put(300000, 2, 5) == POWER_TUNE_BAD);	// no band
	CHECK(put(120000, 2, 5) == POWER_TUNE_BAD);	// receive only
	CHECK(put(446000, 5, 5) == POWER_TUNE_BAD);	// no such level
}

static void test_limits(void)
{
	fresh();
	power_tune_limits_t l;
	power_tune_limits(446000000u, 4, &l);
	CHECK(l.factory == 2600 + 92 && l.hi == l.factory && l.lo == 400 + 92);
	CHECK(put(446000, 4, 1) == POWER_TUNE_BAD);	// the top level only goes down
	CHECK(put(446000, 4, -100) == POWER_TUNE_OK);
	CHECK(put(446000, 0, -1) == POWER_TUNE_BAD);	// the lowest only goes up without the board's floor
	CHECK(put(446000, 0, 50) == POWER_TUNE_OK);
	CHECK(put(446000, 1, 2600 - 800 + 1) == POWER_TUNE_BAD);	// never past the top level's factory drive
	CHECK(put(446000, 1, -401) == POWER_TUNE_BAD);	// never under the lowest's
	CHECK(put(446000, 1, -400) == POWER_TUNE_OK);
	radio.tx_drive_floor = floor_at;
	floor_drive = 300;
	CHECK(put(446000, 0, -192) == POWER_TUNE_OK);	// down to the floor
	CHECK(put(446000, 0, -193) == POWER_TUNE_BAD);
	// A stored offset past the limits of a later calibration is clamped at key-up, never applied past them
	power_tune_preview(3, 4000, now_ms);
	CHECK(power_tune_drive(446000000u, 3, level_drive(446000000u, 3)) == level_drive(446000000u, 4));
	power_tune_preview(3, -4000, now_ms);
	CHECK(power_tune_drive(446000000u, 3, level_drive(446000000u, 3)) == 300);
	CHECK(power_tune_drive(446000000u, 3, 0) == 0);	// a level that doesn't transmit stays off
	power_tune_preview(0xFF, 0, now_ms);
}

static void test_preview(void)
{
	fresh();
	CHECK(put(446000, 2, 30) == POWER_TUNE_OK);
	CHECK(power_tune_preview(2, 60, now_ms));
	CHECK(tuned(446000, 2) == 60);		// in place of the stored offset
	CHECK(tuned(446000, 1) == 0);
	now_ms += POWER_TUNE_PREVIEW_MS;
	CHECK(tuned(446000, 2) == 30);		// lapsed
	CHECK(power_tune_preview(1, 20, now_ms));
	CHECK(tuned(446000, 1) == 20);
	CHECK(power_tune_preview(0xFF, 0, now_ms));
	CHECK(tuned(446000, 1) == 0);
	CHECK(!power_tune_preview(5, 1, now_ms));
}

static void test_copies(void)
{
	fresh();
	CHECK(put(446000, 2, 30) == POWER_TUNE_OK);
	CHECK(put(446000, 3, 40) == POWER_TUNE_OK);
	// One bad byte in either copy: the other one is in force
	sim_flash_data[TUNE + 30] ^= 0x01u;
	power_tune_init();
	CHECK(tuned(446000, 3) == 40);
	sim_flash_data[TUNE + 30] ^= 0x01u;
	sim_flash_data[TUNE + 4096u + 30] ^= 0x01u;
	power_tune_init();
	CHECK(tuned(446000, 3) == 40);
	sim_flash_data[TUNE + 30] ^= 0x01u;	// both bad: factory power, never the bad bytes
	power_tune_init();
	CHECK(tuned(446000, 3) == 0 && tuned(446000, 2) == 0);
	// The next save writes both copies again
	CHECK(put(446000, 1, 5) == POWER_TUNE_OK);
	power_tune_info_t i;
	power_tune_info(&i, now_ms);
	CHECK(i.copies == 3 && i.points == 1);
}

static void test_power_cuts(void)
{
	int torn = 0;
	for (uint32_t cut = 1; cut < 160; cut++) {
		fresh();
		CHECK(put(446000, 2, 30) == POWER_TUNE_OK);
		sim_flash_cut_after(cut);
		power_tune_result_t r = put(446000, 2, 70);
		bool was_cut = sim_flash_is_cut();
		sim_flash_cut_after(0);
		sim_flash_power_on();
		power_tune_init();
		int d = tuned(446000, 2);
		CHECK(d == 30 || d == 70);		// the old record or the new one, never a mix or factory
		if (r == POWER_TUNE_OK) {
			CHECK(d == 70);
		}
		torn += was_cut;
	}
	CHECK(torn > 80);
}

static void test_stale_and_foreign(void)
{
	fresh();
	CHECK(put(446000, 2, 30) == POWER_TUNE_OK);
	CHECK(label(446000, 2, "1.2W", 1200) == POWER_TUNE_OK);
	scale = 1;			// the factory drive moved: the point was tuned against another one
	power_tune_init();
	CHECK(tuned(446000, 2) == 0);
	CHECK(strcmp(power_tune_name(2, 446000000u), "1W") == 0);
	static uint8_t rec[POWER_TUNE_RECORD_MAX];
	size_t n = power_tune_read(rec, sizeof(rec));
	CHECK(n == POWER_TUNE_HDR + POWER_TUNE_POINT_SIZE + POWER_TUNE_LABEL_SIZE);
	CHECK(rec[9] == 1 && rec[10] == 1 && rec[POWER_TUNE_HDR + 5] == POWER_TUNE_STALE);
	scale = 0;
	power_tune_init();
	CHECK(tuned(446000, 2) == 30);
	// Another radio's record (a whole-flash image written to another chip) is never used
	uid0 = 0x9999u;
	power_tune_init();
	CHECK(tuned(446000, 2) == 0);
	CHECK(power_tune_read(rec, sizeof(rec)) == 0);
}

static void test_names(void)
{
	fresh();
	CHECK(label(446000, 0, "50mW", 50) == POWER_TUNE_OK);
	CHECK(strcmp(power_tune_name(0, 440000000u), "50mW") == 0);	// the whole band
	CHECK(strcmp(power_tune_name(0, 146000000u), "100mW") == 0);
	CHECK(strcmp(power_tune_name(1, 440000000u), "500mW") == 0);
	CHECK(label(470000, 0, "40mW", 40) == POWER_TUNE_OK);		// the same band's name replaced
	CHECK(strcmp(power_tune_name(0, 446000000u), "40mW") == 0);
	static uint8_t out[8];
	uint8_t long_req[] = { DL_TUNE_OP_PUT_LABEL, 0x30, 0xCE, 0x06, 0, 0, 0, 0, 0, 0, 'T', 'O', 'O', 'L', 'O', 'N', 'G' };
	CHECK(debuglink_cmd_tune(long_req, sizeof(long_req), out, sizeof(out)) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(label(446000, 0, "", 0) == POWER_TUNE_OK);
	CHECK(strcmp(power_tune_name(0, 446000000u), "100mW") == 0);
	for (uint8_t i = 0; i < POWER_TUNE_POINTS; i++) {
		CHECK(put(400000u + i * 1000u, 1, 1) == POWER_TUNE_OK);
	}
	CHECK(put(479000, 1, 1) == POWER_TUNE_FULL);
	CHECK(label(446000, 0, "T", 0) == POWER_TUNE_OK);		// names have their own room
	power_tune_init();
	CHECK(strcmp(power_tune_name(0, 446000000u), "T") == 0 && tuned(410000, 1) == 1);
}

static void test_debuglink(void)
{
	static uint8_t out[DL_REPLY_MAX];
	fresh();
	uint8_t info_req[] = { DL_TUNE_OP_INFO };
	size_t n = debuglink_cmd_tune(info_req, 1, out, sizeof(out));
	CHECK(out[0] == DL_ST_OK && out[1] == DL_TUNE_VERSION && (out[2] & 1u) && out[8] == POWER_TUNE_POINTS);
	CHECK(out[11] == 5 && out[12] == 2);			// levels, transmit bands
	CHECK(out[13] == 0x40 && out[14] == 0x13 && out[15] == 0x02);	// 136000 kHz
	CHECK(n == 13u + 2u * 8u + 2u * (5u + 5u + 5u + 2u + 4u + 2u));
	uint8_t put_req[] = { DL_TUNE_OP_PUT_POINT, 0x30, 0xCE, 0x06, 0, 2, 25, 0, 0xE8, 3, 0, 0 };	// 446000 kHz
	CHECK(debuglink_cmd_tune(put_req, sizeof(put_req), out, sizeof(out)) == 1 && out[0] == DL_ST_OK);
	CHECK(tuned(446000, 2) == 25);
	state = RS_TX;
	CHECK(debuglink_cmd_tune(put_req, sizeof(put_req), out, sizeof(out)) == 1 && out[0] == DL_ST_REFUSED);
	state = RS_IDLE;
	uint8_t lim_req[] = { DL_TUNE_OP_LIMITS, 0x30, 0xCE, 0x06, 0 };
	n = debuglink_cmd_tune(lim_req, sizeof(lim_req), out, sizeof(out));
	CHECK(out[0] == DL_ST_OK && out[1] == 5);
	CHECK((out[2 + 16] | out[3 + 16] << 8) == 1200 + 92 && (out[4 + 16] | out[5 + 16] << 8) == 1200 + 92 + 25);
	CHECK(out[42] == 5 && memcmp(out + 43, "100mW", 5) == 0);
	uint8_t read_req[] = { DL_TUNE_OP_READ };
	n = debuglink_cmd_tune(read_req, 1, out, sizeof(out));
	CHECK(n == 1u + POWER_TUNE_HDR + POWER_TUNE_POINT_SIZE && out[1 + 9] == 1);
	uint8_t label_req[] = { DL_TUNE_OP_PUT_LABEL, 0x30, 0xCE, 0x06, 0, 2, 0xB0, 4, 0, 0, '1', '.', '2', 'W' };
	CHECK(debuglink_cmd_tune(label_req, sizeof(label_req), out, sizeof(out)) == 1 && out[0] == DL_ST_OK);
	CHECK(strcmp(power_tune_name(2, 446000000u), "1.2W") == 0);
	uint8_t pv_req[] = { DL_TUNE_OP_PREVIEW, 2, 0xF6, 0xFF };	// -10
	CHECK(debuglink_cmd_tune(pv_req, sizeof(pv_req), out, sizeof(out)) == 1 && out[0] == DL_ST_OK);
	CHECK(tuned(446000, 2) == -10);
	uint8_t bad[] = { DL_TUNE_OP_PUT_POINT, 1 };
	CHECK(debuglink_cmd_tune(bad, sizeof(bad), out, sizeof(out)) == 1 && out[0] == DL_ST_BAD_ARG);
	uint8_t clr[] = { DL_TUNE_OP_CLEAR };
	CHECK(debuglink_cmd_tune(clr, 1, out, sizeof(out)) == 1 && out[0] == DL_ST_OK);
	power_tune_preview(0xFF, 0, now_ms);
	CHECK(tuned(446000, 2) == 0);
}

int main(void)
{
	test_points();
	test_limits();
	test_preview();
	test_copies();
	test_power_cuts();
	test_stale_and_foreign();
	test_names();
	test_debuglink();
	sim_flash_free();
	printf("powerTuneTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
