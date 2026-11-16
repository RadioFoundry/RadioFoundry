// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's text: the shared formatter (ui/textfmt.h) into its buffers
#include "debuglink/dl_fmt.h"
#include "ui/textfmt.h"

size_t dl_vfmt(char *buf, size_t cap, const char *fmt, va_list ap)
{
	if (cap == 0)
		return 0;
	sbuf_t b;
	sb_init(&b, buf, cap);
	sb_vfmt(&b, fmt, false, ap);
	return (size_t)(b.p - buf);
}

size_t dl_fmt(char *buf, size_t cap, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	size_t n = dl_vfmt(buf, cap, fmt, ap);
	va_end(ap);
	return n;
}

void dl_text_init(dl_text_t *t, char *buf, size_t cap)
{
	t->buf = buf;
	t->cap = cap;
	t->len = 0;
	t->full = false;
}

void dl_text(dl_text_t *t, const char *fmt, ...)
{
	va_list ap;
	size_t room = t->cap - t->len;

	if (t->full || room < 2) {
		t->full = true;
		return;
	}
	// Formatted in place (its NUL lands inside the room); a piece that fills the room may have been cut, so it
	// counts as not fitting and is dropped
	va_start(ap, fmt);
	size_t n = dl_vfmt(t->buf + t->len, room, fmt, ap);
	va_end(ap);
	if (n >= room - 1) {
		t->full = true;
		return;
	}
	t->len += n;
}

void dl_text_kv_str(dl_text_t *t, const char *key, const char *value)
{
	char v[40];
	size_t n = 0;

	for (const char *s = value; *s != '\0' && n < sizeof(v) - 1; s++) {
		unsigned char c = (unsigned char)*s;
		v[n++] = (c < 0x20 || c == 0x7F) ? '?' : (char)c;
	}
	v[n] = '\0';
	dl_text(t, "%s=%s\n", key, v);
}
