// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's command handlers (core/debuglink/debuglink.c), taken whole so its static handlers are reachable:
// the RADIO dispatcher hands every radio feature operation to debuglink_radio_feature_op, and STORAGE TUNE_WRITE
// never programs over data in the tune area it hasn't checked and erased. What these paths don't reach is in
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
#define TUNE		0x1C000u
static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x2000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x20000u, .tune_addr = TUNE,
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

// --- STORAGE TUNE_WRITE ---

static size_t tune_write(uint8_t copy, uint16_t off, const uint8_t *data, uint16_t n)
{
	uint8_t req[4 + 64];
	req[0] = DL_STORAGE_OP_TUNE_WRITE;
	req[1] = copy;
	put16(req + 2, off);
	memcpy(req + 4, data, n);
	return cmd_storage(req, (uint16_t)(4u + n), out, sizeof(out));
}

static void tune_load(void)
{
	uint8_t req[1] = { DL_STORAGE_OP_TUNE_LOAD };
	CHECK(cmd_storage(req, 1, out, sizeof(out)) == 2u && out[0] == DL_ST_OK);
}

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	tune_load();		// a new session: no copy erased yet
}

static bool refused(size_t n, uint8_t reason)
{
	return n == 2u && out[0] == DL_ST_REFUSED && out[1] == reason;
}

static void test_tune_write(void)
{
	uint8_t chunk[60], back[60];
	memset(chunk, 0x5A, sizeof(chunk));
	put32(chunk, RADIO_TUNE_MAGIC);		// a tune copy's start

	// A save: offset 0 erases, the next chunks follow, into both copies
	fresh();
	for (uint8_t copy = 0; copy < 2u; copy++) {
		uint32_t a = TUNE + copy * SIM_FLASH_SECTOR;
		CHECK(tune_write(copy, 0, chunk, 60) == 1u && out[0] == DL_ST_OK);
		CHECK(tune_write(copy, 60, chunk + 4, 40) == 1u && out[0] == DL_ST_OK);
		CHECK(sim_flash.read(a + 60u, back, 40) && memcmp(back, chunk + 4, 40) == 0);
	}
	CHECK(sim_flash_violations() == 0);

	// Another firmware's data in the area: offset 0 is refused and erases nothing, and so is every later offset
	fresh();
	uint8_t other[4] = { 'C', 'P', 0x01, 0x00 };
	CHECK(sim_flash.write(TUNE + SIM_FLASH_SECTOR + 100u, other, sizeof(other)));
	CHECK(refused(tune_write(0, 0, chunk, 60), DL_TUNE_REFUSED_FOREIGN));
	CHECK(refused(tune_write(1, 100, chunk, 4), DL_TUNE_REFUSED_NOT_ERASED));
	CHECK(refused(tune_write(0, 60, chunk, 4), DL_TUNE_REFUSED_NOT_ERASED));
	CHECK(sim_flash.read(TUNE + SIM_FLASH_SECTOR + 100u, back, 4) && memcmp(back, other, 4) == 0);
	CHECK(sim_flash_violations() == 0);

	// A later offset without its own copy's offset-0 write: copy A erased does not open copy B
	fresh();
	CHECK(tune_write(0, 0, chunk, 60) == 1u && out[0] == DL_ST_OK);
	CHECK(refused(tune_write(1, 60, chunk, 4), DL_TUNE_REFUSED_NOT_ERASED));
	// TUNE_LOAD ends the save: a later chunk needs offset 0 again
	tune_load();
	CHECK(refused(tune_write(0, 60, chunk, 4), DL_TUNE_REFUSED_NOT_ERASED));
	// Refused while the radio transmits, before anything else
	state = RS_TX;
	CHECK(refused(tune_write(0, 0, chunk, 60), DL_TUNE_REFUSED_TX));
	state = RS_IDLE;
	// A refused offset 0 closes the copy's later offsets too, though an earlier offset 0 erased it
	fresh();
	CHECK(tune_write(0, 0, chunk, 60) == 1u && out[0] == DL_ST_OK);
	CHECK(sim_flash.write(TUNE + SIM_FLASH_SECTOR, other, sizeof(other)));
	CHECK(refused(tune_write(0, 0, chunk, 60), DL_TUNE_REFUSED_FOREIGN));
	CHECK(refused(tune_write(0, 60, chunk, 4), DL_TUNE_REFUSED_NOT_ERASED));
	CHECK(sim_flash_violations() == 0);
}

int main(void)
{
	test_feature_ops();
	test_tune_write();
	printf("debuglinkCmdTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
