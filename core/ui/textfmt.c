// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The text formatter (see textfmt.h), shared by the UI and the debug link
#include "config.h"
#include "textfmt.h"

void sb_init(sbuf_t *b, char *buf, size_t len)
{
	b->p = buf;
	b->end = buf + (len ? len - 1 : 0);
	if (len)
		*buf = 0;
}

void str_copy(char *dst, size_t cap, const char *src)
{
	sbuf_t b;
	sb_init(&b, dst, cap);
	sb_str(&b, src);
}

void sb_chr(sbuf_t *b, char c)
{
	if (b->p < b->end) {
		*b->p++ = c;
		*b->p = 0;
	}
}

// Copies s up to end (its NUL when end is NULL). With safe set a byte below 0x20 becomes '?': the text is a name or
// a received text, and those bytes are the markers the text layer reads (font.h)
static void put(sbuf_t *b, const char *s, const char *end, bool safe)
{
	while (s != end && *s) {
#if CONFIG_LANG_ZH
		// A CJK catalog's text is UTF-8 (tools/i18n.py): when the buffer has no room for a whole character, drop
		// it whole rather than leave half of it for the text layer to draw as a box
		const uint8_t *p = (const uint8_t *)s;
		int seq = ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) ? 3 : 1;
		if ((size_t)(b->end - b->p) < (size_t)seq)
			return;
		while (--seq)
			sb_chr(b, *s++);
#endif
		char c = *s++;
		sb_chr(b, safe && (uint8_t)c < 0x20 ? '?' : c);
	}
}

void sb_str(sbuf_t *b, const char *s)
{
	put(b, s, NULL, false);
}


// v in base 10 or 16 with its sign: '0' pads with zeros after the sign, ' ' with spaces before it, up to width
static void put_num(sbuf_t *b, uint32_t v, bool neg, unsigned base, bool upper, char pad, int width)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char t[10];
	int n = 0;
	do {
		t[n++] = digits[v % base];
		v /= base;
	} while (v != 0);
	int len = n + (int)neg;
	if (neg && pad == '0')
		sb_chr(b, '-');
	for (; width > len; width--)
		sb_chr(b, pad);
	if (neg && pad != '0')
		sb_chr(b, '-');
	while (n > 0)
		sb_chr(b, t[--n]);
}

void sb_u32(sbuf_t *b, uint32_t v, int min_digits)
{
	put_num(b, v, false, 10, false, '0', min_digits);
}

void sb_i32(sbuf_t *b, int32_t v)
{
	put_num(b, v < 0 ? 0u - (uint32_t)v : (uint32_t)v, v < 0, 10, false, '0', 1);
}

const char *sb_fmt_name;

// The value after the '%' at f: n is its number from 0 (unnumbered: *next, which then moves on), and the return is
// its kind ('%' for "%%", 0 for text that isn't a value) with *end just after it
typedef struct {
	unsigned n;
	int width;
	char pad;
	bool is_long, numbered;
} spec_t;

static char spec(const char *f, spec_t *v, unsigned *next, const char **end)
{
	const char *k = f + 1;
	v->numbered = k[0] >= '1' && k[0] <= '0' + SB_FMT_VALUES && k[1] == '$';
	v->n = v->numbered ? (unsigned)(k[0] - '1') : *next;
	if (v->numbered)
		k += 2;
	v->pad = ' ';
	v->width = 0;
	v->is_long = false;
	if (*k == '0') {
		v->pad = '0';
		k++;
	}
	while (*k >= '0' && *k <= '9')
		v->width = v->width * 10 + (*k++ - '0');
	if (v->width > 20)
		v->width = 20;
	while (*k == 'l') {
		v->is_long = true;
		k++;
	}
	char c = *k;
	*end = c ? k + 1 : k;
	if (c == '%' && k == f + 1)
		return '%';
	if (c != 's' && c != 'K' && c != 'd' && c != 'i' && c != 'u' && c != 'x' && c != 'X' && c != 'c')
		return 0;
	if (!v->numbered)
		(*next)++;
	return c;
}

static uintptr_t arg(va_list *ap, char c, bool is_long)
{
	if (c == 's' || c == 'K')
		return (uintptr_t)va_arg(*ap, const char *);
	if (c == 'd' || c == 'i')
		return (uintptr_t)(uint32_t)(is_long ? (int32_t)va_arg(*ap, long) : va_arg(*ap, int));
	return is_long ? (uint32_t)va_arg(*ap, unsigned long) : va_arg(*ap, unsigned);
}

void sb_vfmt(sbuf_t *b, const char *fmt, bool ugc, va_list ap)
{
	// A numbered format uses its values in any order, and a va_list gives them only in order: read them all first,
	// each by the kind the text gives it
	va_list args;
	va_copy(args, ap);
	uintptr_t value[SB_FMT_VALUES];
	bool numbered = false;
	spec_t v;
	unsigned next = 0;
	char kind[SB_FMT_VALUES] = { 0 };
	for (const char *p = fmt; *p;) {
		if (*p != '%') {
			p++;
			continue;
		}
		char c = spec(p, &v, &next, &p);
		if (c != 0 && c != '%') {
			numbered |= v.numbered;
			if (v.n < SB_FMT_VALUES)
				kind[v.n] = c;
		}
	}
	if (numbered)
		for (unsigned i = 0; i < SB_FMT_VALUES && kind[i]; i++)
			value[i] = arg(&args, kind[i], false);
	if (ugc)
		sb_fmt_name = NULL;
	next = 0;
	const char *f = fmt;
	for (;;) {
		const char *pc = f;
		while (*pc && *pc != '%')
			pc++;
		put(b, f, pc, false);
		if (!*pc)
			break;
		char c = spec(pc, &v, &next, &f);
		if (c == '%') {
			sb_chr(b, '%');
			continue;
		}
		if (c == 0 || (numbered && (v.n >= SB_FMT_VALUES || !kind[v.n]))) {
			put(b, pc, f, false);	// not a value: text, as written
			continue;
		}
		uintptr_t a = numbered ? value[v.n] : arg(&args, c, v.is_long);
		if (c == 's' || c == 'K') {
			const char *s = (const char *)a;
			if (s == NULL)
				s = ugc ? "" : "(null)";
			bool name = ugc && c == 's';
			if (name)
				sb_fmt_name = b->p;
			int len = 0;
			for (const char *q = s; *q && len < v.width; q++)
				len++;
			for (int w = v.width; w > len; w--)
				sb_chr(b, ' ');
			put(b, s, NULL, name);
		} else if (c == 'c') {
			sb_chr(b, (char)a);
		} else if (c == 'd' || c == 'i') {
			int32_t d = (int32_t)a;
			put_num(b, d < 0 ? 0u - (uint32_t)d : (uint32_t)d, d < 0, 10, false, v.pad, v.width);
		} else {
			put_num(b, (uint32_t)a, false, c == 'u' ? 10u : 16u, c == 'X', v.pad, v.width);
		}
	}
	va_end(args);
}

void sb_fmt(sbuf_t *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sb_vfmt(b, fmt, true, ap);
	va_end(ap);
}
