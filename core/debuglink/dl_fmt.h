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

// GET_VIEW's filter and page (docs/spec/debuglink.md 3.7), in place on text of whole lines: the section= line (and a
// truncated= line), then the lines that start with prefix (plen 0: every line), less the first offset bytes of those
// (from the first line that starts at or past it), whole lines up to limit bytes in all, and "more=<the next offset>"
// when lines were left. A page's first line goes whole even when longer than limit, where the text has room for it
// and more=. Returns the new length (never more than len + DL_VIEW_MORE_MAX)
#define DL_VIEW_MORE_MAX	12u	// "more=65535\n" and a byte to spare
#define DL_VIEW_PAGE_MIN	64u	// the smallest page GET_VIEW makes: the section= line and more= fit
size_t dl_view_page(char *text, size_t len, const char *prefix, size_t plen, uint16_t offset, uint16_t limit);

#endif
