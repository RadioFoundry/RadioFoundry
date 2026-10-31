// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's languages (docs/dev/translating.md). Text the radio shows is written in English and marked:
//   TR("Zone")       the translation now, in the language the user picked (the English when there is none)
//   TRN("Zone")      only marks it (a static table's entry); whoever shows it calls tr() on it
// tools/i18n.py finds the marks and keeps core/ui/lang/<code>.po in step; make builds the tables from them. A
// translation can be longer than its English: copy it into a fixed buffer with str_copy (fmt.h) or sb_str, never
// strcpy. Without CONFIG_LANG_ANY everything here is the English and costs nothing
#ifndef UI_I18N_H
#define UI_I18N_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#define LANG_ENGLISH	0	// the index of English in the language list, always first and always built

#if CONFIG_LANG_ANY
const char *tr(const char *en);
#else
static inline const char *tr(const char *en) { return en; }
#endif
#define TR(s)	tr(s)
#define TRN(s)	(s)

// Every language a catalog exists for, English first, built into this image or not (Settings > Display > Language
// lists them all and dims the ones left out)
uint8_t lang_count(void);
const char *lang_code(uint8_t i);	// ISO 639-1: "en", "es"
const char *lang_name(uint8_t i);	// in the language itself: "Español"
bool lang_built(uint8_t i);
// The language in use; lang_set takes only a built one (false otherwise)
uint8_t lang_current(void);
bool lang_set(uint8_t i);
// A language as the settings store keeps it: its two code letters ('e' << 8 | 's'), 0 = English. lang_find returns the
// index of a stored value, LANG_ENGLISH for 0 or a language not in the list
int32_t lang_value(uint8_t i);
uint8_t lang_find(int32_t value);

#endif
