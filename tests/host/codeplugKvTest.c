// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (core/codeplug/codeplug_kv.c, docs/spec/codeplug-settings.md) on a simulated NOR flash:
// reads of a blank, valid, invalid and newer area, writes that keep unknown keys and drop duplicates, values longer
// than a reader knows, scalars of every width, a record's fields set from the front, a full area, batches of sets and
// removes, walks over a key range, the raw write rule (codeplug_kv_keep, codeplug_kv_commit), the serial, and power
// cuts during a write; the continuation (docs/spec/codeplug-settings.md 2.4): the read order across sectors, a missing,
// erased, bad, newer or looping one, the header an older writer keeps, a batch that spans sectors, the byte table op,
// the raw write rule for its sector, and a power cut while it is made
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
#define CONT		(EE + CODEPLUG_KV_CONT_ADDR)

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
	len += entry(&d[len], CODEPLUG_KEY_IP_GATEWAY, gw, sizeof(gw));	// a number past 32 bits: no ID this reader holds
	len += entry(&d[len], CODEPLUG_KEY_IP_GATEWAY, four, sizeof(four));	// a duplicate: not read
	raw_area(d, len, 1, 20, false);
	uint32_t id;
	uint16_t l, n;
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && l == len && n == 3);
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_IP_GATEWAY, &id));	// read as absent, and kept
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

	// Version 0 was never written: a damaged byte, not a later firmware's area. Read as empty, and a write starts
	// over, so the area can't be stuck
	fresh();
	raw_area(d, len, 0, 16, false);
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_BAD && codeplug_kv_check(&sim_flash_data[AREA]) == CODEPLUG_KV_BAD);
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_IP_GATEWAY, 78));
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && n == 1);

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

// A scalar is 1 to 8 bytes, zero-extended; what a u32 reader can't hold reads as absent (spec 3.1)
static void test_scalar(void)
{
	uint32_t id;
	int32_t x;
	fresh();
	CHECK(codeplug_kv_set(CODEPLUG_KEY_ARS_SERVER, "\x39\x30", 2));
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 12345);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_ARS_SERVER, "\x39\x30\0\0\0\0\0\0", 8));	// a later version's width
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 12345);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_ARS_SERVER, "\x39\x30\0\0\0\0\0\0\0", 9));	// not a scalar
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_ARS_SERVER, "\x39\x30\0\0\x01", 5));	// past 32 bits
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_ARS_SERVER, "", 0));
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));
	// A scalar set replaces the whole value: the width it was stored in doesn't stay
	CHECK(codeplug_kv_set(CODEPLUG_KEY_ARS_SERVER, "\x39\x30\0\0\0\0\0\0", 8) && codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 7));
	uint8_t back[8];
	CHECK(codeplug_kv_get(CODEPLUG_KEY_ARS_SERVER, back, sizeof(back)) == 4);
	// An int sign-extends from its stored length
	CHECK(codeplug_kv_int((const uint8_t *)"\xFE", 1, &x) && x == -2);
	CHECK(codeplug_kv_int((const uint8_t *)"\xFE\x00", 2, &x) && x == 254);
	CHECK(codeplug_kv_int((const uint8_t *)"\x00\x00\x00\x80\xFF\xFF\xFF\xFF", 8, &x) && x == INT32_MIN);
	CHECK(!codeplug_kv_int((const uint8_t *)"\x00\x00\x00\x80\x00", 5, &x));	// +2^31
	CHECK(!codeplug_kv_int((const uint8_t *)"\xFF\xFF\xFF\x7F\xFF", 5, &x));	// below -2^31
	CHECK(!codeplug_kv_int((const uint8_t *)"", 0, &x));
}

// A record's fields set from the front (CODEPLUG_KV_HEAD): a writer that knows fewer fields than the value holds
// keeps the later ones (spec 4)
static void test_head(void)
{
	uint8_t back[8];
	fresh();
	// Absent: the fields become the value
	CHECK(codeplug_kv_set(0x7001, "\x01\x02", 2 | CODEPLUG_KV_HEAD));
	CHECK(codeplug_kv_get(0x7001, back, sizeof(back)) == 2 && memcmp(back, "\x01\x02", 2) == 0);
	// A longer stored value (a later version's): its first bytes set, the rest kept, the length too
	CHECK(codeplug_kv_set(0x7001, "\x01\x02\x03\x04\x05", 5));
	CHECK(codeplug_kv_set(0x7002, "\xAA", 1));
	uint32_t w = sector_writes();
	CHECK(codeplug_kv_set(0x7001, "\x09\x08", 2 | CODEPLUG_KV_HEAD));
	CHECK(codeplug_kv_get(0x7001, back, sizeof(back)) == 5 && memcmp(back, "\x09\x08\x03\x04\x05", 5) == 0);
	CHECK(codeplug_kv_get(0x7002, back, sizeof(back)) == 1 && back[0] == 0xAA && sector_writes() == w + 1);
	// The same fields again: nothing written
	uint8_t serial = codeplug_kv_serial();
	CHECK(codeplug_kv_set(0x7001, "\x09\x08", 2 | CODEPLUG_KV_HEAD) && sector_writes() == w + 1 &&
	      codeplug_kv_serial() == serial);
	// A writer that knows more fields than are stored: the value grows
	CHECK(codeplug_kv_set(0x7001, "\x01\x02\x03\x04\x05\x06", 6 | CODEPLUG_KV_HEAD));
	CHECK(codeplug_kv_get(0x7001, back, sizeof(back)) == 6 && back[5] == 6);
	// Without the flag the value is replaced whole
	CHECK(codeplug_kv_set(0x7001, "\x01", 1) && codeplug_kv_get(0x7001, back, sizeof(back)) == 1);
	uint16_t l, n;
	CHECK(codeplug_kv_info(&l, &n) == CODEPLUG_KV_VALID && n == 2);
}

// The serial moves whenever the keys may read differently: a key written, and every change of the codeplug (a raw
// write's reload, another firmware's codeplug judged again). The commit of a raw sector follows codeplug_kv_keep
static void test_serial_commit(void)
{
	static uint8_t file[STORAGE_SECTOR];
	uint32_t id;
	fresh();
	uint8_t serial = codeplug_kv_serial();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1) && codeplug_kv_serial() != serial);
	serial = codeplug_kv_serial();
	codeplug_reload();
	CHECK(codeplug_kv_serial() != serial);
	// A file without the area: acknowledged, the radio's sector stays
	memset(file, 0xFF, sizeof(file));
	uint32_t w = sector_writes();
	CHECK(codeplug_kv_commit(AREA, file) && sector_writes() == w);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 1);
	// A valid area in the file goes in; so does any other sector
	memcpy(file, &sim_flash_data[AREA], sizeof(file));
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 2));
	w = sector_writes();
	CHECK(codeplug_kv_commit(AREA, file) && sector_writes() == w + 1);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 1);
	memset(file, 0x12, sizeof(file));
	CHECK(codeplug_kv_commit(AREA + STORAGE_SECTOR, file) && sector_writes() == w + 2);
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

static uint32_t get32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A sector of the area as another tool would write it, at flash address at: header length 16, the pointer next
static void raw_sector(uint32_t at, const uint8_t *data, uint16_t len, uint8_t version, uint32_t next)
{
	uint8_t *s = &sim_flash_data[at];
	memset(s, 0xFF, STORAGE_SECTOR);
	put32(&s[0], CODEPLUG_KV_MAGIC);
	s[4] = version;
	s[5] = 16;
	put16(&s[6], len);
	put32(&s[8], storage_crc32(0, data, len));
	put32(&s[12], next);
	memcpy(&s[16], data, len);
}

// The keys of 0x7000-0x7FFF in the order a walk gives them: how many, into keys
static unsigned walk_keys(uint16_t *keys, unsigned max)
{
	codeplug_kv_cursor_t c;
	uint16_t k;
	uint8_t v[8];
	unsigned n = 0;
	codeplug_kv_begin(&c, 0x7000, 0x7FFF);
	while (codeplug_kv_next(&c, &k, v, sizeof(v)) >= 0 && n < max) {
		keys[n++] = k;
	}
	return n;
}

// Whether the sector at flash address at holds an entry with this key
static bool sector_has(uint32_t at, uint16_t key)
{
	const uint8_t *s = &sim_flash_data[at];
	uint16_t len = (uint16_t)(s[6] | (s[7] << 8));
	for (uint32_t e = 0; e < len; e += 4u + (uint32_t)(s[16 + e + 2] | (s[16 + e + 3] << 8))) {
		if ((uint16_t)(s[16 + e] | (s[16 + e + 1] << 8)) == key) {
			return true;
		}
	}
	return false;
}

// The first write to the continuation makes it and then links it from the first sector; later ones are one rewrite
static void test_cont_create(void)
{
	fresh();
	uint8_t name[4];
	uint32_t id;
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1234));
	const uint8_t *m = &sim_flash_data[AREA], *c = &sim_flash_data[CONT];
	CHECK(get32le(&m[12]) == 0);
	uint32_t w = sector_writes();
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME, "Club", 4));
	CHECK(sector_writes() == w + 2);					// the continuation, then the first sector's link
	CHECK(get32le(&m[12]) == CODEPLUG_KV_CONT_ADDR && m[6] == 8 && m[7] == 0);	// the pointer set, its entries as they were
	CHECK(codeplug_kv_check(m) == CODEPLUG_KV_VALID && codeplug_kv_check(c) == CODEPLUG_KV_VALID);
	CHECK(c[5] == 16 && c[6] == 8 && get32le(&c[12]) == 0 && c[16] == 0x00 && c[17] == 0x1E && c[18] == 4);
	CHECK(!sector_has(AREA, CODEPLUG_KEY_LIST_NAME) && !sector_has(CONT, CODEPLUG_KEY_ARS_SERVER));	// each in its own
	CHECK(codeplug_kv_get(CODEPLUG_KEY_LIST_NAME, name, sizeof(name)) == 4 && memcmp(name, "Club", 4) == 0);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 1234);
	// A later continuation key: one rewrite of the continuation, the link as it is
	w = sector_writes();
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME + 1, "Net", 3) && sector_writes() == w + 1);
	// A first sector key: one rewrite of it, the pointer kept (an older writer's too), the continuation untouched
	uint8_t kept[STORAGE_SECTOR];
	memcpy(kept, c, sizeof(kept));
	w = sector_writes();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_IP_GATEWAY, 7) && sector_writes() == w + 1);
	CHECK(get32le(&m[12]) == CODEPLUG_KV_CONT_ADDR && memcmp(kept, c, sizeof(kept)) == 0);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_LIST_NAME + 1, name, sizeof(name)) == 3);
	// The same value, and a remove of an absent key: nothing written
	w = sector_writes();
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME, "Club", 4) && codeplug_kv_remove(CODEPLUG_KEY_LIST_NAME + 9) && sector_writes() == w);
	// A remove of a key of the continuation before there is one makes none
	fresh();
	w = sector_writes();
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_LIST_NAME) && sector_writes() == w);
	CHECK(sim_flash_data[CONT] == 0xFF && sim_flash_data[AREA] == 0xFF);
	CHECK(sim_flash_violations() == 0);
}

// Reads take the first sector's keys before the continuation's; a key in both is the first's
static void test_cont_read(void)
{
	fresh();
	uint8_t a[32], b[32], v[8];
	uint16_t la = entry(a, 0x7001, "AA", 2), lb = entry(b, 0x7001, "BB", 2), keys[8];
	la = (uint16_t)(la + entry(a + la, 0x7003, "CC", 2));
	lb = (uint16_t)(lb + entry(b + lb, 0x7002, "DD", 2));
	raw_sector(AREA, a, la, CODEPLUG_KV_VERSION, CODEPLUG_KV_CONT_ADDR);
	raw_sector(CONT, b, lb, CODEPLUG_KV_VERSION, 0);
	CHECK(codeplug_kv_get(0x7001, v, sizeof(v)) == 2 && memcmp(v, "AA", 2) == 0);
	CHECK(codeplug_kv_get(0x7002, v, sizeof(v)) == 2 && memcmp(v, "DD", 2) == 0);	// only in the continuation
	CHECK(codeplug_kv_get(0x7003, v, sizeof(v)) == 2 && memcmp(v, "CC", 2) == 0);
	CHECK(walk_keys(keys, 8) == 3 && keys[0] == 0x7001 && keys[1] == 0x7003 && keys[2] == 0x7002);	// the duplicate skipped
	// A walk over only the continuation's range or key finds the same
	CHECK(codeplug_kv_read(0x7002, 1, v, 1) == 2 && v[0] == 'D');	// from byte 1 of the value
	CHECK(codeplug_kv_read(0x7002, 2, v, 1) == 2);				// off at the end: the length, nothing copied
	// The pointer: none, not a multiple of the sector, outside the image, to itself, to an erased sector
	static const uint32_t unreached[] = { 0, 0xF001, 0x10000, 0x20000, CODEPLUG_KV_ADDR, 0xB000 };
	for (unsigned i = 0; i < sizeof(unreached) / sizeof(unreached[0]); i++) {
		put32(&sim_flash_data[AREA + 12], unreached[i]);
		CHECK(codeplug_kv_get(0x7002, v, sizeof(v)) == -1);
		CHECK(codeplug_kv_get(0x7001, v, sizeof(v)) == 2 && memcmp(v, "AA", 2) == 0);
	}
	put32(&sim_flash_data[AREA + 12], CODEPLUG_KV_CONT_ADDR);
	CHECK(codeplug_kv_get(0x7002, v, sizeof(v)) == 2);
	// A continuation that is erased, damaged (the CRC), of a later version or without a valid first sector: no keys
	raw_sector(CONT, b, lb, CODEPLUG_KV_VERSION, 0);
	sim_flash_data[CONT + 17] ^= 1;
	CHECK(codeplug_kv_get(0x7002, v, sizeof(v)) == -1 && walk_keys(keys, 8) == 2);
	raw_sector(CONT, b, lb, CODEPLUG_KV_VERSION + 1, 0);
	CHECK(codeplug_kv_get(0x7002, v, sizeof(v)) == -1 && codeplug_kv_get(0x7001, v, sizeof(v)) == 2);
	memset(&sim_flash_data[CONT], 0xFF, STORAGE_SECTOR);
	CHECK(codeplug_kv_get(0x7002, v, sizeof(v)) == -1 && walk_keys(keys, 8) == 2);
	raw_sector(CONT, b, lb, CODEPLUG_KV_VERSION, 0);
	memset(&sim_flash_data[AREA], 0xFF, STORAGE_SECTOR);		// the first sector erased: nothing points to it
	CHECK(codeplug_kv_get(0x7002, v, sizeof(v)) == -1 && walk_keys(keys, 8) == 0);
}

// The chain a continuation can name: at most 8 sectors in all, and a loop ends at the sector it came back to
static void test_cont_chain(void)
{
	fresh();
	static const uint32_t order[] = { CODEPLUG_KV_ADDR, CODEPLUG_KV_CONT_ADDR, 0xB000, 0xC000, 0xD000, 0xA000, 0x9000, 0x8000, 0x7000 };
	uint8_t d[8], v[8];
	uint16_t keys[16];
	for (unsigned i = 0; i < 9; i++) {
		uint16_t len = entry(d, (uint16_t)(0x7100 + i), "x", 1);
		raw_sector(EE + order[i], d, len, CODEPLUG_KV_VERSION, i < 8 ? order[i + 1] : 0);
	}
	CHECK(walk_keys(keys, 16) == 8 && keys[0] == 0x7100 && keys[7] == 0x7107);	// the eighth is read, the ninth is not
	CHECK(codeplug_kv_get(0x7107, v, sizeof(v)) == 1 && codeplug_kv_get(0x7108, v, sizeof(v)) == -1);
	// A loop back to the first sector or to one in the chain
	put32(&sim_flash_data[EE + 0xB000 + 12], CODEPLUG_KV_ADDR);
	CHECK(walk_keys(keys, 16) == 3);
	put32(&sim_flash_data[EE + 0xB000 + 12], CODEPLUG_KV_CONT_ADDR);
	CHECK(walk_keys(keys, 16) == 3);
	// A sector in the chain that is bad ends it
	put32(&sim_flash_data[EE + 0xB000 + 12], 0xC000);
	sim_flash_data[EE + 0xC000 + 17] ^= 1;
	CHECK(walk_keys(keys, 16) == 3 && codeplug_kv_get(0x7103, v, sizeof(v)) == -1);
}

// The continuation's header is kept as found by a rewrite (an older or later writer's fields), and the first sector's
// pointer by a write of a first sector key
static void test_cont_header_kept(void)
{
	fresh();
	uint8_t d[16], hdr_extra[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint16_t len = entry(d, CODEPLUG_KEY_LIST_NAME, "Old", 3);
	raw_sector(CONT, d, len, CODEPLUG_KV_VERSION, 0x0B000);			// another sector named: kept as found
	// Header length 24: the entries start at 24
	uint8_t *c = &sim_flash_data[CONT];
	memmove(&c[24], &c[16], len);
	memcpy(&c[16], hdr_extra, 8);
	c[5] = 24;
	put32(&c[8], storage_crc32(0, &c[24], len));
	raw_sector(AREA, (const uint8_t[]){ 0 }, 0, CODEPLUG_KV_VERSION, CODEPLUG_KV_CONT_ADDR);
	CHECK(codeplug_kv_check(c) == CODEPLUG_KV_VALID);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME + 1, "New", 3));
	CHECK(c[5] == 24 && get32le(&c[12]) == 0xB000 && memcmp(&c[16], hdr_extra, 8) == 0);
	uint8_t v[8];
	CHECK(codeplug_kv_get(CODEPLUG_KEY_LIST_NAME, v, sizeof(v)) == 3 && memcmp(v, "Old", 3) == 0);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_LIST_NAME + 1, v, sizeof(v)) == 3 && memcmp(v, "New", 3) == 0);
	CHECK(get32le(&sim_flash_data[AREA + 12]) == CODEPLUG_KV_CONT_ADDR);
}

// A batch of keys of both sectors is written sector by sector; what doesn't fit stops it before any is written, a
// copy of a key a writer sets in the sector after is dropped when that sector is written anyway
static void test_cont_batch(void)
{
	static uint8_t big[STORAGE_SECTOR];
	uint8_t v4[4] = { 1, 0, 0, 0 }, v[8];
	uint32_t id;
	uint16_t room = STORAGE_SECTOR - CODEPLUG_KV_HDR;
	memset(big, 0x5A, sizeof(big));
	fresh();
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME, "A", 1));
	// Both fit: both written, the continuation first
	codeplug_kv_op_t both[] = { { CODEPLUG_KEY_ARS_SERVER, 4, v4 }, { CODEPLUG_KEY_LIST_NAME + 1, 1, "B" } };
	uint32_t w = sector_writes();
	CHECK(codeplug_kv_write(both, 2) && sector_writes() == w + 2);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 1 && codeplug_kv_get(CODEPLUG_KEY_LIST_NAME + 1, v, sizeof(v)) == 1);
	// The continuation's part too big: nothing written, not even the first sector's
	codeplug_kv_op_t big_cont[] = { { CODEPLUG_KEY_ARS_SERVER, 4, "\x02\0\0\0" }, { CODEPLUG_KEY_LIST_NAME + 2, (uint16_t)(room - 3), big } };
	w = sector_writes();
	CHECK(!codeplug_kv_write(big_cont, 2) && sector_writes() == w);
	CHECK(codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) && id == 1 && codeplug_kv_get(CODEPLUG_KEY_LIST_NAME + 2, v, sizeof(v)) == -1);
	// The first sector's part too big: the continuation isn't written either
	CHECK(codeplug_kv_set(0x7000, big, (uint16_t)(room - 4u - 8u)));
	codeplug_kv_op_t big_main[] = { { 0x7001, 16, big }, { CODEPLUG_KEY_LIST_NAME + 3, 1, "D" } };
	w = sector_writes();
	CHECK(!codeplug_kv_write(big_main, 2) && sector_writes() == w);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_LIST_NAME + 3, v, sizeof(v)) == -1 && codeplug_kv_get(0x7001, v, sizeof(v)) == -1);
	// Without the continuation, a batch of its keys alone that doesn't fit changes nothing and makes none
	fresh();
	codeplug_kv_op_t lone[] = { { CODEPLUG_KEY_LIST_NAME, (uint16_t)(room - 3), big } };
	CHECK(!codeplug_kv_write(lone, 1) && sim_flash_data[CONT] == 0xFF && sim_flash_data[AREA] == 0xFF);
	// A key in both sectors: the writer's set drops the later copy when the continuation is written anyway
	fresh();
	uint8_t a[16], b[16];
	uint16_t la = entry(a, 0x7001, "AA", 2), lb = entry(b, 0x7001, "BB", 2);
	lb = (uint16_t)(lb + entry(b + lb, CODEPLUG_KEY_LIST_NAME, "L", 1));
	raw_sector(AREA, a, la, CODEPLUG_KV_VERSION, CODEPLUG_KV_CONT_ADDR);
	raw_sector(CONT, b, lb, CODEPLUG_KV_VERSION, 0);
	codeplug_kv_op_t twice[] = { { 0x7001, 2, "ZZ" }, { CODEPLUG_KEY_LIST_NAME + 1, 1, "M" } };
	CHECK(codeplug_kv_write(twice, 2));
	CHECK(sector_has(AREA, 0x7001) && !sector_has(CONT, 0x7001) && sector_has(CONT, CODEPLUG_KEY_LIST_NAME));
	CHECK(codeplug_kv_get(0x7001, v, sizeof(v)) == 2 && memcmp(v, "ZZ", 2) == 0);
	// A first sector of a later version: nothing is written, the continuation included
	fresh();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1));
	sim_flash_data[AREA + 4] = CODEPLUG_KV_VERSION + 1;
	CHECK(!codeplug_kv_set(CODEPLUG_KEY_LIST_NAME, "A", 1) && sim_flash_data[CONT] == 0xFF);
}

// A byte table (CODEPLUG_KV_TABLE): bytes set at an offset, the value grown with 0xFF and cut of trailing 0xFF
static void test_table(void)
{
	uint8_t v[16];
	fresh();
	codeplug_kv_op_t at5 = { 0x2000, 1 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 5, 0, 7 } };
	CHECK(codeplug_kv_write(&at5, 1));
	CHECK(codeplug_kv_get(0x2000, v, sizeof(v)) == 6 && v[0] == 0xFF && v[4] == 0xFF && v[5] == 7);
	codeplug_kv_op_t three = { 0x2000, 3 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 1, 0, 1, 2, 3 } };
	CHECK(codeplug_kv_write(&three, 1));
	CHECK(codeplug_kv_get(0x2000, v, sizeof(v)) == 6 && v[0] == 0xFF && v[1] == 1 && v[2] == 2 && v[3] == 3 && v[5] == 7);
	uint32_t w = sector_writes();
	CHECK(codeplug_kv_write(&three, 1) && sector_writes() == w);			// the same bytes: nothing written
	codeplug_kv_op_t clear5 = { 0x2000, 1 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 5, 0, 0xFF } };
	CHECK(codeplug_kv_write(&clear5, 1) && codeplug_kv_get(0x2000, v, sizeof(v)) == 4);	// 0xFF at the end: cut
	codeplug_kv_op_t far = { 0x2000, 1 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 20, 0, 0xFF } };
	w = sector_writes();
	CHECK(codeplug_kv_write(&far, 1) && sector_writes() == w);			// a 0xFF past the end: still nothing
	codeplug_kv_op_t clear123 = { 0x2000, 3 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 1, 0, 0xFF, 0xFF, 0xFF } };
	CHECK(codeplug_kv_write(&clear123, 1) && codeplug_kv_get(0x2000, v, sizeof(v)) == -1);	// all 0xFF: removed
	codeplug_kv_op_t absent = { 0x2001, 1 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 0, 0, 0xFF } };
	w = sector_writes();
	CHECK(codeplug_kv_write(&absent, 1) && sector_writes() == w && codeplug_kv_get(0x2001, v, sizeof(v)) == -1);
	// Malformed: no bytes, none to read, past the sector, with the head flag
	codeplug_kv_op_t none = { 0x2000, CODEPLUG_KV_TABLE, (const uint8_t[]){ 0, 0 } };
	codeplug_kv_op_t late = { 0x2000, 1 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 0xFF, 0x0F, 1 } };
	codeplug_kv_op_t both = { 0x2000, 1 | CODEPLUG_KV_TABLE | CODEPLUG_KV_HEAD, (const uint8_t[]){ 0, 0, 1 } };
	codeplug_kv_op_t null = { 0x2000, 1 | CODEPLUG_KV_TABLE, NULL };
	CHECK(!codeplug_kv_write(&none, 1) && !codeplug_kv_write(&late, 1) && !codeplug_kv_write(&both, 1) && !codeplug_kv_write(&null, 1));
	// Two tables and a scalar in one batch of the continuation: one rewrite
	codeplug_kv_op_t mix[] = {
		{ 0x2000, 1 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 3, 0, 4 } },
		{ 0x2001, 1 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 0, 0, 5 } },
		{ 0x2003, 4, "\x01\x02\x03\x04" },
	};
	w = sector_writes();
	CHECK(codeplug_kv_write(mix, 3) && sector_writes() == w + 1);
	CHECK(codeplug_kv_get(0x2000, v, sizeof(v)) == 4 && codeplug_kv_get(0x2001, v, sizeof(v)) == 1 && codeplug_kv_get(0x2003, v, sizeof(v)) == 4);
	// A table that grows past the room changes nothing
	static uint8_t big[STORAGE_SECTOR];
	memset(big, 0x5A, sizeof(big));
	uint16_t used = 8u + 5u + 8u;							// the three keys set above, with their framing
	CHECK(codeplug_kv_set(0x2004, big, (uint16_t)(STORAGE_SECTOR - CODEPLUG_KV_HDR - used - 4u - 8u)));	// 8 bytes of room left
	codeplug_kv_op_t grow = { 0x2000, 1 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 0xE8, 0x03, 1 } };
	w = sector_writes();
	CHECK(!codeplug_kv_write(&grow, 1) && sector_writes() == w && codeplug_kv_get(0x2000, v, sizeof(v)) == 4);
}

// The raw write rule for the continuation's sector, judged for itself (codeplug-settings.md 6)
static void test_cont_keep(void)
{
	static uint8_t file[STORAGE_SECTOR];
	uint8_t v[8];
	fresh();
	memset(file, 0xFF, sizeof(file));
	CHECK(!codeplug_kv_keep(CONT, file));			// nothing to keep: the radio's continuation is erased
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME, "Mine", 4));
	CHECK(codeplug_kv_keep(CONT, file));			// erased over valid: kept
	uint32_t w = sector_writes();
	CHECK(codeplug_kv_commit(CONT, file) && sector_writes() == w);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_LIST_NAME, v, sizeof(v)) == 4 && memcmp(v, "Mine", 4) == 0);
	// The first sector is judged on its own: a valid file one goes in while the continuation stays
	static uint8_t first[STORAGE_SECTOR];
	memcpy(first, &sim_flash_data[AREA], sizeof(first));
	CHECK(!codeplug_kv_keep(AREA, first) && codeplug_kv_keep(CONT, file));
	// A valid continuation in the file: taken
	memcpy(file, &sim_flash_data[CONT], sizeof(file));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME, "Else", 4));
	CHECK(!codeplug_kv_keep(CONT, file));
	w = sector_writes();
	CHECK(codeplug_kv_commit(CONT, file) && sector_writes() == w + 1);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_LIST_NAME, v, sizeof(v)) == 4 && memcmp(v, "Mine", 4) == 0);
	// A damaged file continuation or a newer one over a valid one: kept
	file[16] ^= 1;
	CHECK(codeplug_kv_keep(CONT, file));
	memcpy(file, &sim_flash_data[CONT], sizeof(file));
	file[4] = CODEPLUG_KV_VERSION + 1;
	CHECK(codeplug_kv_keep(CONT, file));
	// The radio's continuation of a later version: kept whatever comes
	sim_flash_data[CONT + 4] = CODEPLUG_KV_VERSION + 1;
	memset(file, 0xFF, sizeof(file));
	CHECK(codeplug_kv_keep(CONT, file));
	// A damaged radio continuation: the file's bytes go in
	sim_flash_data[CONT + 4] = CODEPLUG_KV_VERSION;
	sim_flash_data[CONT + 17] ^= 1;
	CHECK(!codeplug_kv_keep(CONT, file));
}

// A power cut anywhere while the first continuation is made (its sector, then the link): the first sector's keys stay,
// the list name is absent or whole, and the next write goes through
static void test_cont_power_cut(void)
{
	fresh();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 111));
	uint32_t start = sim_flash_units();
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME, "Club", 4));
	uint32_t total = sim_flash_units() - start;
	CHECK(total > 0);
	unsigned absent = 0, whole = 0;
	for (uint32_t cut = 1; cut <= total; cut += total > 400 ? 37u : 1u) {
		fresh();
		CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 111));
		sim_flash_cut_after(cut);
		codeplug_kv_set(CODEPLUG_KEY_LIST_NAME, "Club", 4);
		sim_flash_power_on();
		storage_init();
		codeplug_init();
		uint32_t id = 0;
		uint8_t v[8];
		int n = codeplug_kv_get(CODEPLUG_KEY_LIST_NAME, v, sizeof(v));
		if (!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id) || id != 111 || (n != -1 && (n != 4 || memcmp(v, "Club", 4) != 0))) {
			printf("FAIL continuation cut after %u of %u units: id %u name length %d\n", cut, total, id, n);
			failures++;
		}
		absent += n == -1;
		whole += n == 4;
		CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME, "Club", 4) && codeplug_kv_get(CODEPLUG_KEY_LIST_NAME, v, sizeof(v)) == 4);
	}
	CHECK(absent > 0 && whole > 0);
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
	test_scalar();
	test_head();
	test_serial_commit();
	test_power_cut();
	test_cont_create();
	test_cont_read();
	test_cont_chain();
	test_cont_header_kept();
	test_cont_batch();
	test_table();
	test_cont_keep();
	test_cont_power_cut();
	sim_flash_free();
	if (failures) {
		printf("codeplugKvTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("codeplugKvTest: all passed\n");
	return 0;
}
