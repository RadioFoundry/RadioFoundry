// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The text formatter (see format.h), shared by the UI and the debug link
#include "config.h"
#include "text/format.h"

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

#define PUT_SAFE	1u	// a byte below 0x20 becomes '?': the text is a name or a received text, and those bytes
				// are the markers the text layer reads (font.h)
#define PUT_UI		2u	// text the UI draws: a CJK character is cut whole (the debug link's bytes are left as
				// they are, whatever the language switches)

// Copies s up to end (its NUL when end is NULL)
static void put(sbuf_t *b, const char *s, const char *end, unsigned how)
{
	while (s != end && *s) {
#if CONFIG_LANG_ZH
		// A CJK catalog's text is UTF-8 (tools/i18n.py): when the buffer has no room for a whole character,
		// drop it whole rather than leave half of it for the text layer to draw as a box
		const uint8_t *p = (const uint8_t *)s;
		int seq = (how & PUT_UI) && (p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 ?
			3 : 1;
		if ((size_t)(b->end - b->p) < (size_t)seq)
			return;
		while (--seq)
			sb_chr(b, *s++);
#endif
		char c = *s++;
		sb_chr(b, (how & PUT_SAFE) && (uint8_t)c < 0x20 ? '?' : c);
	}
}

void sb_str(sbuf_t *b, const char *s)
{
	put(b, s, NULL, PUT_UI);
}


// How put_num writes: 4 arguments at most, so that none goes on the caller's stack (the debug link's deepest path
// runs through fmt_write)
#define NUM_NEG		1u	// v is the magnitude of a negative number
#define NUM_HEX		2u
#define NUM_UPPER	4u
#define NUM_ZEROS	8u	// zero padded after the sign, else (SB_FMT_SPACE_PAD) spaces before it

// v in base 10 or 16 with its sign, padded up to width. The digits come from the highest power of the base down,
// with no buffer
static void put_num(sbuf_t *b, uint32_t v, unsigned how, int width)
{
	unsigned base = how & NUM_HEX ? 16u : 10u;
	bool neg = (how & NUM_NEG) != 0;
	uint32_t div = 1;
	int len = 1 + (int)neg;
	while (v / div >= base) {
		div *= base;
		len++;
	}
	if (neg && (how & NUM_ZEROS))
		sb_chr(b, '-');
	for (; width > len; width--)
		sb_chr(b, how & NUM_ZEROS ? '0' : ' ');
	if (neg && !(how & NUM_ZEROS))
		sb_chr(b, '-');
	do {
		unsigned d = (unsigned)(v / div % base);
		sb_chr(b, (char)(d < 10u ? '0' + d : (how & NUM_UPPER ? 'A' : 'a') + d - 10u));
		div /= base;
	} while (div != 0);
}

void sb_u32(sbuf_t *b, uint32_t v, int min_digits)
{
	put_num(b, v, NUM_ZEROS, min_digits);
}

void sb_i32(sbuf_t *b, int32_t v)
{
	put_num(b, v < 0 ? 0u - (uint32_t)v : (uint32_t)v, (v < 0 ? NUM_NEG : 0u) | NUM_ZEROS, 1);
}

// One value's spec: %[n$][0][width][l]<kind>
typedef struct {
	uint8_t n;		// its number from 0
	uint8_t width;
	char pad;
	bool is_long, numbered;
} spec_t;

// The value after the '%' at f. Its number is n$, else *next, which then moves on. The return is its kind ('%' for
// "%%", 0 for text that isn't a value), with *end just after it
static char spec(const char *f, spec_t *v, unsigned *next, const char **end)
{
	const char *k = f + 1;
	v->numbered = k[0] >= '1' && k[0] <= '0' + SB_FMT_VALUES && k[1] == '$';
	v->n = (uint8_t)(v->numbered ? (unsigned)(k[0] - '1') : *next);
	if (v->numbered)
		k += 2;
	v->pad = SB_FMT_SPACE_PAD ? ' ' : 0;
	v->is_long = false;
	if (*k == '0') {
		v->pad = '0';
		k++;
	}
	unsigned w = 0;
	while (*k >= '0' && *k <= '9')
		w = w * 10u + (unsigned)(*k++ - '0');
	v->width = (uint8_t)(v->pad && w <= 20u ? w : v->pad ? 20u : 0u);
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

static uintptr_t arg(va_list *ap, bool text, bool is_signed, bool is_long)
{
	if (text)
		return (uintptr_t)va_arg(*ap, const char *);
	if (is_signed)
		return (uintptr_t)(uint32_t)(is_long ? (int32_t)va_arg(*ap, long) : va_arg(*ap, int));
	return is_long ? (uint32_t)va_arg(*ap, unsigned long) : va_arg(*ap, unsigned);
}

// Value n (from 0) of a numbered format: the values before it are stepped over by their kind (text: bit i of text),
// from a copy of the list. A va_list gives values only in order, and keeping them all would cost the stack
static uintptr_t nth(va_list *start, unsigned n, unsigned text)
{
	va_list ap;
	va_copy(ap, *start);
	uintptr_t a;
	for (unsigned i = 0;; i++) {
		a = arg(&ap, (text >> i) & 1u, false, false);
		if (i == n)
			break;
	}
	va_end(ap);
	return a;
}

// A format's values, read before it is written: bit n set for each value n that exists (FMT_SEEN), the same for
// each that is text (FMT_TEXT), and FMT_NUMBERED when one is numbered. A function of its own, so that its locals are
// off the stack while sb_vfmt writes (the debug link's deepest path runs through it)
#define FMT_SEEN(k)	((k) & 0xFu)
#define FMT_TEXT(k)	(((k) >> 4) & 0xFu)
#define FMT_NUMBERED	0x100u
#define FMT_UI		0x200u	// UI text, not the debug link's (format.h)
__attribute__((noinline)) static unsigned fmt_kinds(const char *fmt)
{
	spec_t v;
	unsigned next = 0, k = 0;
	for (const char *p = fmt; *p;) {
		if (*p != '%') {
			p++;
			continue;
		}
		char c = spec(p, &v, &next, &p);
		if (c != 0 && c != '%' && v.n < SB_FMT_VALUES) {
			k |= (v.numbered ? FMT_NUMBERED : 0u) | 1u << v.n;
			if (c == 's' || c == 'K')
				k |= 16u << v.n;
		}
	}
	return k;
}

// Writes a format. k: FMT_UI and fmt_kinds()'s for UI text, 0 for the debug link's (printf's: values in order). Four
// arguments, none on the caller's stack; one copy for both callers
__attribute__((noinline)) static const char *fmt_write(sbuf_t *b, const char *fmt, unsigned k, va_list ap)
{
	va_list args;
	va_copy(args, ap);
	bool ui = (k & FMT_UI) != 0;
	// The spec and where the name went, in memory together: one register fewer (the debug link's deepest path runs
	// through here)
	struct {
		spec_t v;
		const char *name_at;
	} st;
	st.name_at = NULL;
	unsigned next = 0;
	const unsigned how = (unsigned)ui * PUT_UI;
	const char *f = fmt;
	for (;;) {
		const char *pc = f;
		while (*pc && *pc != '%')
			pc++;
		put(b, f, pc, how);
		if (!*pc)
			break;
		char c = spec(pc, &st.v, &next, &f);
		if (c == '%') {
			sb_chr(b, '%');
			continue;
		}
		// A numbered format's values must be 1 to n with none left out (tools/i18n.py), else nothing is read
		// past one that is missing
		if (c == 0 || ((k & FMT_NUMBERED) && (st.v.n >= SB_FMT_VALUES || ((FMT_SEEN(k) + 1u) & FMT_SEEN(k))))) {
			put(b, pc, f, how);	// not a value: text, as written
			continue;
		}
		bool is_text = c == 's' || c == 'K';
		uintptr_t a = (k & FMT_NUMBERED) ? nth(&args, st.v.n, FMT_TEXT(k)) :
			arg(&args, is_text, c == 'd' || c == 'i', st.v.is_long);
		if (is_text) {
			const char *s = (const char *)a;
			if (s == NULL)
				s = ui ? "" : "(null)";
			bool name = ui && c == 's';
			if (name)
				st.name_at = b->p;
#if SB_FMT_SPACE_PAD
			int len = 0;
			for (const char *q = s; *q && len < st.v.width; q++)
				len++;
			for (int w = st.v.width; w > len; w--)
				sb_chr(b, ' ');
#endif
			put(b, s, NULL, how | (name ? PUT_SAFE : 0u));
		} else if (c == 'c') {
			sb_chr(b, (char)a);
		} else if (c == 'd' || c == 'i') {
			int32_t d = (int32_t)a;
			put_num(b, d < 0 ? 0u - (uint32_t)d : (uint32_t)d, (d < 0 ? NUM_NEG : 0u) |
				(st.v.pad == '0' ? NUM_ZEROS : 0u), st.v.width);
		} else {
			put_num(b, (uint32_t)a, (c == 'u' ? 0u : NUM_HEX) | (c == 'X' ? NUM_UPPER : 0u) |
				(st.v.pad == '0' ? NUM_ZEROS : 0u), st.v.width);
		}
	}
	va_end(args);
	return st.name_at;
}

// The UI's: a numbered format uses its values in any order, so it is read once first (fmt_kinds). The debug link's
// path (sb_vprintf) never calls fmt_kinds, and the stack check sees that
const char *sb_fmt(sbuf_t *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	const char *name_at = fmt_write(b, fmt, FMT_UI | fmt_kinds(fmt), ap);
	va_end(ap);
	return name_at;
}

void sb_vprintf(sbuf_t *b, const char *fmt, va_list ap)
{
	(void)fmt_write(b, fmt, 0u, ap);
}
