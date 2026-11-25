// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The emergency wipe (core/zeroize, docs/spec/zeroize.md) on a simulated NOR flash with the real storage, settings
// store and codeplug: the code's hash, the key sequence, and whole wipes in both scopes. After a wipe no copy of a key
// is anywhere in the flash (old settings records, the journal's copy, the staging area), the sectors erased are
// exactly the expected ones and every other sector (the calibration window, the DMR ID database, the voice prompts,
// the custom data) is byte for byte as before; the RAM copies are cleared, the key fill hook runs, failures are
// reported and don't stop the other steps
#define _GNU_SOURCE	// memmem
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zeroize/zeroize.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "radio/radio.h"
#include "hal_time.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

// Time: hal_delay_ms moves the clock (the wipe waits for the radio core)
static uint32_t now_ms;
uint32_t hal_millis(void) { return now_ms; }
void hal_delay_ms(uint32_t ms) { now_ms += ms; }

// The radio core: RREQ_ZEROIZE answered at once, unless radio_stuck
static int radio_zeroizes;
static bool radio_stuck;
void radio_req_zeroize(void) { radio_zeroizes++; }
bool radio_zeroize_busy(void) { return radio_stuck; }

// What other features hold: key fill's hook, the crash record
static int hook_calls, crash_clears;
void zeroize_ram_hook(void) { hook_calls++; }
void crash_clear(void) { crash_clears++; }

#define FLASH_SIZE	0x400000u
#define EE		0x000000u
#define FL		0x020000u
#define CAL		0x010000u
#define SETTINGS	0x011000u
#define SETTINGS_SIZE	0x008000u
#define JOURNAL		0x019000u
#define NATIVE		0x300000u
#define NATIVE_SIZE	0x040000u
#define STAGING		0x340000u
#define STAGING_SIZE	0x040000u
#define SECTOR		STORAGE_SECTOR

static const hal_flash_map_t map = {
	.settings_addr = SETTINGS, .settings_size = SETTINGS_SIZE, .journal_addr = JOURNAL,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = NATIVE,
	.calibration_addr = CAL, .calibration_size = 0x200u,
	.native_addr = NATIVE, .native_size = NATIVE_SIZE, .staging_addr = STAGING, .staging_size = STAGING_SIZE,
};

static uint32_t uid[3] = { 0x11223344u, 0x55667788u, 0x99AABBCCu };
static bool chip(hal_chip_t *c)
{
	memset(c, 0, sizeof(*c));
	memcpy(c->uid, uid, sizeof(uid));
	return true;
}
static const hal_system_t sys = { .chip = chip };
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash,
			.system = &sys };

// Secrets: a traffic key and a system secret in the codeplug settings area, a privacy key in the settings store
static const uint8_t traffic_key[32] = "TRAFFIC-KEY-0123456789abcdefghij";
static const uint8_t sys_secret[16] = "SYSTEM-SECRET-XY";
#define PRIVACY_KEY	0x4B3Cu

static bool found(const void *pat, size_t n)
{
	for (uint32_t a = 0; a + n <= FLASH_SIZE; a++) {
		if (sim_flash_data[a] == ((const uint8_t *)pat)[0] && memcmp(&sim_flash_data[a], pat, n) == 0) {
			if (getenv("ZZ_DEBUG"))
				printf("  found at 0x%06x\n", (unsigned)a);
			return true;
		}
	}
	return false;
}

static bool blank(uint32_t addr, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		if (sim_flash_data[addr + i] != 0xFF) {
			return false;
		}
	}
	return true;
}

// Valid settings records whose page 0 holds a privacy key
static int records_with_key(void)
{
	int n = 0;
	for (uint32_t a = SETTINGS; a < SETTINGS + SETTINGS_SIZE; a += STORAGE_SLOT_SIZE) {
		const uint8_t *r = &sim_flash_data[a];
		if (storage_slot_check(r) != STORAGE_SLOT_VALID || (r[9] >> 4) != 0 || (r[10] | r[11] << 8) == 0) {
			continue;
		}
		const uint8_t *k = &r[STORAGE_SLOT_HDR + offsetof(settings_t, privacy_key)];
		for (unsigned i = 0; i < sizeof(((settings_t *)0)->privacy_key); i++) {
			if (k[i] != 0) {
				n++;
				break;
			}
		}
	}
	return n;
}

static uint8_t *before;		// the flash before the wipe

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	settings_init();
	codeplug_init();
	radio_zeroizes = hook_calls = crash_clears = 0;
	radio_stuck = false;
}

// A radio in use: what must survive outside the codeplug, a stored codeplug with keys, a privacy key saved many times
static void populate(void)
{
	memset(&sim_flash_data[CAL], 0xCA, 0x200);				// the calibration window
	memcpy(&sim_flash_data[FL], "CUSTDATA", 8);				// custom data (boot image, melody)
	memcpy(&sim_flash_data[FL + 0x30000u], "IdN\x59" "001", 7);		// the DMR ID database
	memcpy(&sim_flash_data[FL + 0x8F400u], "VP\0\0", 4);			// the voice prompts, the next sector
	memcpy(&sim_flash_data[FL + 0x7A000u], "below", 5);			// just below the flash codeplug
	channel_t ch;
	CHECK(codeplug_channel(1, &ch));
	strcpy(ch.name, "Secret net");
	CHECK(codeplug_set_channel(5, &ch));					// a stored codeplug
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 3100999));
	uint8_t rec[5 + sizeof(traffic_key)] = { 0, 1, 1, 0, 0 };
	memcpy(&rec[5], traffic_key, sizeof(traffic_key));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_TRAFFIC + 3u, rec, sizeof(rec)));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY, sys_secret, sizeof(sys_secret)));	// last: J1 holds the area
	CHECK(zeroize_code_set("2468"));
	for (int i = 0; i < 12; i++) {
		settings_t *st = settings_begin();
		st->privacy_type[0] = 2;
		st->privacy_key[0] = PRIVACY_KEY;
		st->privacy_active = 1;
		st->brightness = (uint8_t)(40 + i);
		settings_end(hal_millis());
		CHECK(settings_flush());
	}
	memcpy(&sim_flash_data[STAGING + 0x1E000u + 64u], traffic_key, sizeof(traffic_key));	// a staged image's area
	// Nothing native written yet (it is ours with its magic): one sector with it, to see it emptied
	memcpy(&sim_flash_data[NATIVE], "ODRN", 4);
	memcpy(&sim_flash_data[NATIVE + 0x1000u], "native data", 11);
	// The secrets are where the wipe must find them
	CHECK(found(traffic_key, sizeof(traffic_key)) && found(sys_secret, sizeof(sys_secret)));
	CHECK(memmem(&sim_flash_data[JOURNAL + SECTOR], SECTOR, sys_secret, sizeof(sys_secret)) != NULL);	// J1
	CHECK(records_with_key() >= 12);
	memcpy(before, sim_flash_data, FLASH_SIZE);
}

// Every sector outside the given ranges as before
static bool unchanged_except(const uint32_t (*r)[2], unsigned n)
{
	bool ok = true;
	for (uint32_t a = 0; a < FLASH_SIZE; a += SECTOR) {
		bool skip = false;
		for (unsigned i = 0; i < n; i++) {
			skip = skip || (a >= r[i][0] && a < r[i][0] + r[i][1]);
		}
		if (!skip && memcmp(&sim_flash_data[a], &before[a], SECTOR) != 0) {
			printf("  sector 0x%06x changed\n", (unsigned)a);
			ok = false;
		}
	}
	return ok;
}

static void test_hash(void)
{
	fresh();
	uint8_t a[ZEROIZE_HASH_LEN], b[ZEROIZE_HASH_LEN];
	zeroize_hash(1, "1234", a);
	zeroize_hash(2, "1234", b);
	CHECK(memcmp(a, b, sizeof(a)) != 0);			// the salt changes it
	zeroize_hash(1, "1235", b);
	CHECK(memcmp(a, b, sizeof(a)) != 0);
	uid[0] ^= 1u;
	zeroize_hash(1, "1234", b);
	uid[0] ^= 1u;
	CHECK(memcmp(a, b, sizeof(a)) != 0);			// so does the radio's UID
	zeroize_hash(1, "1234", b);
	CHECK(memcmp(a, b, sizeof(a)) == 0);

	CHECK(!zeroize_code_is_set() && !zeroize_code_check("0000"));
	CHECK(!zeroize_code_set("12a4"));			// digits only
	CHECK(zeroize_code_set("1234") && zeroize_code_is_set());
	CHECK(zeroize_code_check("1234") && !zeroize_code_check("1243") && !zeroize_code_check("4321"));
	settings_t st;
	settings_read(&st);
	CHECK(memmem(&st, sizeof(st), "1234", 4) == NULL);	// never the code itself
	uint32_t salt = st.zz_salt;
	now_ms += 777;
	CHECK(zeroize_code_set("1234"));
	settings_read(&st);
	CHECK(st.zz_salt != salt);				// a new salt each time
	CHECK(settings_flush());
	settings_init();					// after a restart
	CHECK(zeroize_code_check("1234"));
	zeroize_code_clear();
	CHECK(!zeroize_code_is_set() && !zeroize_code_check("1234"));

	// The options, with their defaults
	CHECK(!zeroize_keys_only() && zeroize_confirm() && zeroize_tries() == 0);
	zeroize_set_keys_only(true);
	zeroize_set_confirm(false);
	zeroize_set_tries(5);
	CHECK(zeroize_keys_only() && !zeroize_confirm() && zeroize_tries() == 5);
	zeroize_set_tries(99);
	CHECK(zeroize_tries() == ZEROIZE_TRIES_MAX);
}

static void test_sequence(void)
{
	zeroize_seq_t s;
	char c[ZEROIZE_CODE_LEN];
	zeroize_seq_reset(&s);
	CHECK(!zeroize_seq_end(&s, c));				// # alone
	const char *d = "2468";
	for (int i = 0; i < 4; i++) {
		zeroize_seq_digit(&s, d[i]);
	}
	CHECK(zeroize_seq_end(&s, c) && memcmp(c, "2468", 4) == 0);
	CHECK(s.n == 0);					// starts over
	for (int i = 0; i < 3; i++) {
		zeroize_seq_digit(&s, '1');
	}
	CHECK(!zeroize_seq_end(&s, c));				// three digits: no code
	for (int i = 0; i < 5; i++) {
		zeroize_seq_digit(&s, (char)('1' + i));
	}
	CHECK(!zeroize_seq_end(&s, c));				// five: no code either, not the last four
	for (int i = 0; i < 300; i++) {
		zeroize_seq_digit(&s, '9');			// never wraps round to four
	}
	CHECK(!zeroize_seq_end(&s, c));
}

static void test_wipe_all(void)
{
	fresh();
	populate();
	uint8_t f = zeroize_run(false);
	CHECK(f == 0);
	// No copy of a key anywhere
	CHECK(!found(traffic_key, sizeof(traffic_key)) && !found(sys_secret, sizeof(sys_secret)));
	CHECK(records_with_key() == 0);
	// Erased: the EEPROM image, the flash image's codeplug, the journal, the staging area, the native area
	CHECK(blank(EE, 0x10000u));
	CHECK(blank(FL + 0x7B000u, 0x14000u));
	CHECK(blank(JOURNAL, 2u * SECTOR));
	CHECK(blank(STAGING, STAGING_SIZE));
	CHECK(blank(NATIVE, NATIVE_SIZE));
	// ... and nothing else changed but the settings ring (the ownership mark and the defaults saved)
	static const uint32_t changed[][2] = { { EE, 0x10000u }, { FL + 0x7B000u, 0x14000u }, { JOURNAL, 2u * SECTOR },
		{ STAGING, STAGING_SIZE }, { NATIVE, NATIVE_SIZE }, { SETTINGS, SETTINGS_SIZE } };
	CHECK(unchanged_except(changed, sizeof(changed) / sizeof(changed[0])));
	CHECK(sim_flash_data[CAL] == 0xCA && sim_flash_data[CAL + 0x1FF] == 0xCA);
	CHECK(memcmp(&sim_flash_data[FL + 0x8F400u], "VP", 2) == 0 && memcmp(&sim_flash_data[FL], "CUSTDATA", 8) == 0);
	CHECK(sim_flash_violations() == 0);
	// RAM: the radio core asked twice (before the settings and at the end), the hook, the crash record
	CHECK(radio_zeroizes == 2 && hook_calls == 1 && crash_clears == 1);
	// The radio comes back on the built-in set, the settings at their defaults (the code gone with them)
	CHECK(codeplug_source() == CODEPLUG_BUILTIN);
	CHECK(storage_region() == STORAGE_REGION_OURS);
	settings_init();
	settings_t st;
	settings_read(&st);
	CHECK(st.privacy_key[0] == 0 && st.privacy_type[0] == 0 && st.brightness == 100 && st.zz_flags == 0);
	codeplug_init();
	CHECK(codeplug_source() == CODEPLUG_BUILTIN);
	uint32_t id;
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));
	// Settings saved afterwards work as on a new radio
	settings_t *w = settings_begin();
	w->brightness = 70;
	settings_end(hal_millis());
	CHECK(settings_flush());
	settings_init();
	settings_read(&st);
	CHECK(st.brightness == 70);
}

static void test_wipe_keys_only(void)
{
	fresh();
	populate();
	zeroize_set_keys_only(true);
	CHECK(settings_flush());
	memcpy(before, sim_flash_data, FLASH_SIZE);
	uint8_t f = zeroize_run(true);
	CHECK(f == 0);
	CHECK(!found(traffic_key, sizeof(traffic_key)) && !found(sys_secret, sizeof(sys_secret)));
	CHECK(records_with_key() == 0);
	CHECK(blank(JOURNAL, 2u * SECTOR) && blank(STAGING, STAGING_SIZE));
	// Changed: the settings area's sector, the settings ring, the journal and the staging area; the codeplug stays
	static const uint32_t changed[][2] = { { EE + CODEPLUG_KV_ADDR, SECTOR }, { JOURNAL, 2u * SECTOR },
		{ STAGING, STAGING_SIZE }, { SETTINGS, SETTINGS_SIZE } };
	CHECK(unchanged_except(changed, sizeof(changed) / sizeof(changed[0])));
	codeplug_init();
	CHECK(codeplug_source() == CODEPLUG_OPENFW);
	channel_t ch;
	CHECK(codeplug_channel(5, &ch) && strcmp(ch.name, "Secret net") == 0);
	uint32_t id;
	uint8_t v[64];
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 3100999);	// the area's other keys kept
	CHECK(codeplug_kv_get(CODEPLUG_KEY_TRAFFIC + 3u, v, sizeof(v)) == -1);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_SYS_KEY, v, sizeof(v)) == -1);
	// The settings kept, but the keys; the wipe code and its options too
	settings_init();
	settings_t st;
	settings_read(&st);
	CHECK(st.privacy_key[0] == 0 && st.privacy_type[0] == 0 && st.privacy_active == 0);
	CHECK(st.brightness == 51);
	CHECK(zeroize_code_check("2468") && zeroize_keys_only());
	CHECK(radio_zeroizes == 2 && hook_calls == 1);
	CHECK(sim_flash_violations() == 0);
}

static void test_failures(void)
{
	// A codeplug sector that can't be read back: reported, and every other step still ran
	fresh();
	populate();
	sim_flash_bad_read(FL + 0x80000u, 16);
	uint8_t f = zeroize_run(false);
	sim_flash_bad_read(0, 0);
	CHECK((f & ZEROIZE_FAIL_CODEPLUG) != 0 && (f & ~ZEROIZE_FAIL_CODEPLUG) == 0);
	CHECK(!found(traffic_key, sizeof(traffic_key)) && records_with_key() == 0);
	CHECK(blank(EE, 0x10000u) && blank(STAGING, STAGING_SIZE));
	CHECK(hook_calls == 1);
	// Run again (OK on the wipe screen): it finishes
	CHECK(zeroize_run(false) == 0);

	// The radio core doesn't answer: said, after the wait; the flash is wiped all the same
	fresh();
	populate();
	radio_stuck = true;
	f = zeroize_run(true);
	CHECK(f == ZEROIZE_FAIL_RADIO);
	CHECK(!found(traffic_key, sizeof(traffic_key)) && records_with_key() == 0);

	// No flash map: nothing was ever stored, so only RAM
	sim_flash_create(FLASH_SIZE, NULL);
	CHECK(!storage_init());
	settings_init();
	codeplug_init();
	radio_zeroizes = hook_calls = 0;
	radio_stuck = false;
	CHECK(zeroize_run(false) == 0 && radio_zeroizes == 2 && hook_calls == 1);
}

static void test_erase_guard(void)
{
	fresh();
	memset(&sim_flash_data[CAL], 0xCA, 0x200);
	// Only inside the open-firmware images, the native and staging areas, never the calibration, ring or journal
	CHECK(!storage_erase(CAL, SECTOR));
	CHECK(!storage_erase(SETTINGS, SECTOR));
	CHECK(!storage_erase(JOURNAL, SECTOR));
	CHECK(!storage_erase(EE + 0xF000u, 2u * SECTOR));	// runs past the EEPROM image into the calibration
	CHECK(!storage_erase(NATIVE + NATIVE_SIZE + STAGING_SIZE, SECTOR));	// past every area
	CHECK(!storage_erase(EE + 1u, SECTOR) && !storage_erase(EE, 100u));	// whole sectors
	CHECK(sim_flash_data[CAL] == 0xCA);
	memset(&sim_flash_data[EE + 0x2000u], 0x00, 16);
	CHECK(storage_erase(EE + 0x2000u, SECTOR) && blank(EE + 0x2000u, SECTOR));
	CHECK(storage_erase(STAGING, SECTOR) && storage_erase(NATIVE, SECTOR));
	uint32_t erases = sim_flash_erases(EE + 0x3000u);
	CHECK(storage_erase(EE + 0x3000u, SECTOR) && sim_flash_erases(EE + 0x3000u) == erases);	// blank: no erase
}

int main(void)
{
	before = malloc(FLASH_SIZE);
	test_hash();
	test_sequence();
	test_wipe_all();
	test_wipe_keys_only();
	test_failures();
	test_erase_guard();
	sim_flash_free();
	free(before);
	if (failures != 0) {
		printf("zeroizeTest: %d FAILED\n", failures);
		return 1;
	}
	printf("zeroizeTest: all passed\n");
	return 0;
}
