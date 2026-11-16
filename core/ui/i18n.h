// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's languages (docs/dev/translating.md). Text the radio shows is written in English and marked with a key, a
// C identifier that names it in the catalogs:
//   TR(ZONE_TITLE, "Zone")      the text now, in the language the user picked (the English when there is none)
//   TRN(ZONE_TITLE, "Zone")     a tr_id_t for a static table's entry or a struct field; call tr() in order to show it
//   TRX(CHAN_LABEL_RX, "RX")    a tr_id_t representation, like TRN, but for non-localizable text (never translated)
//   TRF(&b, MSG_BANNER_FROM, "Message from %s", name)
//                               text with values, appended to an sbuf_t (fmt.h: sb_fmt and its %s %K %d %u, numbered
//                               %1$s %2$u when there are several). A translation may move the values. The compiler
//                               checks the values against the English: tools/i18n.py writes a prototype for the key
//                               into tr_keys.h
// The English at the mark is the source of truth: tools/i18n.py reads it from there. One key always has the same
// English; the same English may have more than one key.
// A tr_id_t is opaque: pass it to tr() or tr_en(), compare it with TR_NONE (the zero value: a memset or an omitted
// initializer gives it), and nothing else. With a translation built it is a number (TR_<KEY>, from the generated
// tr_keys.h); without one it is the English itself and costs nothing. Callers compile unchanged either way.
// A translation can be longer than its English: copy it into a fixed buffer with str_copy (fmt.h) or sb_str, never
// strcpy, and never compare shown text
#ifndef UI_I18N_H
#define UI_I18N_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#define LANG_ENGLISH	0	// the index of English in the language list, always first and always built

#include "tr_keys.h"

#if CONFIG_LANG_ANY
typedef uint16_t tr_id_t;
#define TR_NONE		((tr_id_t)0)
// The key is pasted here, in the macro the mark names, so that a key that is also a macro's name stays a key
#define TR(k, s)	tr((tr_id_t)TR_##k)
#define TRN(k, s)	((tr_id_t)TR_##k)
#define TRX(k, s)	((tr_id_t)TR_##k)
#define TRF_TEXT(k, s)	tr((tr_id_t)TR_##k)
// The text in the language in use, else the English; NULL for TR_NONE
const char *tr(tr_id_t id);
// The English, whatever the language (Find matches it as well as the translation); NULL for TR_NONE
const char *tr_en(tr_id_t id);
#else
typedef const char *tr_id_t;
#define TR_NONE		((tr_id_t)0)
#define TR(k, s)	(s)
#define TRN(k, s)	(s)
#define TRX(k, s)	(s)
#define TRF_TEXT(k, s)	(s)
static inline const char *tr(tr_id_t id) { return id; }
static inline const char *tr_en(tr_id_t id) { return id; }
#endif

// tr_args_<KEY> is declared in tr_keys.h and defined nowhere: inside sizeof it is only checked, a value of the wrong
// type, in the wrong order or missing fails the build, and no code is made
#define TRF(b, k, s, ...)	((void)sizeof(tr_args_##k(__VA_ARGS__)), sb_fmt((b), TRF_TEXT(k, s), __VA_ARGS__))

// Every language a catalog exists for, English first, built into this image or not (Settings > Display > Language
// lists them all and dims the ones left out)
uint8_t lang_count(void);
const char *lang_code(uint8_t i);	// ISO 639-1: "en", "es"
const char *lang_name(uint8_t i);	// in the language itself: "Español", or "Chinese" in a build without the CJK face
bool lang_built(uint8_t i);
// True when the language's catalog is UTF-8 with a CJK face (X-Glyphs: utf8): its lines need a 16 px font
bool lang_wide(uint8_t i);
// The language in use; lang_set takes only a built one (false otherwise)
uint8_t lang_current(void);
bool lang_set(uint8_t i);
// A language as the settings store keeps it: its two code letters ('e' << 8 | 's'; English 'e' << 8 | 'n'). lang_find
// returns the index of a stored value: for 0 (never picked) the build's default (CONFIG_LANG_DEFAULT_<CODE>, else
// English), for a language not in the list English
int32_t lang_value(uint8_t i);
uint8_t lang_find(int32_t value);

#endif
