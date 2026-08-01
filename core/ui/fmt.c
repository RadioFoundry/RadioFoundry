// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Small text builders for the UI
#include "fmt.h"

void sb_init(sbuf_t *b, char *buf, size_t len)
{
	b->p = buf;
	b->end = buf + (len ? len - 1 : 0);
	if (len)
		*buf = 0;
}

void sb_chr(sbuf_t *b, char c)
{
	if (b->p < b->end) {
		*b->p++ = c;
		*b->p = 0;
	}
}

void sb_str(sbuf_t *b, const char *s)
{
	while (*s)
		sb_chr(b, *s++);
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

void sb_mmss(sbuf_t *b, uint32_t seconds)
{
	sb_u32(b, seconds / 60u, 1);
	sb_chr(b, ':');
	sb_u32(b, seconds % 60u, 2);
}

void sb_age(sbuf_t *b, uint32_t ms)
{
	uint32_t s = ms / 1000u;
	if (s < 60u) {
		sb_u32(b, s, 1);
		sb_chr(b, 's');
	} else if (s < 3600u) {
		sb_u32(b, s / 60u, 1);
		sb_chr(b, 'm');
	} else if (s < 86400u) {
		sb_u32(b, s / 3600u, 1);
		sb_chr(b, 'h');
	} else {
		sb_u32(b, s / 86400u, 1);
		sb_chr(b, 'd');
	}
}
