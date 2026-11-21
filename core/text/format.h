// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The text formatter: an appending buffer and one printf-like formatter for the UI's text with dynamic data (i18n.h's
// TRF) and the debug link's text (dl_fmt.h). No libc printf (flash, stack, reentrancy), no RTOS, no language tables:
// the host tests and the debug link build it on its own
#ifndef TEXT_FORMAT_H
#define TEXT_FORMAT_H

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

// Formats into the buffer, like printf. A placeholder is %[n$][flags][width]<kind>, replaced by its argument:
//   %d %i %u %x %X %c   numbers and a character; %05u pads with zeros after the sign up to the width (at most 20);
//                       'l' reads a long. A width without the '0' (space padding, %5u, %8s) is ignored unless
//                       SB_FMT_SPACE_PAD is 1: nothing uses it, and the code stays for whoever needs it
//   %s                  a string. In UI text it is untrusted (a name, the CPS's data, a received message): its
//                       control characters (below 0x20, the markers font.h reads) are shown as '?'
//   %K                  (UI) a trusted string the firmware built, kept as it is (K for Keep), so its text features
//                       (text alternates) work
//   %%                  a '%'. Anything else after a '%' is text, as written (tools/i18n.py rejects it in UI text)
// Placeholders take the arguments in the order they are passed. UI text (a translation) may number them, %1$s to
// %4$s, to move or repeat them: every argument is then read first, by the kind its placeholder gives, so the text must
// use each argument at least once. In UI text a NULL string shows nothing, and a CJK character is cut whole at the
// buffer's end. The debug link's text (sb_vprintf) keeps every byte as it is and shows NULL as "(null)". sb_fmt
// returns where the last %s argument went, NULL when there was none (a banner's title breaks its line there)
#define SB_FMT_VALUES	4
#ifndef SB_FMT_SPACE_PAD
#define SB_FMT_SPACE_PAD	0
#endif
const char *sb_fmt(sbuf_t *b, const char *fmt, ...);
// The debug link's (dl_fmt.h): printf's values in order, every byte kept, NULL as "(null)"
void sb_vprintf(sbuf_t *b, const char *fmt, va_list ap);

#endif
