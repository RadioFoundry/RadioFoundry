// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Storing a key from key fill (core/codeplug/key_store.c key_store_put, docs/spec/key-fill.md 8) on a simulated NOR
// flash with an in-memory settings record: a privacy slot and its check value, the radio core told only for the slot
// in use with privacy on; traffic keys into the system that matches by type and identity (empty identity: the type's
// only system), a record of that system, algorithm and key ID replaced, else a new one; and the refusals (no system,
// ambiguous, bad lengths, an unsupported class, a full area). Every stored key read back through the key list
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/key_store.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_records.h"
#include "settings/settings.h"
#include "dmr/dmrPrivacy.h"
#include "radio/radio.h"
#include "storage/storage.h"
#include "support/kcv.h"
#include "hal_time.h"
#include "sim/sim_flash.h"

#if !KEY_STORE || !CONFIG_DMR_PRIVACY || !CODEPLUG_MODE_RECORDS
#error "keyStoreTest needs CONFIG_KEY_CHECK, CONFIG_DMR_PRIVACY and CODEPLUG_MODE_RECORDS"
#endif

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The firmware's pieces this test stands in for: the storage lock, the clock, the settings record, the radio core
void storage_lock(void) {}
void storage_unlock(void) {}
uint32_t hal_millis(void) { return 1000u; }

static settings_t settings;
static int settings_ends;
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
void settings_end(uint32_t now_ms) { (void)now_ms; settings_ends++; }

static bool view_privacy;
static int privacy_reqs;
static uint8_t req_type;
static uint16_t req_key;
bool radio_dmr_privacy_on(void)
{
	return view_privacy;
}
void radio_req_dmr_privacy(uint8_t type, uint16_t key)
{
	privacy_reqs++;
	req_type = type;
	req_key = key;
}

#define FLASH_SIZE	0x200000u
static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0x000000u, .openfw_flash = 0x020000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

#define TYPE_A	0x80u	// a system type with no AES algorithm: hashed check values
#define TYPE_B	0x81u
#define ALG	0x84u

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
	memset(&settings, 0, sizeof settings);
	settings_ends = privacy_reqs = 0;
	view_privacy = false;
}

// System n of type, its block starting with the identity
static void put_system(uint8_t n, uint8_t type, const char *name, const uint8_t *identity, uint8_t id_len)
{
	uint8_t v[CODEPLUG_SYS_HDR + 12];
	memset(v, 0, sizeof v);
	v[0] = type;
	v[3] = id_len;
	memset(&v[12], 0xFF, CODEPLUG_NAME_LEN);
	memcpy(&v[12], name, strlen(name));
	if (id_len != 0)
		memcpy(&v[CODEPLUG_SYS_HDR], identity, id_len);
	memset(&v[CODEPLUG_SYS_HDR + id_len], 0x5A, 4);		// the type's own fields after the identity
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_SYSTEM + n), v, (uint16_t)(CODEPLUG_SYS_HDR + id_len + 4u)));
}

static key_put_t traffic(uint8_t type, const uint8_t *identity, uint8_t id_len, uint16_t key_id, const uint8_t *key,
			 uint8_t key_len)
{
	key_put_t p;
	memset(&p, 0, sizeof p);
	p.cls = KCV_CLASS_TRAFFIC;
	p.type = type;
	p.alg = ALG;
	p.key_id = key_id;
	p.flags = 0x01;
	p.id_len = id_len;
	if (id_len != 0)
		memcpy(p.identity, identity, id_len);
	p.key_len = key_len;
	p.key = key;
	return p;
}

static int traffic_count(void)
{
	codeplug_kv_cursor_t c;
	uint16_t k;
	int n = 0;
	codeplug_kv_begin(&c, CODEPLUG_KEY_TRAFFIC, (uint16_t)(CODEPLUG_KEY_TRAFFIC + 0x3FFu));
	while (codeplug_kv_next(&c, &k, NULL, 0) >= 0)
		n++;
	return n;
}

// The key list's entry for record key k, or false
static bool entry_of(uint16_t k, key_entry_t *e, uint8_t *key)
{
	for (uint16_t i = 0; key_store_get(i, e, key); i++)
		if (e->cls == KCV_CLASS_TRAFFIC && e->record == k)
			return true;
	return false;
}

static void test_privacy(void)
{
	fresh();
	uint8_t kcv[KCV_LEN], want[KCV_LEN];
	const uint8_t value[4] = { 0x00, 0x00, 0x12, 0x34 };
	key_put_t p = { .cls = KCV_CLASS_PRIVACY, .type = DMR_PRIVACY_ANYTONE_BP, .slot = 3, .key_len = 4, .key = value };
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED);
	CHECK(settings.privacy_type[2] == DMR_PRIVACY_ANYTONE_BP && settings.privacy_key[2] == 0x1234u);
	CHECK(settings.privacy_type[0] == 0 && settings.privacy_active == 0 && settings_ends == 1);
	CHECK(kcv_privacy(DMR_PRIVACY_ANYTONE_BP, 0x1234u, want) && memcmp(kcv, want, KCV_LEN) == 0);
	CHECK(privacy_reqs == 0);	// privacy off: nothing loaded
	// The list shows it with the same check value
	key_entry_t e;
	uint8_t key[KEY_STORE_MAX_LEN];
	CHECK(key_store_count() == 1 && key_store_get(0, &e, key));
	CHECK(e.cls == KCV_CLASS_PRIVACY && e.slot == 3 && e.kcv_ok && memcmp(e.kcv, kcv, KCV_LEN) == 0);
	CHECK(memcmp(key, value, 4) == 0);

	// Slot 3 is in use but privacy is off: stored, not loaded
	settings.privacy_active = 3;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED && privacy_reqs == 0);
	// On: the radio core takes it at once
	view_privacy = true;
	const uint8_t v2[4] = { 0x00, 0x00, 0xBE, 0xEF };
	p.key = v2;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED && privacy_reqs == 1 && req_type == DMR_PRIVACY_ANYTONE_BP &&
	      req_key == 0xBEEFu && settings.privacy_key[2] == 0xBEEFu);
	// Another slot than the one in use: not loaded
	p.slot = 2;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED && privacy_reqs == 1 && settings.privacy_key[1] == 0xBEEFu);
	// No slot saved in use yet while on: the radio core's slot 1
	settings.privacy_active = 0;
	const uint8_t v3[4] = { 0x00, 0x00, 0x00, 0x2A };
	key_store_put(&(key_put_t){ .cls = KCV_CLASS_PRIVACY, .type = DMR_PRIVACY_MOTOROLA_BP, .slot = 1, .key_len = 4,
				    .key = v3 }, kcv);
	CHECK(privacy_reqs == 2 && req_type == DMR_PRIVACY_MOTOROLA_BP && req_key == 42 && settings.privacy_key[0] == 42);
	CHECK(kcv_privacy(DMR_PRIVACY_MOTOROLA_BP, 42, want) && memcmp(kcv, want, KCV_LEN) == 0);

	// Refused: a slot out of range, a length not 4, type off or unknown, a key the type doesn't take, past 16 bits
	int ends = settings_ends;
	p.key = value;
	p.slot = 0;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_BAD);
	p.slot = SETTINGS_PRIVACY_KEYS + 1;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_BAD);
	p.slot = 1;
	p.key_len = 3;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_BAD);
	p.key_len = 4;
	p.type = DMR_PRIVACY_OFF;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_BAD);
	p.type = DMR_PRIVACY_NUM_TYPES;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_BAD);
	p.type = DMR_PRIVACY_MOTOROLA_BP;	// 1..255 only
	CHECK(key_store_put(&p, kcv) == KEY_PUT_BAD);
	const uint8_t wide[4] = { 0x00, 0x01, 0x00, 0x01 };
	p.type = DMR_PRIVACY_ANYTONE_BP;
	p.key = wide;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_BAD);
	CHECK(settings_ends == ends && settings.privacy_key[0] == 42);
	const uint8_t zero[KCV_LEN] = { 0 };
	CHECK(memcmp(kcv, zero, KCV_LEN) == 0);	// nothing stored: zeros
}

static void test_identity(void)
{
	fresh();
	const uint8_t id[5] = { 0x01, 0x23, 0x0B, 0x45, 0x03 };
	put_system(3, TYPE_A, "Wide", id, 5);
	put_system(4, TYPE_A, "Plain", NULL, 0);
	uint8_t out[8];
	CHECK(key_store_identity(3, out, sizeof out) == 5 && memcmp(out, id, 5) == 0);
	CHECK(key_store_identity(3, out, 4) == -1);	// longer than cap
	CHECK(key_store_identity(4, out, sizeof out) == 0);
	CHECK(key_store_identity(5, out, sizeof out) == -1);	// gone
	CHECK(key_store_identity(0xFF, out, sizeof out) == -1);
}

static void test_traffic(void)
{
	fresh();
	const uint8_t id3[5] = { 0x01, 0x23, 0x0B, 0x45, 0x03 }, id6[5] = { 0x01, 0x23, 0x0B, 0x45, 0x04 };
	put_system(3, TYPE_A, "Wide", id3, 5);
	put_system(6, TYPE_A, "Other", id6, 5);
	put_system(9, TYPE_B, "Bee", NULL, 0);
	uint8_t kcv[KCV_LEN], want[KCV_LEN];
	const uint8_t k1[16] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };
	const uint8_t k2[8] = { 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7 };
	// A record of another system already at the first key: the new one goes after it
	const uint8_t other[5 + 2] = { 6, ALG, 0x01, 0x00, 0, 0xC1, 0xC2 };
	CHECK(codeplug_kv_set(CODEPLUG_KEY_TRAFFIC + 0, other, sizeof other));

	// New: matched by type and identity (system 3, not 6)
	key_put_t p = traffic(TYPE_A, id3, 5, 0x0001, k1, sizeof k1);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED);
	CHECK(kcv_traffic(TYPE_A, ALG, k1, sizeof k1, want) && memcmp(kcv, want, KCV_LEN) == 0);
	uint8_t key[KEY_STORE_MAX_LEN];
	CHECK(codeplug_traffic_key(3, ALG, 0x0001, key, sizeof key) == 16 && memcmp(key, k1, 16) == 0);
	key_entry_t e;
	CHECK(entry_of(CODEPLUG_KEY_TRAFFIC + 1, &e, key) && e.system == 3 && e.key_id == 1 && e.flags == 0x01 &&
	      e.kcv_ok && memcmp(e.kcv, kcv, KCV_LEN) == 0 && strcmp(e.name, "Wide") == 0);
	CHECK(traffic_count() == 2);

	// The same system, algorithm and key ID: replaced in place, even with another length
	p = traffic(TYPE_A, id3, 5, 0x0001, k2, sizeof k2);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED && traffic_count() == 2);
	CHECK(codeplug_traffic_key(3, ALG, 0x0001, key, sizeof key) == 8 && memcmp(key, k2, 8) == 0);
	CHECK(kcv_traffic(TYPE_A, ALG, k2, sizeof k2, want) && memcmp(kcv, want, KCV_LEN) == 0);
	CHECK(entry_of(CODEPLUG_KEY_TRAFFIC + 1, &e, NULL) && e.len == 8);

	// Another key ID: a new record at the next free key; system 6's record is untouched
	p = traffic(TYPE_A, id3, 5, 0x0102, k1, sizeof k1);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED && traffic_count() == 3);
	CHECK(entry_of(CODEPLUG_KEY_TRAFFIC + 2, &e, NULL) && e.system == 3 && e.key_id == 0x0102);
	CHECK(codeplug_traffic_key(6, ALG, 0x0001, key, sizeof key) == 2 && key[0] == 0xC1);
	// The other system by its identity, same algorithm and key ID as system 3's first: its own record replaced
	p = traffic(TYPE_A, id6, 5, 0x0001, k2, 4);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED && traffic_count() == 3);
	CHECK(codeplug_traffic_key(6, ALG, 0x0001, key, sizeof key) == 4 && key[0] == 0xB0);
	CHECK(codeplug_traffic_key(3, ALG, 0x0001, key, sizeof key) == 8);

	// A gap fills first: remove record 1, the next new key lands there
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_TRAFFIC + 1));
	p = traffic(TYPE_A, id3, 5, 0x0003, k1, 1);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED && entry_of(CODEPLUG_KEY_TRAFFIC + 1, &e, NULL) && e.key_id == 3);

	// Empty identity: the type's only system (type B's 9)
	p = traffic(TYPE_B, NULL, 0, 0x0007, k1, KEY_STORE_MAX_LEN > 16 ? 16 : KEY_STORE_MAX_LEN);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED && codeplug_traffic_key(9, ALG, 7, key, sizeof key) == 16);
	// ... ambiguous for type A, which has two
	uint8_t zero[KCV_LEN] = { 0 };
	int before = traffic_count();
	p = traffic(TYPE_A, NULL, 0, 0x0009, k1, 16);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_AMBIGUOUS && memcmp(kcv, zero, KCV_LEN) == 0);
	// No system: an identity no system has, a type no system has, an identity the system has but a type it hasn't
	const uint8_t idx[5] = { 9, 9, 9, 9, 9 };
	p = traffic(TYPE_A, idx, 5, 1, k1, 16);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_NO_SYSTEM);
	p = traffic(0x82, NULL, 0, 1, k1, 16);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_NO_SYSTEM);
	p = traffic(TYPE_B, id3, 5, 1, k1, 16);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_NO_SYSTEM);
	// A prefix of the identity is not the identity
	p = traffic(TYPE_A, id3, 4, 1, k1, 16);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_NO_SYSTEM);
	// Bad lengths: no key, past the longest, an identity past 8
	p = traffic(TYPE_A, id3, 5, 1, k1, 0);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_BAD);
	uint8_t big[KEY_STORE_MAX_LEN + 1];
	memset(big, 0x11, sizeof big);
	p = traffic(TYPE_A, id3, 5, 1, big, KEY_STORE_MAX_LEN + 1);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_BAD);
	p = traffic(TYPE_A, id3, 5, 1, big, KEY_STORE_MAX_LEN);
	p.id_len = 9;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_BAD);
	CHECK(traffic_count() == before);
	// The longest key is taken
	p = traffic(TYPE_A, id3, 5, 0x0010, big, KEY_STORE_MAX_LEN);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_STORED && traffic_count() == before + 1);
	CHECK(kcv_traffic(TYPE_A, ALG, big, KEY_STORE_MAX_LEN, want) && memcmp(kcv, want, KCV_LEN) == 0);
}

static void test_unsupported(void)
{
	fresh();
	uint8_t kcv[KCV_LEN];
	const uint8_t value[4] = { 0, 0, 0, 1 };
	key_put_t p = { .cls = 0, .key_len = 4, .key = value };
	CHECK(key_store_put(&p, kcv) == KEY_PUT_UNSUPPORTED);
	p.cls = 3;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_UNSUPPORTED);
	p.cls = 0xFF;
	CHECK(key_store_put(&p, kcv) == KEY_PUT_UNSUPPORTED);
}

// The area full: the write fails, nothing changes
static void test_full(void)
{
	fresh();
	const uint8_t id[2] = { 0x77, 0x01 };
	put_system(1, TYPE_A, "Full", id, 2);
	uint8_t filler[256];
	memset(filler, 0xEE, sizeof filler);
	uint16_t k = CODEPLUG_KEY_TARGET;
	// Big values until one doesn't fit, then smaller ones, until not even one octet does
	for (uint16_t len = sizeof filler; len >= 1; len = (uint16_t)(len / 2u))
		while (k < CODEPLUG_KEY_TARGET + 0x7FFu && codeplug_kv_set(k, filler, len))
			k++;
	CHECK(!codeplug_kv_set(k, filler, 1));
	uint8_t kcv[KCV_LEN];
	const uint8_t key[16] = { 1 };
	key_put_t p = traffic(TYPE_A, id, 2, 1, key, sizeof key);
	CHECK(key_store_put(&p, kcv) == KEY_PUT_WRITE_FAILED && traffic_count() == 0);
	uint8_t zero[KCV_LEN] = { 0 };
	CHECK(memcmp(kcv, zero, KCV_LEN) == 0);
}

int main(void)
{
	test_privacy();
	test_identity();
	test_traffic();
	test_unsupported();
	test_full();
	if (failures != 0) {
		printf("keyStoreTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("keyStoreTest: all passed\n");
	return 0;
}
