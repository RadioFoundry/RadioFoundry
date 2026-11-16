// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tables of keyed settings (codeplug_keys.h): the decoding by type, the one walk that applies a table and the batch
// that writes a change back
#include <string.h>
#include "codeplug_keys.h"

const codeplug_key_row_t *codeplug_keys_find(const codeplug_key_row_t *rows, uint8_t n, uint16_t key)
{
	for (uint8_t i = 0; i < n; i++) {
		if (rows[i].key == key && rows[i].built) {
			return &rows[i];
		}
	}
	return NULL;
}

static const uint8_t *field(const codeplug_key_row_t *row, const void *settings)
{
	return (const uint8_t *)settings + row->off;
}

uint32_t codeplug_keys_field(const codeplug_key_row_t *row, const void *settings)
{
	const uint8_t *f = field(row, settings);
	if (row->size == 1) {
		return row->type == CODEPLUG_T_INT ? (uint32_t)(int32_t)(int8_t)f[0] : f[0];
	}
	if (row->size == 2) {
		uint16_t v;
		memcpy(&v, f, sizeof(v));
		return row->type == CODEPLUG_T_INT ? (uint32_t)(int32_t)(int16_t)v : v;
	}
	uint32_t v;
	memcpy(&v, f, sizeof(v));
	return v;
}

void codeplug_keys_set_field(const codeplug_key_row_t *row, void *settings, uint32_t value)
{
	uint8_t *f = (uint8_t *)settings + row->off;
	if (row->size == 1) {
		f[0] = (uint8_t)value;
	} else if (row->size == 2) {
		uint16_t v = (uint16_t)value;
		memcpy(f, &v, sizeof(v));
	} else {
		memcpy(f, &value, sizeof(value));
	}
}

bool codeplug_keys_valid(const codeplug_key_row_t *row, uint32_t value)
{
	if (row->type == CODEPLUG_T_BOOL) {
		return value <= 1u;
	}
	if (row->type == CODEPLUG_T_INT) {
		return (int32_t)value >= (int32_t)row->min && (int32_t)value <= (int32_t)row->max;
	}
	return value >= row->min && value <= row->max;
}

// A value as read (its first min(len, CODEPLUG_KV_SCALAR) bytes) as the row's setting: false when the setting can't
// take it, which reads as an absent key (docs/spec/codeplug-settings.md 3.1)
static bool decode(const codeplug_key_row_t *row, const uint8_t *v, int len, uint32_t *value)
{
	if (row->type == CODEPLUG_T_INT) {
		int32_t x;
		if (!codeplug_kv_int(v, len, &x)) {
			return false;
		}
		*value = (uint32_t)x;
	} else if (row->type == CODEPLUG_T_BOOL) {
		if (len < 1 || len > CODEPLUG_KV_SCALAR) {
			return false;
		}
		*value = 0;
		for (int i = 0; i < len; i++) {
			*value |= v[i] != 0;	// anything but 0 is on
		}
	} else if (!codeplug_kv_uint(v, len, value)) {
		return false;
	}
	return codeplug_keys_valid(row, *value);
}

bool codeplug_keys_apply(const codeplug_key_row_t *rows, uint8_t n, void *settings)
{
	uint16_t lo = 0xFFFFu, hi = 0;
	for (uint8_t i = 0; i < n; i++) {
		if (rows[i].built) {
			lo = rows[i].key < lo ? rows[i].key : lo;
			hi = rows[i].key > hi ? rows[i].key : hi;
		}
	}
	if (lo > hi) {
		return false;	// no row in the build
	}
	bool changed = false;
	codeplug_kv_cursor_t c;
	uint16_t key;
	uint8_t v[CODEPLUG_KV_SCALAR];
	int len;
	codeplug_kv_begin(&c, lo, hi);
	while ((len = codeplug_kv_next(&c, &key, v, sizeof(v))) >= 0) {
		const codeplug_key_row_t *row = codeplug_keys_find(rows, n, key);
		uint32_t value;
		if (row != NULL && decode(row, v, len, &value) && codeplug_keys_field(row, settings) != value) {
			codeplug_keys_set_field(row, settings, value);
			changed = true;
		}
	}
	return changed;
}

bool codeplug_keys_store(const codeplug_key_row_t *rows, uint8_t n, const void *was, const void *now)
{
	// A value is the field's own bytes: every target is little endian, as the area is, and a C bool is 0 or 1
	_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "a field's bytes are written as the key's value");
	codeplug_kv_op_t ops[CODEPLUG_KEYS_MAX];
	uint8_t count = 0;
	for (uint8_t i = 0; i < n; i++) {
		if (!rows[i].built || codeplug_keys_field(&rows[i], now) == codeplug_keys_field(&rows[i], was)) {
			continue;
		}
		if (count == CODEPLUG_KEYS_MAX) {
			return false;
		}
		ops[count++] = (codeplug_kv_op_t){ rows[i].key, rows[i].size, field(&rows[i], now) };
	}
	if (count == 0 || codeplug_kv_write(ops, count)) {
		return true;
	}
	for (uint8_t i = 0; i < count; i++) {
		uint8_t v[1];
		if (codeplug_kv_get(ops[i].key, v, sizeof(v)) >= 0) {
			return false;
		}
	}
	return true;
}
