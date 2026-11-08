// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (core/codeplug/codeplug_kv.c, docs/spec/codeplug-settings.md) on a simulated NOR flash:
// reads of a blank, valid, invalid and newer area, writes that keep unknown keys and drop duplicates, values longer
// than a reader knows, a full area, batches of sets and removes, walks over a key range, the raw write rule
// (codeplug_kv_keep), and power cuts during a write
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

#define FLASH_SIZE	0x200000u
#define EE		0x000000u
#define FL		0x020000u
#define AREA		(EE + CODEPLUG_KV_ADDR)

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// An area as another tool would write it: the header over data of len bytes, the CRC computed unless bad_crc
static void raw_area(const uint8_t *data, uint16_t len, uint8_t version, uint8_t hdr, bool bad_crc)
{
	uint8_t *s = &sim_flash_data[AREA];
	memset(s, 0xFF, STORAGE_SECTOR);
	put32(&s[0], CODEPLUG_KV_MAGIC);
	s[4] = version;
	s[5] = hdr;
	put16(&s[6], len);
	put32(&s[8], storage_crc32(0, data, len) ^ (bad_crc ? 1u : 0u));
	memset(&s[12], 0, hdr - 12u);
	memcpy(&s[hdr], data, len);
}

static uint16_t entry(uint8_t *p, uint16_t key, const void *v, uint16_t n)
{
	put16(p, key);
	put16(p + 2, n);
	memcpy(p + 4, v, n);
	return (uint16_t)(4u + n);
}

static void test_blank(void)
{
	fresh();
	uint8_t v[4];
	uint16_t len, n;
	uint32_t id;
	CHECK(codeplug_kv_info(&len, &n) == CODEPLUG_KV_EMPTY && len == 0 && n == 0);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_ARS_SERVER, v, sizeof(v)) == -1);
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));
	// A blank flash keeps the built-in codeplug: the area is apart from the records and doesn't store them
	uint32_t serial = codeplug_kv_serial(), cp_serial = codeplug_serial();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 3100999));
	CHECK(codeplug_kv_serial() != serial && codeplug_serial() == cp_serial);	// a key written: no codeplug reload
	CHECK(codeplug_source() == CODEPLUG_BUILTIN);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 3100999);
	CHECK(codeplug_kv_info(&len, &n) == CODEPLUG_KV_VALID && len == 8 && n == 1);
	// The bytes as the spec gives them
	const uint8_t *s = &sim_flash_data[AREA];
	CHECK(memcmp(s, "RFKV", 4) == 0 && s[4] == 1 && s[5] == 16 && s[6] == 8 && s[7] == 0);
	CHECK(s[16] == 0x01 && s[17] == 0 && s[18] == 4 && s[19] == 0);
	CHECK(s[20] == 0x47 && s[21] == 0x51 && s[22] == 0x2F && s[23] == 0);	// 3100999 = 0x2F5147
	CHECK(s[24] == 0xFF && s[STORAGE_SECTOR - 1] == 0xFF);
	CHECK(sim_flash_violations() == 0);
}

static void test_edits(void)
{
	fresh();
	uint32_t id;
	uint16_t len, n;
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1234));
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_IP_GATEWAY, 3100500));
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 0));		// off: present, 0
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 0);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_IP_GATEWAY, &id) && id == 3100500);
	CHECK(codeplug_kv_info(&len, &n) == CODEPLUG_KV_VALID && len == 16 && n == 2);
	// A longer and a shorter value: replaced in place, the order kept
	uint8_t big[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }, back[16];
	CHECK(codeplug_kv_set(CODEPLUG_KEY_ARS_SERVER, big, sizeof(big)));
	CHECK(codeplug_kv_get(CODEPLUG_KEY_ARS_SERVER, back, sizeof(back)) == 10 && memcmp(back, big, 10) == 0);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_ARS_SERVER, back, 3) == 10);	// the length, only max bytes copied
	CHECK(sim_flash_data[AREA + 16] == 0x01 && sim_flash_data[AREA + 16 + 14] == 0x02);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_IP_GATEWAY, &id) && id == 3100500);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_ARS_SERVER, big, 1));
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 1);	// shorter than a u32: zero-extended
	CHECK(codeplug_kv_set(CODEPLUG_KEY_ARS_SERVER, (const uint8_t[]){ 0x40, 0x42, 0x0F }, 3));	// a 3-byte ID
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 1000000);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_ARS_SERVER, big, 0));
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));	// empty: as if absent
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_IP_GATEWAY, &id) && id == 3100500);
	// Not keys
	CHECK(!codeplug_kv_set(0, big, 4) && !codeplug_kv_set(0xFFFF, big, 4));
	// An ID above the radios' range reads as absent
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_IP_GATEWAY, CODEPLUG_DMR_ID_MAX + 1u));
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_IP_GATEWAY, &id));
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_IP_GATEWAY, CODEPLUG_DMR_ID_MAX));
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_IP_GATEWAY, &id) && id == CODEPLUG_DMR_ID_MAX);
}

// Another tool's area: unknown keys, a value longer than we know, a longer header, a duplicate
static void test_foreign(void)
{
	fresh();
	uint8_t d[64], gw[8] = { 0x10, 0x27, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD }, odd[3] = { 7, 8, 9 }, four[4] = { 1, 0, 0, 0 };
	uint16_t len = 0;
	len += entry(&d[len], 0x7123, odd, sizeof(odd));
	len += entry(&d[len], CODEPLUG_KEY_IP_GATEWAY, gw, sizeof(gw));	// 10000, plus fields a later version added
	len += entry(&d[len], CODEPLUG_KEY_IP_GATEWAY, four, sizeof(four));	// a duplicate: not read
	raw_area(d, len, 1, 20, false);
	uint32_t id;
	uint16_t l, n;
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && l == len && n == 3);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_IP_GATEWAY, &id) && id == 10000);
	uint8_t back[8];
	CHECK(codeplug_kv_get(0x7123, back, sizeof(back)) == 3 && memcmp(back, odd, 3) == 0);
	// A write keeps the unknown key and the longer header, drops the duplicate
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 42));
	const uint8_t *s = &sim_flash_data[AREA];
	CHECK(s[5] == 20);
	CHECK(memcmp(&s[20], d, 4 + 3 + 4 + 8) == 0);	// the unknown key and the gateway, byte for byte
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && n == 3 && l == 7 + 12 + 8);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 42);
	CHECK(codeplug_kv_get(0x7123, back, sizeof(back)) == 3);
}

static void test_invalid(void)
{
	uint8_t d[16], v[4] = { 5, 0, 0, 0 };
	uint16_t len = entry(d, CODEPLUG_KEY_ARS_SERVER, v, sizeof(v)), l, n;
	uint32_t id;

	// A bad CRC: read as empty, a write starts over
	fresh();
	raw_area(d, len, 1, 16, true);
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_BAD);
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_IP_GATEWAY, 77));
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && n == 1);
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));

	// An entry that runs past the data
	fresh();
	put16(&d[2], 40);
	raw_area(d, len, 1, 16, false);
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_BAD);
	put16(&d[2], 4);

	// Data longer than the sector
	fresh();
	raw_area(d, len, 1, 16, false);
	put16(&sim_flash_data[AREA + 6], STORAGE_SECTOR);
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_BAD);

	// Some other tool's bytes, and a header shorter than ours
	fresh();
	memset(&sim_flash_data[AREA], 0, 16);
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_BAD);
	fresh();
	raw_area(d, len, 1, 16, false);
	sim_flash_data[AREA + 5] = 12;
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_BAD);

	// A later version: read as empty and never rewritten
	fresh();
	raw_area(d, len, 2, 16, false);
	uint8_t before[STORAGE_SECTOR];
	memcpy(before, &sim_flash_data[AREA], sizeof(before));
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_NEWER);
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));
	uint32_t serial = codeplug_kv_serial();
	CHECK(!codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 9));
	CHECK(memcmp(before, &sim_flash_data[AREA], sizeof(before)) == 0 && codeplug_kv_serial() == serial);
}

// codeplug_kv_check on a sector in RAM (TFTP's staged area) says what a reader of the same bytes in flash says
static void test_check(void)
{
	fresh();
	const uint8_t *s = &sim_flash_data[AREA];
	uint16_t len, n;
	uint8_t d[16];
	uint16_t dl = entry(d, CODEPLUG_KEY_ARS_SERVER, "\x01\x02\x03\x00", 4);
	CHECK(codeplug_kv_check(s) == CODEPLUG_KV_EMPTY);
	raw_area(d, dl, 1, 16, false);
	CHECK(codeplug_kv_check(s) == CODEPLUG_KV_VALID && codeplug_kv_info(&len, &n) == CODEPLUG_KV_VALID);
	raw_area(d, dl, 1, 20, false);	// a longer header
	CHECK(codeplug_kv_check(s) == CODEPLUG_KV_VALID && codeplug_kv_info(&len, &n) == CODEPLUG_KV_VALID);
	raw_area(d, dl, 1, 16, true);
	CHECK(codeplug_kv_check(s) == CODEPLUG_KV_BAD && codeplug_kv_info(&len, &n) == CODEPLUG_KV_BAD);
	raw_area(d, dl, 2, 16, false);
	CHECK(codeplug_kv_check(s) == CODEPLUG_KV_NEWER && codeplug_kv_info(&len, &n) == CODEPLUG_KV_NEWER);
	put16(&d[2], 5);		// the entry runs past the data
	raw_area(d, dl, 1, 16, false);
	CHECK(codeplug_kv_check(s) == CODEPLUG_KV_BAD && codeplug_kv_info(&len, &n) == CODEPLUG_KV_BAD);
}

static void test_full(void)
{
	fresh();
	static uint8_t big[STORAGE_SECTOR];
	memset(big, 0x5A, sizeof(big));
	uint16_t room = STORAGE_SECTOR - CODEPLUG_KV_HDR;
	CHECK(!codeplug_kv_set(0x7000, big, (uint16_t)(room - 3u)));	// one byte too many with its 4-byte entry header
	CHECK(codeplug_kv_set(0x7000, big, (uint16_t)(room - 4u)));
	CHECK(!codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1));
	uint16_t l, n;
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && l == room && n == 1);
	CHECK(codeplug_kv_set(0x7000, big, 100));	// shrinks: room again
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1));
}

static uint32_t sector_writes(void)
{
	storage_stats_t st;
	storage_get_stats(&st);
	return st.sector_writes;
}

// Several sets and removes in one rewrite, all or nothing
static void test_batch(void)
{
	fresh();
	uint32_t id;
	uint8_t a4[4], b4[4], big[STORAGE_SECTOR];
	uint16_t l, n;
	put32(a4, 3100111);
	put32(b4, 3100222);
	CHECK(codeplug_kv_set(0x7001, "x", 1));
	uint32_t w = sector_writes(), serial = codeplug_kv_serial();
	codeplug_kv_op_t ops[] = {
		{ CODEPLUG_KEY_ARS_SERVER, 4, a4 },
		{ CODEPLUG_KEY_IP_GATEWAY, 4, b4 },
		{ 0x7001, CODEPLUG_KV_REMOVE, NULL },
		{ 0x7002, 0, NULL },			// an empty value: present, length 0
	};
	CHECK(codeplug_kv_write(ops, 4));
	CHECK(sector_writes() == w + 1 && codeplug_kv_serial() != serial);	// one rewrite, one serial move
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 3100111);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_IP_GATEWAY, &id) && id == 3100222);
	CHECK(codeplug_kv_get(0x7001, a4, 4) == -1);
	CHECK(codeplug_kv_get(0x7002, a4, 4) == 0);
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && n == 3);

	// One op that doesn't fit: nothing of the batch is written
	memset(big, 0x5A, sizeof(big));
	uint8_t before[STORAGE_SECTOR];
	memcpy(before, &sim_flash_data[AREA], sizeof(before));
	w = sector_writes();
	serial = codeplug_kv_serial();
	codeplug_kv_op_t too_big[] = {
		{ CODEPLUG_KEY_ARS_SERVER, CODEPLUG_KV_REMOVE, NULL },
		{ 0x7003, STORAGE_SECTOR - CODEPLUG_KV_HDR, big },
	};
	CHECK(!codeplug_kv_write(too_big, 2));
	CHECK(memcmp(before, &sim_flash_data[AREA], sizeof(before)) == 0 && sector_writes() == w && codeplug_kv_serial() == serial);
	codeplug_kv_op_t no_key[] = { { CODEPLUG_KEY_ARS_SERVER, CODEPLUG_KV_REMOVE, NULL }, { 0, 4, a4 } };
	CHECK(!codeplug_kv_write(no_key, 2) && sector_writes() == w);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));

	// Ops apply in order: a set then a remove of the same key leaves it absent
	codeplug_kv_op_t order[] = { { 0x7004, 4, a4 }, { 0x7004, CODEPLUG_KV_REMOVE, NULL } };
	CHECK(codeplug_kv_write(order, 2) && codeplug_kv_get(0x7004, a4, 4) == -1);

	// Nothing changed: no rewrite, the serial stays (a key set to its value, an absent key removed, an empty batch)
	w = sector_writes();
	serial = codeplug_kv_serial();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_IP_GATEWAY, 3100222));
	CHECK(codeplug_kv_remove(0x7123));
	CHECK(codeplug_kv_write(NULL, 0));
	CHECK(sector_writes() == w && codeplug_kv_serial() == serial);
}

static void test_remove(void)
{
	uint32_t id;
	uint16_t l, n;
	fresh();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 5));
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_IP_GATEWAY, 6));
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_ARS_SERVER));
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_IP_GATEWAY, &id) && id == 6);
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && n == 1 && l == 8);
	CHECK(sim_flash_data[AREA + 16] == 0x02 && sim_flash_data[AREA + 24] == 0xFF);	// the gap closed
	// The last key out: a valid area with no entries
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_IP_GATEWAY));
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && n == 0 && l == 0);
	CHECK(!codeplug_kv_remove(0) && !codeplug_kv_remove(0xFFFF));

	// Every entry of a key goes, a later duplicate too
	fresh();
	uint8_t d[32];
	uint16_t len = 0;
	len += entry(&d[len], CODEPLUG_KEY_ARS_SERVER, "\x01\x00\x00\x00", 4);
	len += entry(&d[len], 0x7123, "z", 1);
	len += entry(&d[len], CODEPLUG_KEY_ARS_SERVER, "\x02\x00\x00\x00", 4);
	raw_area(d, len, 1, 16, false);
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_ARS_SERVER));
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && n == 1 && l == 5);

	// Over an area that isn't valid, a remove has nothing to take out: no write, the bytes stay
	fresh();
	memset(&sim_flash_data[AREA], 0x00, 32);
	uint32_t w = sector_writes();
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_ARS_SERVER) && sector_writes() == w && sim_flash_data[AREA] == 0x00);
	// Nor over a newer one
	fresh();
	raw_area(d, len, 2, 16, false);
	CHECK(!codeplug_kv_remove(CODEPLUG_KEY_ARS_SERVER));
}

// A walk over a key range: checked once, entries in the area's order, a later duplicate skipped, ended by a change
static void test_walk(void)
{
	fresh();
	uint8_t d[96], v[8];
	uint16_t len = 0, key;
	len += entry(&d[len], 0x7105, "e", 1);
	len += entry(&d[len], CODEPLUG_KEY_ARS_SERVER, "\x01\x00\x00\x00", 4);
	len += entry(&d[len], 0x7100, "a", 1);
	len += entry(&d[len], 0x7105, "E", 1);	// a duplicate: no reader takes it
	len += entry(&d[len], 0x7200, "out", 3);
	len += entry(&d[len], 0x7102, "cc", 2);
	raw_area(d, len, 1, 16, false);

	codeplug_kv_cursor_t c;
	codeplug_kv_begin(&c, 0x7100, 0x71FF);
	CHECK(codeplug_kv_next(&c, &key, v, sizeof(v)) == 1 && key == 0x7105 && v[0] == 'e');
	CHECK(codeplug_kv_next(&c, &key, v, sizeof(v)) == 1 && key == 0x7100 && v[0] == 'a');
	CHECK(codeplug_kv_next(&c, &key, v, 1) == 2 && key == 0x7102 && v[0] == 'c');	// only max bytes copied
	CHECK(codeplug_kv_next(&c, &key, v, sizeof(v)) == -1);
	CHECK(codeplug_kv_next(&c, &key, v, sizeof(v)) == -1);		// stays at the end

	// The whole key space: 0x0000 and 0xFFFF are never keys
	unsigned count = 0;
	codeplug_kv_begin(&c, 0, 0xFFFF);
	while (codeplug_kv_next(&c, &key, v, sizeof(v)) >= 0) {
		count++;
	}
	CHECK(count == 5);
	codeplug_kv_begin(&c, 0, 0);
	CHECK(codeplug_kv_next(&c, &key, v, sizeof(v)) == -1);

	// The area rewritten during the walk: it ends rather than read old offsets
	codeplug_kv_begin(&c, 0x7100, 0x71FF);
	CHECK(codeplug_kv_next(&c, &key, v, sizeof(v)) == 1 && key == 0x7105);
	CHECK(codeplug_kv_set(0x7101, "b", 1));
	CHECK(codeplug_kv_next(&c, &key, v, sizeof(v)) == -1);

	// A walk over an area that isn't valid finds nothing
	sim_flash_data[AREA + 20] ^= 1;
	codeplug_kv_begin(&c, 0, 0xFFFF);
	CHECK(codeplug_kv_next(&c, &key, v, sizeof(v)) == -1);

	// The cost: the first call reads the whole area once, later ones only a header and entries
	fresh();
	for (uint16_t k = 0; k < 100; k++) {
		CHECK(codeplug_kv_set(0x7000 + k, "\x00\x00\x00\x00\x00\x00\x00\x00", 8));
	}
	codeplug_kv_begin(&c, 0x7050, 0x705F);
	CHECK(codeplug_kv_next(&c, &key, v, sizeof(v)) == 8 && key == 0x7050);
	uint32_t at = sim_flash_reads();
	count = 1;
	while (codeplug_kv_next(&c, &key, v, sizeof(v)) >= 0) {
		count++;
	}
	CHECK(count == 16);
	uint32_t walk = sim_flash_reads() - at;
	at = sim_flash_reads();
	CHECK(codeplug_kv_get(0x705F, v, sizeof(v)) == 8);
	uint32_t get = sim_flash_reads() - at;
	CHECK(walk < 4u * get);		// 15 more entries for less than four lookups' reads
}

// The raw write rule every transfer path applies (docs/spec/codeplug-settings.md 6)
static void test_keep(void)
{
	static uint8_t file[STORAGE_SECTOR];
	uint8_t d[16];
	uint16_t len = entry(d, CODEPLUG_KEY_ARS_SERVER, "\x07\x00\x00\x00", 4);
	fresh();
	memset(file, 0xFF, sizeof(file));
	CHECK(!codeplug_kv_keep(AREA, file));		// nothing to keep: the radio's area is erased
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1));
	CHECK(codeplug_kv_keep(AREA, file));		// erased over valid: kept
	CHECK(!codeplug_kv_keep(AREA + STORAGE_SECTOR, file) && !codeplug_kv_keep(AREA - STORAGE_SECTOR, file));
	memset(file, 0x00, 32);
	CHECK(codeplug_kv_keep(AREA, file));		// some other bytes over valid: kept
	// A valid file area: taken, an empty one too (erase every key)
	memcpy(file, &sim_flash_data[AREA], sizeof(file));
	CHECK(!codeplug_kv_keep(AREA, file));
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_ARS_SERVER));
	memcpy(file, &sim_flash_data[AREA], sizeof(file));
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1));
	CHECK(!codeplug_kv_keep(AREA, file));
	// A newer file area over a valid one: kept (we can't read it, the radio's keys stay)
	memcpy(file, &sim_flash_data[AREA], sizeof(file));
	file[4] = 2;
	CHECK(codeplug_kv_keep(AREA, file));
	// The radio's area newer: kept whatever comes
	raw_area(d, len, 2, 16, false);
	memset(file, 0xFF, sizeof(file));
	CHECK(codeplug_kv_keep(AREA, file));
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1) == false);
	raw_area(d, len, 1, 16, false);
	memcpy(file, &sim_flash_data[AREA], sizeof(file));
	raw_area(d, len, 2, 16, false);
	CHECK(codeplug_kv_keep(AREA, file));
	// A damaged radio area: the file's bytes go in, whatever they are
	raw_area(d, len, 1, 16, true);
	memset(file, 0xFF, sizeof(file));
	CHECK(!codeplug_kv_keep(AREA, file));
}

// A rewrite keeps the header as it found it (spec 2.4): its length, the continuation in bytes 12-15 and a later
// version's fields after them
static void test_header_kept(void)
{
	fresh();
	uint8_t d[16], *s = &sim_flash_data[AREA];
	uint16_t len = entry(d, CODEPLUG_KEY_ARS_SERVER, (const uint8_t[]){ 1, 0, 0, 0 }, 4);
	raw_area(d, len, CODEPLUG_KV_VERSION, 24, false);
	put32(&s[12], 0x04030201u);
	memset(&s[16], 0x5A, 8);
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_IP_GATEWAY, 7));
	CHECK(s[5] == 24);
	CHECK(s[12] == 1 && s[13] == 2 && s[14] == 3 && s[15] == 4);
	CHECK(s[16] == 0x5A && s[23] == 0x5A);
	uint32_t id;
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 1);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_IP_GATEWAY, &id) && id == 7);
}

// A power cut anywhere in a write leaves the old area or the new one (the journal finishes it at the next boot)
static void test_power_cut(void)
{
	fresh();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 111));
	uint32_t start = sim_flash_units();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 222));
	uint32_t total = sim_flash_units() - start;
	CHECK(total > 0);
	unsigned o = 0, nw = 0;
	for (uint32_t cut = 1; cut <= total; cut += total > 400 ? 37u : 1u) {
		fresh();
		CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 111));
		sim_flash_cut_after(cut);
		codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 222);
		sim_flash_power_on();
		storage_init();
		codeplug_init();
		uint32_t id = 0;
		bool ok = codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id);
		if (ok && id == 222) {
			nw++;
		} else if (ok && id == 111) {
			o++;
		} else {
			printf("FAIL cut after %u of %u units: %s %u\n", cut, total, ok ? "read" : "unreadable", id);
			failures++;
		}
	}
	CHECK(o > 0 && nw > 0);
}

int main(void)
{
	test_blank();
	test_edits();
	test_foreign();
	test_invalid();
	test_check();
	test_full();
	test_header_kept();
	test_batch();
	test_remove();
	test_walk();
	test_keep();
	test_power_cut();
	sim_flash_free();
	if (failures) {
		printf("codeplugKvTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("codeplugKvTest: all passed\n");
	return 0;
}
