// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tables of settings-area keys (core/codeplug/codeplug_keys.c, docs/spec/codeplug-settings.md 3, 5) on a fake settings
// area: lookup, every scalar type and field width, a value the setting can't take (reads as absent), apply (present
// holds, absent leaves), the write-back of a change and its rule when the area can't be written; then the radio
// core's table (core/radio/radio_keys.h)
#include <stdio.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_keys.h"
#include "radio/radio_keys.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- fake settings area: one slot per key, a flag for "the area can't be written" ----
#define SLOTS	16
static struct { bool present; uint8_t len; uint8_t v[12]; } kv[SLOTS];
static bool kv_held;
static int kv_writes;	// batches written
static int kv_walks;	// walks begun

static void put(uint16_t key, const void *v, uint8_t len)
{
	kv[key].present = true;
	kv[key].len = len;
	memcpy(kv[key].v, v, len);
}

static void put_u32(uint16_t key, uint32_t x)
{
	put(key, &x, 4);
}

static uint32_t stored(uint16_t key)
{
	uint32_t x = 0;
	memcpy(&x, kv[key].v, kv[key].len < 4 ? kv[key].len : 4);
	return x;
}

int codeplug_kv_get(uint16_t key, void *buf, uint16_t max)
{
	if (key >= SLOTS || !kv[key].present) {
		return -1;
	}
	memcpy(buf, kv[key].v, max < kv[key].len ? max : kv[key].len);
	return kv[key].len;
}

void codeplug_kv_begin(codeplug_kv_cursor_t *c, uint16_t lo, uint16_t hi)
{
	c->at = lo;
	c->hi = hi;
	kv_walks++;
}

int codeplug_kv_next(codeplug_kv_cursor_t *c, uint16_t *key, void *buf, uint16_t max)
{
	for (; c->at <= c->hi && c->at < SLOTS; c->at++) {
		if (kv[c->at].present) {
			*key = c->at++;
			return codeplug_kv_get(*key, buf, max);
		}
	}
	return -1;
}

bool codeplug_kv_write(const codeplug_kv_op_t *ops, uint8_t n)
{
	// all or nothing, as the real one: every op checked before any is applied
	for (uint8_t i = 0; i < n; i++) {
		if (kv_held || ops[i].key >= SLOTS || ops[i].len > sizeof(kv[0].v)) {
			return false;
		}
	}
	for (uint8_t i = 0; i < n; i++) {
		put(ops[i].key, ops[i].value, (uint8_t)ops[i].len);
	}
	kv_writes++;
	return true;
}

// ---- a table with every scalar type and field width ----
typedef struct {
	uint8_t tries;		// uint, 1..10
	bool tone;		// bool
	uint8_t mode;		// enum, 0..2
	int16_t trim;		// int, -1000..1000
	uint16_t port;		// uint, a u16 field
	uint32_t target;	// id
	uint8_t off;		// built out
} owner_t;

enum { K_TRIES = 3, K_TONE, K_MODE, K_TRIM, K_PORT, K_TARGET, K_OFF };

static const codeplug_key_row_t rows[] = {
	CODEPLUG_KEY_ROW(K_TRIES, CODEPLUG_T_UINT, true, owner_t, tries, 1, 10),
	CODEPLUG_KEY_ROW(K_TONE, CODEPLUG_T_BOOL, true, owner_t, tone, 0, 1),
	CODEPLUG_KEY_ROW(K_MODE, CODEPLUG_T_ENUM, true, owner_t, mode, 0, 2),
	CODEPLUG_KEY_ROW(K_TRIM, CODEPLUG_T_INT, true, owner_t, trim, -1000, 1000),
	CODEPLUG_KEY_ROW(K_PORT, CODEPLUG_T_UINT, true, owner_t, port, 0, 65535),
	CODEPLUG_KEY_ROW(K_TARGET, CODEPLUG_T_ID, true, owner_t, target, 0, CODEPLUG_DMR_ID_MAX),
	CODEPLUG_KEY_ROW(K_OFF, CODEPLUG_T_UINT, false, owner_t, off, 0, 255),
};
#define ROWS	((uint8_t)(sizeof(rows) / sizeof(rows[0])))

static const owner_t own = { .tries = 4, .tone = true, .mode = 1, .trim = -20, .port = 14580, .target = 9, .off = 7 };

static bool same(const owner_t *a, const owner_t *b)
{
	return a->tries == b->tries && a->tone == b->tone && a->mode == b->mode && a->trim == b->trim &&
	       a->port == b->port && a->target == b->target && a->off == b->off;
}

static void test_types(void)
{
	owner_t s = own;
	memset(kv, 0, sizeof(kv));
	CHECK(codeplug_keys_find(rows, ROWS, K_TRIM) == &rows[3] && codeplug_keys_find(rows, ROWS, K_OFF) == NULL &&
	      codeplug_keys_find(rows, ROWS, 15) == NULL);
	CHECK(codeplug_keys_field(&rows[3], &s) == (uint32_t)-20 && codeplug_keys_field(&rows[1], &s) == 1 &&
	      codeplug_keys_field(&rows[4], &s) == 14580);
	CHECK(codeplug_keys_valid(&rows[0], 10) && !codeplug_keys_valid(&rows[0], 0) && !codeplug_keys_valid(&rows[0], 11));
	CHECK(codeplug_keys_valid(&rows[3], (uint32_t)-1000) && !codeplug_keys_valid(&rows[3], (uint32_t)-1001));
	CHECK(!codeplug_keys_valid(&rows[1], 2));

	// nothing present: nothing changes, in one walk
	kv_walks = 0;
	CHECK(!codeplug_keys_apply(rows, ROWS, &s) && same(&s, &own) && kv_walks == 1);

	// each type from a one-byte value, the narrowest a tool may write
	put(K_TRIES, (uint8_t[]){ 10 }, 1);
	put(K_TONE, (uint8_t[]){ 0 }, 1);
	put(K_MODE, (uint8_t[]){ 2 }, 1);
	put(K_TRIM, (uint8_t[]){ 0xFB }, 1);		// -5, sign-extended from one byte
	put(K_PORT, (uint8_t[]){ 80 }, 1);
	put(K_TARGET, (uint8_t[]){ 0 }, 1);		// 0 is a value: off
	put(K_OFF, (uint8_t[]){ 1 }, 1);		// a row the build doesn't have: skipped
	CHECK(codeplug_keys_apply(rows, ROWS, &s));
	CHECK(s.tries == 10 && !s.tone && s.mode == 2 && s.trim == -5 && s.port == 80 && s.target == 0 && s.off == 7);
	CHECK(!codeplug_keys_apply(rows, ROWS, &s));	// nothing left to change

	// a wider value than the field: taken while it fits (a setting that outgrew its width, stored in more bytes)
	s = own;
	put(K_TRIES, (uint8_t[]){ 7, 0, 0, 0, 0, 0, 0, 0 }, 8);
	put(K_TONE, (uint8_t[]){ 0, 0, 4, 0 }, 4);	// a bool: anything but 0 is on
	put(K_TRIM, (uint8_t[]){ 0x18, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, 8);	// -1000 in 8 bytes
	put(K_PORT, (uint8_t[]){ 0xFF, 0xFF, 0, 0 }, 4);
	put_u32(K_TARGET, CODEPLUG_DMR_ID_MAX);
	memset(&kv[K_MODE], 0, sizeof(kv[0]));
	s.tone = false;
	CHECK(codeplug_keys_apply(rows, ROWS, &s));
	CHECK(s.tries == 7 && s.tone && s.mode == 1 && s.trim == -1000 && s.port == 65535 &&
	      s.target == CODEPLUG_DMR_ID_MAX);

	// a value the setting can't take reads as if the key were absent: the radio keeps its own
	s = own;
	put(K_TRIES, (uint8_t[]){ 0 }, 1);		// below the range
	put(K_TONE, (uint8_t[]){ 0 }, 0);		// empty
	put(K_MODE, (uint8_t[]){ 3 }, 1);		// an enum value this firmware doesn't know
	put(K_TRIM, (uint8_t[]){ 0xE9, 0x03 }, 2);	// 1001
	put(K_PORT, (uint8_t[]){ 0, 0, 1, 0 }, 4);	// past the field
	put_u32(K_TARGET, CODEPLUG_DMR_ID_MAX + 1);
	CHECK(!codeplug_keys_apply(rows, ROWS, &s) && same(&s, &own));
	put(K_TRIES, (uint8_t[]){ 200 }, 1);		// above the range
	put(K_MODE, (uint8_t[]){ 1, 0, 0, 0, 1, 0, 0, 0 }, 8);		// past 32 bits
	put(K_TRIM, (uint8_t[]){ 0, 0, 0, 0x80, 0, 0, 0, 0 }, 8);	// a positive past 32 bits
	put(K_PORT, (uint8_t[]){ 1, 0, 0, 0, 0, 0, 0, 0, 0 }, 9);	// longer than a scalar
	put(K_TARGET, (uint8_t[]){ 9, 0, 0, 0, 1 }, 5);
	CHECK(!codeplug_keys_apply(rows, ROWS, &s) && same(&s, &own));
}

static void test_store(void)
{
	owner_t was = own, now = own;
	memset(kv, 0, sizeof(kv));
	kv_writes = 0;
	// no change: nothing written
	CHECK(codeplug_keys_store(rows, ROWS, &was, &now) && kv_writes == 0);
	// each changed field goes out in its own width, one batch; a field built out and the unchanged ones don't
	now.tries = 9;
	now.tone = false;
	now.trim = -300;
	now.target = 3100444;
	now.off = 1;
	CHECK(codeplug_keys_store(rows, ROWS, &was, &now) && kv_writes == 1);
	CHECK(kv[K_TRIES].len == 1 && kv[K_TRIES].v[0] == 9 && kv[K_TONE].len == 1 && kv[K_TONE].v[0] == 0);
	CHECK(kv[K_TRIM].len == 2 && kv[K_TRIM].v[0] == 0xD4 && kv[K_TRIM].v[1] == 0xFE);
	CHECK(kv[K_TARGET].len == 4 && stored(K_TARGET) == 3100444);
	CHECK(!kv[K_MODE].present && !kv[K_PORT].present && !kv[K_OFF].present);
	// what was written reads back as it was set
	owner_t back = own;
	CHECK(codeplug_keys_apply(rows, ROWS, &back));
	back.off = now.off;
	CHECK(same(&back, &now));

	// the area can't be written: refused while the codeplug has one of the changed keys, since it would put the old
	// value back; with none of them the change is the radio's own
	kv_held = true;
	was = now;
	now.mode = 2;
	CHECK(codeplug_keys_store(rows, ROWS, &was, &now));	// K_MODE isn't there
	now.tries = 2;
	CHECK(!codeplug_keys_store(rows, ROWS, &was, &now));	// K_TRIES is
	CHECK(kv[K_TRIES].v[0] == 9 && !kv[K_MODE].present);
	kv_held = false;
	CHECK(codeplug_keys_store(rows, ROWS, &was, &now) && kv[K_TRIES].v[0] == 2 && kv[K_MODE].v[0] == 2);
}

static void test_radio_keys(void)
{
	memset(kv, 0, sizeof(kv));
	// the rows are unique, known keys of the assigned range
	for (size_t i = 0; i < RADIO_KEYS_N; i++) {
		CHECK(radio_keys[i].key >= 1 && radio_keys[i].key <= 0x7FFF);
		for (size_t j = i + 1; j < RADIO_KEYS_N; j++) {
			CHECK(radio_keys[i].key != radio_keys[j].key);
		}
	}
	CHECK(codeplug_keys_find(radio_keys, RADIO_KEYS_N, 0x7FFE) == NULL);
	const codeplug_key_row_t *ars = codeplug_keys_find(radio_keys, RADIO_KEYS_N, CODEPLUG_KEY_ARS_SERVER);
	const codeplug_key_row_t *gw = codeplug_keys_find(radio_keys, RADIO_KEYS_N, CODEPLUG_KEY_IP_GATEWAY);
	CHECK((CONFIG_DMR_ARS) == (ars != NULL) && (CONFIG_DMR_IP_GATEWAY) == (gw != NULL));
	if (ars == NULL || gw == NULL) {
		return;
	}

	dmrDataSettings_t set, was;
	memset(&set, 0, sizeof(set));
	set.arsServer = 3100111;
	set.ipGatewayId = 5;
	CHECK(codeplug_keys_field(ars, &set) == 3100111 && codeplug_keys_field(gw, &set) == 5);

	// absent keys leave the settings alone
	CHECK(!codeplug_keys_apply(radio_keys, RADIO_KEYS_N, &set) && set.arsServer == 3100111 && set.ipGatewayId == 5);
	// a present key holds its setting, 0 included; the other stays
	put_u32(CODEPLUG_KEY_ARS_SERVER, 0);
	CHECK(codeplug_keys_apply(radio_keys, RADIO_KEYS_N, &set) && set.arsServer == 0 && set.ipGatewayId == 5);
	// an ID above the maximum reads as absent
	put_u32(CODEPLUG_KEY_IP_GATEWAY, CODEPLUG_DMR_ID_MAX + 1);
	CHECK(!codeplug_keys_apply(radio_keys, RADIO_KEYS_N, &set) && set.ipGatewayId == 5);

	// two settings changed at once: one batch, both keys
	memset(kv, 0, sizeof(kv));
	was = set;
	set.arsServer = 3100444;
	set.ipGatewayId = 3100555;
	kv_writes = 0;
	CHECK(codeplug_keys_store(radio_keys, RADIO_KEYS_N, &was, &set) && kv_writes == 1 &&
	      stored(CODEPLUG_KEY_ARS_SERVER) == 3100444 && stored(CODEPLUG_KEY_IP_GATEWAY) == 3100555);
}

int main(void)
{
	test_types();
	test_store();
	test_radio_keys();
	printf("%d checks, %d failed\n", checks, failures);
	return failures != 0;
}
