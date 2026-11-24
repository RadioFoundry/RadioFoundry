// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A tiny printf subset for the debug link's text replies and dlog(): no libc printf (flash, stack, reentrancy).
//
//   %d %i %u %x %X %s %c %%   with an optional '0' flag, a width (1..20) and an ignored 'l' modifier (32-bit values;
//                             int and long are the same size on every target and on the host tests' ILP32/LP64 the
//                             'l' forms read a long). No floats, no precision.
//
// No RTOS and no board globals: the host tests build it.
#ifndef DL_FMT_H
#define DL_FMT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Format into buf (cap bytes, always NUL terminated when cap > 0). Returns the characters written (no NUL),
// truncated to cap - 1
size_t dl_vfmt(char *buf, size_t cap, const char *fmt, va_list ap);
size_t dl_fmt(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

// An appending text buffer for key=value records. full is set once something did not fit (the record is cut at the
// last whole line that fitted)
typedef struct {
	char *buf;
	size_t cap;	// bytes available (no NUL is kept: the text goes out with its length)
	size_t len;
	bool full;
} dl_text_t;

void dl_text_init(dl_text_t *t, char *buf, size_t cap);
// Append one formatted piece; a piece that does not fit whole is dropped and full set
void dl_text(dl_text_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// key=value\n with a string value; control characters and '\n' in the value become '?'
void dl_text_kv_str(dl_text_t *t, const char *key, const char *value);
// A 1e-7 degree value as text, -12.3456789, into buf; returns buf. Text, not a dl_text piece, so a caller's line
// stays one piece: a reply that fills drops it whole rather than cutting it after the key
#define DL_DEG_LEN 13	// "-214.7483648" and its NUL
const char *dl_deg(char buf[DL_DEG_LEN], int32_t e7);
// key=<degrees>\n, as dl_deg writes them
void dl_text_deg(dl_text_t *t, const char *key, int32_t e7);
// s as a JSON string. Codeplug names are Latin-1 (U+0000-U+00FF): a byte from 0x7F up is written as \u00XX, so the
// text stays ASCII and valid JSON (RFC 8259 8.1)
void dl_json_str(dl_text_t *t, const char *s);

#endif
