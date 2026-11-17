// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The text formatter: an appending buffer and one printf-like formatter for the UI's text with dynamic data (i18n.h's
// TRF) and the debug link's text (dl_fmt.h). No libc printf (flash, stack, reentrancy), no RTOS, no language tables:
// the host tests and the debug link build it on its own
#ifndef UI_TEXTFMT_H
#define UI_TEXTFMT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Appends to a fixed buffer, always terminated, silently truncated
typedef struct {
	char *p;
	char *end;	// last usable byte (for the terminator)
} sbuf_t;

void sb_init(sbuf_t *b, char *buf, size_t len);
// Copies src into a buffer of cap bytes, truncated and always terminated: for text that may be a translation
// (i18n.h), whose length isn't known when the code is written
void str_copy(char *dst, size_t cap, const char *src);
void sb_str(sbuf_t *b, const char *s);
void sb_chr(sbuf_t *b, char c);
// Unsigned decimal with at least min_digits digits (zero padded)
void sb_u32(sbuf_t *b, uint32_t v, int min_digits);
void sb_i32(sbuf_t *b, int32_t v);

// Formats into the buffer, like printf. A value is %[n$][flags][width]<kind>:
//   %d %i %u %x %X %c   numbers and a character; %05u pads with zeros after the sign up to the width (at most 20);
//                       'l' reads a long. A width without the '0' (space padding, %5u, %8s) is ignored unless
//                       SB_FMT_SPACE_PAD is 1: nothing uses it, and the code stays for whoever needs it
//   %s                  text
//   %K                  (UI) text kept as it is: our own, which may carry text alternates (font.h)
//   %%                  a '%'. Anything else after a '%' is text, as written
// The values are used in the order they are passed. A UI format (a translation) may number them, %1$s to %4$s, so
// that it can move or repeat them: every value is then read first, by the kinds the text gives it, so the text must
// use each value at least once. A UI format's %s is text that isn't ours (a name, a received text): a byte below
// 0x20 is shown as '?', since the text layer reads those as markers (font.h); NULL is nothing, and sb_fmt_name is
// where the last one went. The debug link's (ugc false) keeps every byte and shows NULL as "(null)"
#define SB_FMT_VALUES	4
#ifndef SB_FMT_SPACE_PAD
#define SB_FMT_SPACE_PAD	0
#endif
void sb_vfmt(sbuf_t *b, const char *fmt, bool ugc, va_list ap);
void sb_fmt(sbuf_t *b, const char *fmt, ...);
// Where the last UI format put its %s value, NULL when it had none (a banner's title breaks its line there)
extern const char *sb_fmt_name;

#endif
