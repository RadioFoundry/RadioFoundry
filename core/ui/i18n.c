// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The language tables (built by tools/i18n.py from the marks in core/ and core/ui/lang/*.po into the build's gen
// directory) and the lookup. A tr_id_t is a key's number (tr_keys.h): the keys the catalogs translate first, then the
// ones they never do (TRX). Each table keeps its strings in key order, NUL-terminated, "" for none, and where every
// LANG_STRIDE-th one starts: a lookup steps over at most LANG_STRIDE - 1 strings. The English table has every key; a
// language's only the translated ones, and one without a translation shows the English
#include <stddef.h>
#include <string.h>
#include "i18n.h"

typedef struct {
	const uint16_t *off;	// into text, for every LANG_STRIDE-th string
	const char *text;	// a NUL-terminated string per hash, "" for none
} lang_table_t;

typedef struct {
	const char *code;
	const char *name;
	bool built;
	bool wide;			// its catalog is UTF-8 with a CJK face: a line of it needs 16 px
	const lang_table_t *table;	// NULL: English, or not built
} lang_info_t;

#include "lang_tables.h"

static const lang_info_t langs[LANG_KNOWN] = LANG_LIST;
static uint8_t cur = LANG_DEFAULT;	// until the settings are applied

#if CONFIG_LANG_ANY
static const char *lookup(const lang_table_t *t, tr_id_t id)
{
	const char *s = t->text + t->off[id / LANG_STRIDE];
	for (uint16_t i = (uint16_t)(id % LANG_STRIDE); i > 0; i--)
		s += strlen(s) + 1u;
	return s;
}

const char *tr_en(tr_id_t id)
{
	if (id == TR_NONE)
		return NULL;
	if (id >= TR_KEYS)
		return "";	// not a key of this build: never made by a mark
	return lookup(&lang_en_table, id);
}

const char *tr(tr_id_t id)
{
	const lang_table_t *t = langs[cur].table;
	if (t != NULL && id != TR_NONE && id < TR_TRANSLATED) {
		const char *s = lookup(t, id);
		if (s[0])
			return s;
	}
	return tr_en(id);
}
#endif

uint8_t lang_count(void) { return LANG_KNOWN; }
const char *lang_code(uint8_t i) { return i < LANG_KNOWN ? langs[i].code : ""; }
const char *lang_name(uint8_t i) { return i < LANG_KNOWN ? langs[i].name : ""; }
bool lang_built(uint8_t i) { return i < LANG_KNOWN && langs[i].built; }
bool lang_wide(uint8_t i) { return i < LANG_KNOWN && langs[i].wide; }
uint8_t lang_current(void) { return cur; }

bool lang_set(uint8_t i)
{
	if (!lang_built(i))
		return false;
	cur = i;
	return true;
}

int32_t lang_value(uint8_t i)
{
	if (i >= LANG_KNOWN)
		return 0;
	return (int32_t)((uint8_t)langs[i].code[0] << 8 | (uint8_t)langs[i].code[1]);
}

uint8_t lang_find(int32_t value)
{
	if (value == 0)
		return LANG_DEFAULT;
	for (uint8_t i = 0; i < LANG_KNOWN; i++)
		if (lang_value(i) == value)
			return i;
	return LANG_ENGLISH;
}
