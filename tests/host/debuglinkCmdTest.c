// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's command handlers (core/debuglink/debuglink.c), taken whole so its static handlers are reachable:
// the RADIO dispatcher hands every radio feature operation to debuglink_radio_feature_op, and STORAGE's radio tuning
// operations put, read back, save, load and clear the trims in the settings ring. What these paths don't reach is in
// sim/debuglink_unreached.c
#include <stdio.h>
#include <string.h>
#include "debuglink/debuglink.c"
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

// --- what the tested paths call ---

static uint32_t now_ms = 1000;
static radio_state_t state = RS_IDLE;
static int feature_calls;
static uint8_t feature_op;
static uint16_t feature_len;

uint32_t hal_millis(void) { return now_ms; }
radio_state_t radio_get_state(void) { return state; }
void storage_lock(void) { }
void storage_unlock(void) { }
void storage_quick_lock(void) { }
void storage_quick_unlock(void) { }
radio_view_t hostlink_view;
const char build_preset[] = "test";
const char build_off[] = "";

uint8_t debuglink_radio_feature_op(uint8_t op, const uint8_t *args, uint16_t n)
{
	feature_calls++;
	feature_op = op;
	feature_len = n;
	return DL_ST_OK;
}

#define FLASH_SIZE	0x20000u
static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x2000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x20000u,
};
static const band_t bands[] = { { 400000000u, 480000000u, true } };
static const char *const names[] = { "1W", "5W" };
static hal_radio_t radio;
const board_t board = {
	.name = "dl-test", .bands = bands, .band_count = 1, .power_count = 2, .power_names = names,
	.radio = &radio, .flash = &sim_flash,
};

static uint8_t out[DL_REPLY_MAX + 2u];

// --- RADIO ---

static uint8_t radio_op(const uint8_t *req, uint16_t len)
{
	now_ms += RADIO_OP_GAP_MS;	// past the rate limit
	feature_calls = 0;
	CHECK(cmd_radio(req, len, out) >= 1u);
	return out[0];
}

// Every DL_OP_* reaches debuglink_radio_feature_op with its arguments, not the fixed-length table's BAD_ARG
static void test_feature_ops(void)
{
	static const uint8_t ops[] = {
		DL_OP_SCAN, DL_OP_SCAN_VFO, DL_OP_BEEP, DL_OP_TONE_BURST, DL_OP_SETTING, DL_OP_ROAM, DL_OP_VIBRATE,
		DL_OP_VOLUME, DL_OP_DMR_FIELDS, DL_OP_VFO_COPY, DL_OP_VOICE_TEST, DL_OP_DTMF, DL_OP_TX_LIMIT,
		DL_OP_TX_DRIVE, DL_OP_PA_KEYUP, DL_OP_VFO_SFR, DL_OP_PA_SWEEP, DL_OP_PA_KEYRUN, DL_OP_VOICE_CAPTURE,
		DL_OP_P25, DL_OP_NXDN, DL_OP_TUNE_SET,
	};
	for (unsigned i = 0; i < sizeof(ops); i++) {
		uint8_t req[1] = { ops[i] };
		CHECK(radio_op(req, 1) == DL_ST_OK && feature_calls == 1 && feature_op == ops[i]);
		if (feature_calls != 1) {
			printf("  op %u not handed to the radio features\n", ops[i]);
		}
	}
	// TUNE_SET as the Radio Tuning page sends it: first, total, two words
	uint8_t set[1 + 2 + 8] = { DL_OP_TUNE_SET, 0, 2 };
	CHECK(radio_op(set, sizeof(set)) == DL_ST_OK && feature_calls == 1 && feature_op == DL_OP_TUNE_SET &&
	      feature_len == 10u);
	// An op no one handles stays a bad argument
	uint8_t none[1] = { 200 };
	CHECK(radio_op(none, 1) == DL_ST_BAD_ARG && feature_calls == 0);
}

// --- STORAGE radio tuning ---

static size_t storage_op(const uint8_t *req, uint16_t len)
{
	return cmd_storage(req, len, out, sizeof(out));
}

// op with no argument: OK and the entries in the table
static bool tune_op(uint8_t op, uint8_t entries)
{
	uint8_t req[1] = { op };
	return storage_op(req, 1) == 2u && out[0] == DL_ST_OK && out[1] == entries;
}

static void boot(void)
{
	sim_flash_power_on();
	CHECK(storage_init());
	settings_init();
	radio_tune_load();
}

// A frequency trim (needs no calibration): FM, khz, value
static void freq_entry(uint8_t *e, uint32_t khz, int16_t v)
{
	memset(e, 0, RADIO_TUNE_ENTRY);
	e[0] = (uint8_t)(RADIO_TUNE_FREQ << 4 | RADIO_MODE_FM);
	put16(e + 2, (uint16_t)(khz / 25u));
	put16(e + 4, (uint16_t)v);
}

static void test_tune(void)
{
	uint8_t req[2 + 7u * RADIO_TUNE_ENTRY];
	sim_flash_free();
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(tune_op(DL_STORAGE_OP_TUNE_LOAD, 0));

	// 9 entries in two requests (the second without the start flag), live at once
	req[0] = DL_STORAGE_OP_TUNE_PUT;
	req[1] = 1;
	for (uint8_t i = 0; i < 7u; i++) {
		freq_entry(req + 2 + i * RADIO_TUNE_ENTRY, 401000u + i * 1000u, (int16_t)(10 + i));
	}
	CHECK(storage_op(req, sizeof(req)) == 2u && out[0] == DL_ST_OK && out[1] == 7u);
	req[1] = 0;
	freq_entry(req + 2, 430000u, 100);
	freq_entry(req + 2 + RADIO_TUNE_ENTRY, 431000u, 101);
	CHECK(storage_op(req, 2u + 2u * RADIO_TUNE_ENTRY) == 2u && out[1] == 9u);
	CHECK(radio_tune_freq(RADIO_MODE_FM, 430000000u) == 100);
	CHECK(storage_op(req, 3) == 1u && out[0] == DL_ST_BAD_ARG);		// not whole entries
	// Read back from entry 7: the last two, as put
	uint8_t get[2] = { DL_STORAGE_OP_TUNE_GET, 7 };
	CHECK(storage_op(get, 2) == 2u + 2u * RADIO_TUNE_ENTRY && out[0] == DL_ST_OK && out[1] == 9u &&
	      memcmp(out + 2, req + 2, 2u * RADIO_TUNE_ENTRY) == 0);
	get[1] = 0;
	CHECK(storage_op(get, 2) == 2u + 7u * RADIO_TUNE_ENTRY);		// at most 7 a reply
	get[1] = 9;
	CHECK(storage_op(get, 2) == 2u && out[1] == 9u);

	// Not saved: a restart has none; saved: a restart has all 9
	boot();
	CHECK(radio_tune_freq(RADIO_MODE_FM, 430000000u) == 0);
	req[1] = 1;
	CHECK(storage_op(req, sizeof(req)) == 2u && out[1] == 7u);
	req[1] = 0;
	CHECK(storage_op(req, 2u + 2u * RADIO_TUNE_ENTRY) == 2u && out[1] == 9u);
	CHECK(tune_op(DL_STORAGE_OP_TUNE_SAVE, 9));
	boot();
	get[1] = 9;
	CHECK(radio_tune_freq(RADIO_MODE_FM, 430000000u) == 100 && storage_op(get, 2) == 2u && out[1] == 9u);

	// A live change undone by LOAD
	req[1] = 1;
	freq_entry(req + 2, 430000u, -5);
	CHECK(storage_op(req, 2u + RADIO_TUNE_ENTRY) == 2u && out[1] == 1u && radio_tune_freq(RADIO_MODE_FM, 430000000u) == -5);
	CHECK(tune_op(DL_STORAGE_OP_TUNE_LOAD, 9) && radio_tune_freq(RADIO_MODE_FM, 430000000u) == 100);

	// CLEAR: none now, none after a restart
	CHECK(tune_op(DL_STORAGE_OP_TUNE_CLEAR, 0) && radio_tune_freq(RADIO_MODE_FM, 430000000u) == 0);
	boot();
	CHECK(tune_op(DL_STORAGE_OP_TUNE_LOAD, 0));
	CHECK(sim_flash_violations() == 0);

	// No settings ring: nothing to keep a record in
	sim_flash.map = NULL;
	CHECK(!storage_init());
	settings_init();
	CHECK(storage_op(get, 2) == 1u && out[0] == DL_ST_UNAVAILABLE);
	sim_flash.map = &map;
}

int main(void)
{
	test_feature_ops();
	test_tune();
	printf("debuglinkCmdTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
