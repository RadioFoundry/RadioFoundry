// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The debug link's printf subset (see dl_fmt.h)
#include "debuglink/dl_fmt.h"

typedef struct {
	char *p;
	char *end;	// the last byte, kept for the NUL
} out_t;

static void put_c(out_t *o, char c)
{
	if (o->p < o->end) {
		*o->p++ = c;
	}
}

static void put_num(out_t *o, uint32_t v, bool neg, unsigned base, bool upper, char pad, int width)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char t[12];
	int n = 0;

	do {
		t[n++] = digits[v % base];
		v /= base;
	} while (v != 0 && n < (int)sizeof(t));
	int len = n + (neg ? 1 : 0);
	if (neg && pad == '0') {
		put_c(o, '-');
	}
	for (; width > len; width--) {
		put_c(o, pad);
	}
	if (neg && pad != '0') {
		put_c(o, '-');
	}
	while (n > 0) {
		put_c(o, t[--n]);
	}
}

size_t dl_vfmt(char *buf, size_t cap, const char *fmt, va_list ap)
{
	if (cap == 0) {
		return 0;
	}
	out_t o = { buf, buf + cap - 1 };

	for (const char *f = fmt; *f != '\0'; f++) {
		if (*f != '%') {
			put_c(&o, *f);
			continue;
		}
		f++;
		char pad = ' ';
		int width = 0;
		bool is_long = false;

		if (*f == '0') {
			pad = '0';
			f++;
		}
		while (*f >= '0' && *f <= '9') {
			width = width * 10 + (*f - '0');
			f++;
		}
		if (width > 20) {
			width = 20;
		}
		while (*f == 'l') {
			is_long = true;
			f++;
		}
		switch (*f) {
		case 'd':
		case 'i': {
			long v = is_long ? va_arg(ap, long) : va_arg(ap, int);
			bool neg = v < 0;
			uint32_t u = neg ? (uint32_t)(-(v + 1)) + 1u : (uint32_t)v;
			put_num(&o, u, neg, 10, false, pad, width);
			break;
		}
		case 'u':
		case 'x':
		case 'X': {
			uint32_t v = is_long ? (uint32_t)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
			put_num(&o, v, false, *f == 'u' ? 10u : 16u, *f == 'X', pad, width);
			break;
		}
		case 'c':
			put_c(&o, (char)va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (s == NULL) {
				s = "(null)";
			}
			int n = 0;
			for (const char *q = s; *q != '\0'; q++) {
				n++;
			}
			for (; width > n; width--) {
				put_c(&o, ' ');
			}
			while (*s != '\0') {
				put_c(&o, *s++);
			}
			break;
		}
		case '%':
			put_c(&o, '%');
			break;
		case '\0':
			f--;	// a lone '%' at the end
			break;
		default:	// unknown conversion: shown as is
			put_c(&o, '%');
			put_c(&o, *f);
			break;
		}
	}
	*o.p = '\0';
	return (size_t)(o.p - buf);
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
