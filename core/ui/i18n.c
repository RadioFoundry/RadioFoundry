// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The language tables (built by tools/i18n.py from core/ui/lang/*.po into the build's gen directory) and the lookup:
// one sorted array of the FNV-1a hashes of the English any catalog translates (lang_hash), shared by the languages;
// each built language keeps its translations in that order, "" for none, and where every LANG_STRIDE-th one starts. A
// lookup hashes the English, searches, and steps over at most LANG_STRIDE - 1 strings; anything not found, or without
// a translation, is shown as it is
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
	const lang_table_t *table;	// NULL: English, or not built
} lang_info_t;

#include "lang_tables.h"

static const lang_info_t langs[LANG_KNOWN] = LANG_LIST;
static uint8_t cur = LANG_DEFAULT;	// until the settings are applied

#if CONFIG_LANG_ANY
const char *tr(const char *en)
{
	const lang_table_t *t = langs[cur].table;
	if (t == NULL || en == NULL || en[0] == 0)
		return en;
#ifdef LANG_STRINGS
	uint32_t h = 2166136261u;
	for (const char *p = en; *p; p++)
		h = (h ^ (uint8_t)*p) * 16777619u;
	uint16_t lo = 0, hi = LANG_STRINGS;
	while (lo < hi) {
		uint16_t mid = (uint16_t)((lo + hi) / 2u);
		if (lang_hash[mid] == h) {
			const char *s = t->text + t->off[mid / LANG_STRIDE];
			for (uint16_t i = mid % LANG_STRIDE; i > 0; i--)
				s += strlen(s) + 1u;
			return s[0] ? s : en;
		}
		if (lang_hash[mid] < h)
			lo = (uint16_t)(mid + 1u);
		else
			hi = mid;
	}
#endif
	return en;
}
#endif

uint8_t lang_count(void) { return LANG_KNOWN; }
const char *lang_code(uint8_t i) { return i < LANG_KNOWN ? langs[i].code : ""; }
const char *lang_name(uint8_t i) { return i < LANG_KNOWN ? langs[i].name : ""; }
bool lang_built(uint8_t i) { return i < LANG_KNOWN && langs[i].built; }
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
