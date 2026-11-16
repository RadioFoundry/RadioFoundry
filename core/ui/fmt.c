// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Small text builders for the UI
#include <stdarg.h>
#include "fmt.h"
#include "font.h"
#include "i18n.h"
#include "gfx.h"

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
		// A CJK catalog's text is UTF-8 (tools/i18n.py): when the buffer has no room for a whole character, drop it
		// whole rather than leave half of it for the text layer to draw as a box
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

// One value of a format: which (from 0), its kind (s K d u), and for a number its least width and its padding
typedef struct {
	uint8_t n;
	uint8_t width;
	char pad;
	char kind;
} fmt_value_t;

// The next value at or after f. *at is its '%', or the end of the text when there is none; the return is the text
// after the value, NULL when there is none. A '%' that doesn't start a value is text. *next counts the values written
// without a number: each is the one after the last
static const char *fmt_next(const char *f, const char **at, fmt_value_t *v, uint8_t *next)
{
	for (; *f; f++) {
		if (*f != '%')
			continue;
		const char *k = f + 1;
		bool numbered = k[0] >= '1' && k[0] <= '0' + SB_FMT_VALUES && k[1] == '$';
		v->n = numbered ? (uint8_t)(k[0] - '1') : *next;
		if (numbered)
			k += 2;
		else if (*next >= SB_FMT_VALUES)
			continue;
		v->pad = ' ';
		v->width = 0;
		if (*k == '0') {
			v->pad = '0';
			k++;
		}
		if (*k >= '1' && *k <= '9')
			v->width = (uint8_t)(*k++ - '0');
		if (*k != 's' && *k != 'K' && *k != 'd' && *k != 'u')
			continue;
		if (!numbered)
			(*next)++;
		v->kind = *k;
		*at = f;
		return k + 1;
	}
	*at = f;
	return NULL;
}

const char *sb_fmt_name;

void sb_fmt(sbuf_t *b, const char *fmt, ...)
{
	// A value is used by its number, and a va_list gives them only in order: fetch them all first. A text and a number
	// are fetched as different types, so the first pass finds which is which
	uintptr_t value[SB_FMT_VALUES];
	fmt_value_t v;
	const char *at;
	unsigned count = 0, text = 0;
	uint8_t next = 0;
	sb_fmt_name = NULL;
	for (const char *f = fmt; (f = fmt_next(f, &at, &v, &next)) != NULL;) {
		if (v.n >= count)
			count = v.n + 1u;
		if (v.kind == 's' || v.kind == 'K')
			text |= 1u << v.n;
	}
	va_list ap;
	va_start(ap, fmt);
	for (unsigned i = 0; i < count; i++) {
		if (text & (1u << i))
			value[i] = (uintptr_t)va_arg(ap, const char *);
		else
			value[i] = (uintptr_t)va_arg(ap, unsigned);
	}
	va_end(ap);
	next = 0;
	for (;;) {
		const char *rest = fmt_next(fmt, &at, &v, &next);
		put(b, fmt, at, false);
		if (rest == NULL)
			return;
		fmt = rest;
		uintptr_t a = value[v.n];
		switch (v.kind) {
		case 's':
			sb_fmt_name = b->p;
			// fallthrough
		case 'K':
			if (a != 0)
				put(b, (const char *)a, NULL, v.kind == 's');
			break;
		default: {
			// The sign and the digits, zero padded after the sign; then spaces before them up to the width
			SB_LOCAL(n, 12);
			uint32_t u = (uint32_t)a;
			if (v.kind == 'd' && (int32_t)u < 0) {
				sb_chr(&n, '-');
				u = 0u - u;
			}
			sb_u32(&n, u, v.pad == '0' ? v.width - (int)(n.p - n_text) : 1);
			for (int len = (int)(n.p - n_text); len < v.width; len++)
				sb_chr(b, ' ');
			sb_str(b, n_text);
			break;
		}
		}
	}
}

void sb_u32(sbuf_t *b, uint32_t v, int min_digits)
{
	char tmp[10];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v && n < 10);
	while (n < min_digits && n < 10)
		tmp[n++] = '0';
	while (n)
		sb_chr(b, tmp[--n]);
}

void sb_i32(sbuf_t *b, int32_t v)
{
	if (v < 0) {
		sb_chr(b, '-');
		sb_u32(b, (uint32_t)(-(int64_t)v), 1);
	} else {
		sb_u32(b, (uint32_t)v, 1);
	}
}

void sb_hex(sbuf_t *b, uint32_t v, int digits)
{
	for (int sh = 4 * (digits - 1); sh >= 0; sh -= 4)
		sb_chr(b, "0123456789ABCDEF"[(v >> sh) & 0xFu]);
}

void sb_freq(sbuf_t *b, uint32_t hz)
{
	sb_u32(b, hz / 1000000u, 1);
	sb_chr(b, '.');
	uint32_t frac = hz % 1000000u;	// Hz
	sb_u32(b, frac / 1000u, 3);
	uint32_t rest = (frac % 1000u) / 10u;	// 10 Hz units, two more decimals at most
	if (rest) {
		sb_chr(b, (char)('0' + rest / 10u));
		if (rest % 10u)
			sb_chr(b, (char)('0' + rest % 10u));
	}
}

void sb_tone(sbuf_t *b, const subtone_t *t)
{
	switch (t->type) {
	case TONE_CTCSS:
		sb_u32(b, t->value / 10u, 1);
		sb_chr(b, '.');
		sb_u32(b, t->value % 10u, 1);
		break;
	case TONE_DCS:
	case TONE_DCS_INV:
		sb_chr(b, 'D');
		sb_chr(b, (char)('0' + ((t->value >> 6) & 7u)));
		sb_chr(b, (char)('0' + ((t->value >> 3) & 7u)));
		sb_chr(b, (char)('0' + (t->value & 7u)));
		sb_chr(b, t->type == TONE_DCS_INV ? 'I' : 'N');
		break;
	default:
		break;
	}
}

void sb_split(sbuf_t *b, uint32_t rx_hz, uint32_t tx_hz)
{
	if (tx_hz == rx_hz) {
		sb_str(b, TR(VALUE_OFF, "Off"));
		return;
	}
	sb_chr(b, tx_hz > rx_hz ? '+' : '-');
	uint32_t d = tx_hz > rx_hz ? tx_hz - rx_hz : rx_hz - tx_hz;
	// Below 1 MHz in kHz, else in MHz; three decimals at most, trailing zeros dropped: "+600k", "+12.5k", "-7.6M"
	bool mhz = d >= 1000000u;
	uint32_t unit = mhz ? 1000000u : 1000u;
	sb_u32(b, d / unit, 1);
	uint32_t frac = d % unit / (unit / 1000u);
	if (frac != 0) {
		sb_chr(b, '.');
		for (uint32_t div = 100u; frac != 0; div /= 10u) {
			sb_chr(b, (char)('0' + frac / div));
			frac %= div;
		}
	}
	sb_chr(b, mhz ? 'M' : 'k');
}

void sb_mmss(sbuf_t *b, uint32_t seconds)
{
	sb_u32(b, seconds / 60u, 1);
	sb_chr(b, ':');
	sb_u32(b, seconds % 60u, 2);
}

// The age's number and unit letter: 45 's', 14 'm', 3 'h', 2 'd'
// The unit goes to unit[0]: the caller's is a string of one letter
static uint32_t age_unit(uint32_t ms, char *unit)
{
	uint32_t s = ms / 1000u;
	if (s < 60u) {
		*unit = 's';
		return s;
	}
	if (s < 3600u) {
		*unit = 'm';
		return s / 60u;
	}
	if (s < 86400u) {
		*unit = 'h';
		return s / 3600u;
	}
	*unit = 'd';
	return s / 86400u;
}

void sb_age_long(sbuf_t *b, uint32_t ms)
{
	char unit[2] = "";
	uint32_t v = age_unit(ms, unit);
	TRF(b, VALUE_AGE_LONG, "%1$u%2$K ago", v, unit);
}

// "14m ago", "14m" as text alternates: the drawing keeps the longest that fits (font.h). One unit style everywhere,
// so two ages on one screen always read alike
void sb_age(sbuf_t *b, uint32_t ms)
{
	char unit[2] = "";
	uint32_t v = age_unit(ms, unit);
	TRF(b, VALUE_AGE, TEXT_ALT_OPEN "%1$u%2$K ago" TEXT_ALT_NEXT "%1$u%2$K" TEXT_ALT_CLOSE, v, unit);
}

void sb_degrees(sbuf_t *b, int32_t e7)
{
	uint32_t v = (uint32_t)(e7 < 0 ? -(int64_t)e7 : e7);
	v = (v + 500u) / 1000u;	// 1e-4 degree
	if (e7 < 0)
		sb_chr(b, '-');
	sb_u32(b, v / 10000u, 1);
	sb_chr(b, '.');
	sb_u32(b, v % 10000u, 4);
}

void sb_coord(sbuf_t *b, int32_t e7, char pos, char neg)
{
	uint32_t v = (uint32_t)(e7 < 0 ? -(int64_t)e7 : e7);
	v = (v + 5u) / 10u;	// 1e-6 degree
	sb_u32(b, v / 1000000u, 1);
	sb_chr(b, '.');
	sb_u32(b, v % 1000000u, 6);
	sb_chr(b, ' ');
	sb_chr(b, e7 < 0 ? neg : pos);
}

void sb_compass(sbuf_t *b, uint16_t bearing)
{
	static const char *const points[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	sb_str(b, points[((bearing % 360u) * 2u + 45u) / 90u % 8u]);
}

void sb_range(sbuf_t *b, uint32_t meters, uint16_t bearing)
{
	if (meters < 1000u) {
		sb_u32(b, meters, 1);
		sb_str(b, " m ");
	} else if (meters < 100000u) {
		uint32_t h = (meters + 50u) / 100u;	// 0.1 km
		sb_u32(b, h / 10u, 1);
		sb_chr(b, '.');
		sb_u32(b, h % 10u, 1);
		sb_str(b, " km ");
	} else {
		sb_u32(b, (meters + 500u) / 1000u, 1);
		sb_str(b, " km ");
	}
	sb_compass(b, bearing);
}

// A string's display width in cells, for truncation that never splits a character: a CJK glyph (code point > 0xFF) is
// two cells, matching the room math ((w - x) / f->w) the talk line uses; ASCII, Latin-1 and font glyph bytes are one
size_t ui_text_cells(const char *s)
{
	size_t cells = 0;
	for (const char *p = s; *p; ) {
		int seq;
#if CONFIG_LANG_ZH
		uint32_t cp = gfx_utf8_next(p, &seq);
		cells += (cp > 0xFF) ? 2 : seq;
#else
		seq = 1;
		cells += 1;
#endif
		p += seq;
	}
	return cells;
}

// Append up to max_cells of s to b, stopping at a character boundary (a CJK character is two cells); the caller adds the
// ellipsis. Never writes a cut-off character, so a Chinese name stays whole or drops whole
void sb_cut_text(sbuf_t *b, const char *s, size_t max_cells)
{
	size_t cells = 0;
	for (const char *p = s; *p; ) {
		int seq;
		size_t cw;
#if CONFIG_LANG_ZH
		uint32_t cp = gfx_utf8_next(p, &seq);
		cw = (cp > 0xFF) ? 2 : seq;
#else
		seq = 1;
		cw = 1;
#endif
		if (cells + cw > max_cells)
			break;
		cells += cw;
		for (size_t k = 0; k < (size_t)seq; k++)
			sb_chr(b, p[k]);
		p += seq;
	}
}
