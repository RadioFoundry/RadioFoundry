// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stored keys as one list (core/codeplug/key_store.c, docs/spec/key-check.md 4) on a simulated NOR flash with an
// in-memory settings record: the privacy slots that hold a valid key, in slot order, then the traffic key records whose
// system exists, in the area's order, each with its check value (the same as kcv.c computes for it) and, on request, its
// octets; slots and records that aren't keys are left out. A page from one walk (key_store_window) is what key_store_get
// gives one entry at a time, the count too, at every start and size
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/key_store.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_records.h"
#include "settings/settings.h"
#include "dmr/dmrPrivacy.h"
#include "storage/storage.h"
#include "support/kcv.h"
#include "hal_time.h"
#include "sim/sim_flash.h"

#if !KEY_STORE || !CONFIG_DMR_PRIVACY || !CODEPLUG_MODE_RECORDS
#error "keyStoreTest needs CONFIG_KEY_CHECK, CONFIG_DMR_PRIVACY and CODEPLUG_MODE_RECORDS"
#endif

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The firmware's pieces this test stands in for: the storage lock, the clock, the settings record
void storage_lock(void) {}
void storage_unlock(void) {}
uint32_t hal_millis(void) { return 1000u; }

static settings_t settings;
void settings_read(settings_t *out) { *out = settings; }
uint32_t settings_read_field(size_t offset, size_t size)	// as settings.c: one 1, 2 or 4-byte field
{
	uint32_t v = 0;
	if ((size != 1 && size != 2 && size != 4) || offset > sizeof settings || size > sizeof settings - offset)
		return 0;
	if (size == 1) {
		v = ((const uint8_t *)&settings)[offset];
	} else if (size == 2) {
		uint16_t h;
		memcpy(&h, (const uint8_t *)&settings + offset, sizeof h);
		v = h;
	} else {
		memcpy(&v, (const uint8_t *)&settings + offset, sizeof v);
	}
	return v;
}
settings_t *settings_begin(void) { return &settings; }
void settings_end(uint32_t now_ms) { (void)now_ms; }

#define FLASH_SIZE	0x200000u
static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0x000000u, .openfw_flash = 0x020000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

#define TYPE_A	0x80u	// system types with no AES algorithm: hashed check values
#define TYPE_B	0x81u
#define ALG	0x84u

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
	memset(&settings, 0, sizeof settings);
}

// System n of type, named
static void put_system(uint8_t n, uint8_t type, const char *name)
{
	uint8_t v[CODEPLUG_SYS_HDR + 4];
	memset(v, 0, sizeof v);
	v[0] = type;
	memset(&v[12], 0xFF, CODEPLUG_NAME_LEN);
	memcpy(&v[12], name, strlen(name));
	memset(&v[CODEPLUG_SYS_HDR], 0x5A, 4);		// the type's own fields
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_SYSTEM + n), v, sizeof v));
}

// Traffic key record n: system, algorithm ALG, key ID, flags 0x01, the key
static void put_traffic(uint16_t n, uint8_t system, uint16_t key_id, const uint8_t *key, uint8_t len)
{
	uint8_t v[CODEPLUG_TRAFFIC_HDR + 64];
	v[0] = system;
	v[1] = ALG;
	v[2] = (uint8_t)key_id;
	v[3] = (uint8_t)(key_id >> 8);
	v[4] = 0x01;
	memcpy(&v[CODEPLUG_TRAFFIC_HDR], key, len);
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_TRAFFIC + n), v, (uint16_t)(CODEPLUG_TRAFFIC_HDR + len)));
}

static void test_privacy(void)
{
	fresh();
	key_entry_t e;
	uint8_t key[KEY_STORE_MAX_LEN], want[KCV_LEN];
	CHECK(key_store_count() == 0 && !key_store_get(0, &e, NULL));
	// Slot 1 off, slot 2 a key its type doesn't take (Motorola's are 1..255), slot 3 past 16 bits: none listed
	settings.privacy_type[0] = DMR_PRIVACY_OFF;
	settings.privacy_key[0] = 5;
	settings.privacy_type[1] = DMR_PRIVACY_MOTOROLA_BP;
	settings.privacy_key[1] = 300;
	settings.privacy_type[2] = DMR_PRIVACY_ANYTONE_BP;
	settings.privacy_key[2] = 0x10001u;
	CHECK(key_store_count() == 0);
	// Slots 4 and 2 valid: listed in slot order, with the check value kcv.c gives and the value big endian
	settings.privacy_type[3] = DMR_PRIVACY_ANYTONE_BP;
	settings.privacy_key[3] = 0x1234u;
	settings.privacy_key[1] = 42;
	CHECK(key_store_count() == 2);
	CHECK(key_store_get(0, &e, key) && e.cls == KCV_CLASS_PRIVACY && e.slot == 2 && e.type == DMR_PRIVACY_MOTOROLA_BP &&
	      e.len == 4 && e.name[0] == 0);
	CHECK(kcv_privacy(DMR_PRIVACY_MOTOROLA_BP, 42, want) && e.kcv_ok && memcmp(e.kcv, want, KCV_LEN) == 0);
	uint8_t moto42[KCV_LEN];
	memcpy(moto42, want, KCV_LEN);
	CHECK(key[0] == 0 && key[1] == 0 && key[2] == 0 && key[3] == 42);
	CHECK(key_store_get(1, &e, key) && e.slot == 4 && e.type == DMR_PRIVACY_ANYTONE_BP);
	CHECK(kcv_privacy(DMR_PRIVACY_ANYTONE_BP, 0x1234u, want) && memcmp(e.kcv, want, KCV_LEN) == 0);
	CHECK(key[2] == 0x12 && key[3] == 0x34);
	CHECK(!key_store_get(2, &e, NULL));
	// The two schemes give the same value different check values
	settings.privacy_type[1] = DMR_PRIVACY_ANYTONE_BP;
	CHECK(key_store_get(0, &e, NULL) && e.type == DMR_PRIVACY_ANYTONE_BP && memcmp(e.kcv, moto42, KCV_LEN) != 0);
	CHECK(kcv_privacy(DMR_PRIVACY_ANYTONE_BP, 42, want) && memcmp(e.kcv, want, KCV_LEN) == 0);
}

static void test_traffic(void)
{
	fresh();
	put_system(3, TYPE_A, "Wide");
	put_system(9, TYPE_B, "Bee");
	const uint8_t k1[16] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };
	const uint8_t k2[8] = { 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7 };
	uint8_t big[KEY_STORE_MAX_LEN + 8];
	memset(big, 0x11, sizeof big);
	put_traffic(0, 3, 0x0001, k1, sizeof k1);
	put_traffic(1, 5, 0x0002, k1, sizeof k1);	// system 5 doesn't exist: left out
	put_traffic(2, 9, 0x0102, k2, sizeof k2);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_TRAFFIC + 3, (const uint8_t[]){ 3, ALG, 4, 0, 1 }, CODEPLUG_TRAFFIC_HDR));	// no key
	put_traffic(4, 3, 0x0010, big, KEY_STORE_MAX_LEN);		// the longest carried
	put_traffic(5, 3, 0x0011, big, KEY_STORE_MAX_LEN + 8);		// longer: listed, no check value here
	settings.privacy_type[0] = DMR_PRIVACY_MOTOROLA_BP;		// a slot first
	settings.privacy_key[0] = 7;
	CHECK(key_store_count() == 5);

	key_entry_t e;
	uint8_t key[KEY_STORE_MAX_LEN], want[KCV_LEN];
	CHECK(key_store_get(0, &e, NULL) && e.cls == KCV_CLASS_PRIVACY && e.slot == 1);
	CHECK(key_store_get(1, &e, key) && e.cls == KCV_CLASS_TRAFFIC && e.system == 3 && e.type == TYPE_A &&
	      e.alg == ALG && e.key_id == 1 && e.flags == 0x01 && e.record == CODEPLUG_KEY_TRAFFIC + 0 && e.len == 16 &&
	      strcmp(e.name, "Wide") == 0 && memcmp(key, k1, 16) == 0);
	CHECK(kcv_traffic(TYPE_A, ALG, k1, sizeof k1, want) && e.kcv_ok && memcmp(e.kcv, want, KCV_LEN) == 0);
	CHECK(key_store_get(2, &e, key) && e.system == 9 && e.type == TYPE_B && e.key_id == 0x0102 &&
	      e.record == CODEPLUG_KEY_TRAFFIC + 2 && strcmp(e.name, "Bee") == 0 && memcmp(key, k2, 8) == 0);
	CHECK(kcv_traffic(TYPE_B, ALG, k2, sizeof k2, want) && memcmp(e.kcv, want, KCV_LEN) == 0);
	CHECK(key_store_get(3, &e, NULL) && e.key_id == 0x0010 && e.len == KEY_STORE_MAX_LEN && e.kcv_ok);
	CHECK(key_store_get(4, &e, key) && e.key_id == 0x0011 && e.len == KEY_STORE_MAX_LEN + 8 && !e.kcv_ok);
	CHECK(memcmp(key, big, KEY_STORE_MAX_LEN) == 0);		// at most KEY_STORE_MAX_LEN copied
	CHECK(!key_store_get(5, &e, NULL));

	// The system gone: its keys leave the list
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_SYSTEM + 9));
	CHECK(key_store_count() == 4 && key_store_get(2, &e, NULL) && e.key_id == 0x0010);
}

// A page from one walk is what key_store_get gives one by one, the count too, at every start and size
static void test_window(void)
{
	fresh();
	put_system(3, TYPE_A, "Wide");
	const uint8_t k[6] = { 1, 2, 3, 4, 5, 6 };	// the longest record's key below
	for (uint16_t i = 0; i < 6; i++)
		put_traffic((uint16_t)(i * 2u), 3, (uint16_t)(i + 1u), k, (uint8_t)(i + 1u));
	put_traffic(1, 4, 0x0099, k, 4);	// no system 4: skipped, in the middle
	settings.privacy_type[2] = DMR_PRIVACY_ANYTONE_BP;
	settings.privacy_key[2] = 0x0BEEu;
	settings.privacy_type[3] = DMR_PRIVACY_MOTOROLA_BP;
	settings.privacy_key[3] = 9;
	uint16_t total = 0, n = key_store_count();
	key_entry_t page[4], one;
	CHECK(n == 8);
	for (uint16_t first = 0; first <= n; first++) {
		for (uint16_t max = 0; max <= 4; max++) {
			uint16_t got = key_store_window(first, page, max, &total);
			CHECK(total == n && got == (uint16_t)(first + max <= n ? max : n - first));
			for (uint16_t i = 0; i < got; i++) {
				CHECK(key_store_get((uint16_t)(first + i), &one, NULL));
				CHECK(memcmp(&one, &page[i], sizeof one) == 0);
			}
		}
	}
}

int main(void)
{
	test_privacy();
	test_traffic();
	test_window();
	if (failures != 0) {
		printf("keyStoreTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("keyStoreTest: all passed\n");
	return 0;
}
