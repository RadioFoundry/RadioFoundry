// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (core/codeplug/codeplug_kv.c, docs/spec/codeplug-settings.md) on a simulated NOR flash:
// reads of a blank, valid, invalid and newer area, writes that keep unknown keys and drop duplicates, values longer
// than a reader knows, a full area, and power cuts during a write
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
	uint32_t serial = codeplug_serial();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 3100999));
	CHECK(codeplug_serial() != serial);
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
	CHECK(!codeplug_kv_get_id(CODEPLUG_KEY_ARS_SERVER, &id));	// shorter than the u32: as if absent
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
	uint32_t serial = codeplug_serial();
	CHECK(!codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 9));
	CHECK(memcmp(before, &sim_flash_data[AREA], sizeof(before)) == 0 && codeplug_serial() == serial);
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
	test_full();
	test_power_cut();
	sim_flash_free();
	if (failures) {
		printf("codeplugKvTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("codeplugKvTest: all passed\n");
	return 0;
}
