// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tables of settings kept as scalar keys of the codeplug's settings area (docs/spec/codeplug-settings.md 3, 5). An
// owner of settings (the radio core's data service, a UI block) has one table: a row per key, naming a field of the
// owner's settings struct. The owner reads its settings, applies the table and sets them back if anything changed, so
// a key costs a row and no code. Every table shares the walk and the write-back here
#ifndef RFOUNDRY_CODEPLUG_KEYS_H
#define RFOUNDRY_CODEPLUG_KEYS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug_kv.h"

typedef struct {
	uint16_t key;
	uint8_t type;		// CODEPLUG_T_UINT, _INT, _BOOL, _ENUM or _ID: how the value is encoded
	uint8_t size;		// the field's bytes: 1, 2 or 4 (a bool field is a C bool)
	uint16_t off;		// where the field sits in the owner's settings struct
	bool built;		// the build has the setting (its CONFIG switch); else the row is skipped
	uint32_t min, max;	// the setting's range (an int's as int32_t): a value outside it reads as if absent
} codeplug_key_row_t;

// A row for field f of struct s. The size and the offset come from the field, so they can't disagree with it
#define CODEPLUG_KEY_ROW(k, t, built, s, f, lo, hi) \
	{ (k), (t), (uint8_t)sizeof(((s *)0)->f), (uint16_t)offsetof(s, f), (built), (uint32_t)(lo), (uint32_t)(hi) }

// The most rows one table has: codeplug_keys_store's batch is on the caller's stack
#define CODEPLUG_KEYS_MAX	16

// The built row of key, or NULL
const codeplug_key_row_t *codeplug_keys_find(const codeplug_key_row_t *rows, uint8_t n, uint16_t key);
// A row's field in settings, as the number the key would hold (an int sign-extended, a bool 0 or 1)
uint32_t codeplug_keys_field(const codeplug_key_row_t *row, const void *settings);
// Whether value is one the row's setting takes (its range; a bool takes 0 and 1)
bool codeplug_keys_valid(const codeplug_key_row_t *row, uint32_t value);
// Set a row's field in settings to value (as codeplug_keys_field gives it)
void codeplug_keys_set_field(const codeplug_key_row_t *row, void *settings, uint32_t value);

// Put every key the codeplug has into settings: true if a field changed. An absent key, and one whose value the
// setting can't take, leaves its field as it is. One walk, so the area is checked once however many rows there are
bool codeplug_keys_apply(const codeplug_key_row_t *rows, uint8_t n, void *settings);

// Write the key of every field that differs between was and now, in one batch (all or nothing). The rule for every
// way the radio sets a keyed setting (docs/spec/codeplug-settings.md 5): true when the caller may go on and set now
// as the radio's own. That is when the keys were written, and also when they couldn't be (another firmware's
// codeplug, a CPS session holding the flash) while the codeplug holds none of them, since then no key would put the
// old value back. False when a key that is present couldn't be updated: the change must be refused
bool codeplug_keys_store(const codeplug_key_row_t *rows, uint8_t n, const void *was, const void *now);

#endif
