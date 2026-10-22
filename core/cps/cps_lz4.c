// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// LZ4 block encoder and streaming decoder (cps_lz4.h, docs/spec/cps-fast.md 5)
#include <string.h>
#include "cps/cps_lz4.h"

#define HASH_BITS	6	// 64 entries: on codeplug blocks as good as 4096 (cps-fast.md 5.2)
#define MIN_MATCH	4
#define LAST_LITERALS	5	// the block's last 5 bytes are literals
#define MF_LIMIT	12	// no match starts in the block's last 12 bytes
#define EMPTY		0xFFFFu

enum { S_TOKEN, S_LITLEN, S_LIT, S_OFF0, S_OFF1, S_MLEN };

// --- encoder ---

typedef struct {
	uint8_t *out;
	uint32_t skip, cap, pos;	// the window [skip, skip + cap) of the stream goes to out; pos counts every byte
} sink_t;

static void put(sink_t *s, uint8_t b)
{
	if (s->pos >= s->skip && s->pos - s->skip < s->cap) {
		s->out[s->pos - s->skip] = b;
	}
	s->pos++;
}

static void put_len(sink_t *s, uint32_t v)
{
	while (v >= 255u) {
		put(s, 255u);
		v -= 255u;
	}
	put(s, (uint8_t)v);
}

// One sequence: literals, then a match of ml bytes at off back (ml 0: the last sequence, literals only)
static void sequence(sink_t *s, const uint8_t *lit, uint32_t nlit, uint32_t off, uint32_t ml)
{
	uint8_t tok = (uint8_t)((nlit < 15u ? nlit : 15u) << 4);
	if (ml != 0) {
		tok |= (uint8_t)(ml - MIN_MATCH < 15u ? ml - MIN_MATCH : 15u);
	}
	put(s, tok);
	if (nlit >= 15u) {
		put_len(s, nlit - 15u);
	}
	for (uint32_t i = 0; i < nlit; i++) {
		put(s, lit[i]);
	}
	if (ml != 0) {
		put(s, (uint8_t)off);
		put(s, (uint8_t)(off >> 8));
		if (ml - MIN_MATCH >= 15u) {
			put_len(s, ml - MIN_MATCH - 15u);
		}
	}
}

static uint32_t read32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t cps_lz4_encode(const uint8_t *src, uint32_t n, uint8_t *out, uint32_t skip, uint32_t cap)
{
	uint16_t table[1u << HASH_BITS];
	sink_t s = { out, skip, cap, 0 };
	uint32_t ip = 0, anchor = 0;

	memset(table, 0xFF, sizeof(table));
	while (n > MF_LIMIT && ip + MF_LIMIT <= n) {
		uint32_t v = read32(src + ip);
		uint32_t h = (v * 2654435761u) >> (32 - HASH_BITS);
		uint32_t ref = table[h];

		table[h] = (uint16_t)ip;
		if (ref != EMPTY && read32(src + ref) == v) {
			uint32_t ml = MIN_MATCH;
			while (ip + ml < n - LAST_LITERALS && src[ref + ml] == src[ip + ml]) {
				ml++;
			}
			sequence(&s, src + anchor, ip - anchor, ip - ref, ml);
			ip += ml;
			anchor = ip;
			continue;
		}
		ip++;
	}
	sequence(&s, src + anchor, n - anchor, 0, 0);
	return s.pos;
}

// --- decoder ---

void cps_lz4_dec_init(cps_lz4_dec_t *d, uint8_t *out, uint32_t size)
{
	memset(d, 0, sizeof(*d));
	d->out = out;
	d->size = size;
	d->state = S_TOKEN;
}

static void copy_match(cps_lz4_dec_t *d)
{
	uint32_t ml = d->ml + MIN_MATCH;
	if (d->off == 0 || d->off > d->o || ml > d->size - d->o) {
		d->bad = true;
		return;
	}
	for (uint32_t k = 0; k < ml; k++, d->o++) {
		d->out[d->o] = d->out[d->o - d->off];	// byte by byte: an overlapping match repeats what it just wrote
	}
	d->state = S_TOKEN;
}

bool cps_lz4_dec_feed(cps_lz4_dec_t *d, const uint8_t *in, uint32_t n)
{
	uint32_t i = 0;

	while (i < n && !d->bad) {
		uint8_t b = in[i];
		switch (d->state) {
		case S_TOKEN:
			i++;
			d->lit = b >> 4;
			d->ml = b & 15u;
			d->state = d->lit == 15u ? S_LITLEN : (d->lit != 0 ? S_LIT : S_OFF0);
			break;
		case S_LITLEN:
			i++;
			d->lit += b;
			if (d->lit > d->size) {
				d->bad = true;
			} else if (b != 255u) {
				d->state = S_LIT;
			}
			break;
		case S_LIT: {
			uint32_t k = n - i < d->lit ? n - i : d->lit;
			if (k > d->size - d->o) {
				d->bad = true;
				break;
			}
			memcpy(d->out + d->o, in + i, k);
			d->o += k;
			i += k;
			d->lit -= k;
			if (d->lit == 0) {
				d->state = S_OFF0;
			}
			break;
		}
		case S_OFF0:
			i++;
			d->off = b;
			d->state = S_OFF1;
			break;
		case S_OFF1:
			i++;
			d->off |= (uint16_t)(b << 8);
			if (d->ml == 15u) {
				d->state = S_MLEN;
			} else {
				copy_match(d);
			}
			break;
		default:	// S_MLEN
			i++;
			d->ml += b;
			if (d->ml > d->size) {
				d->bad = true;
			} else if (b != 255u) {
				copy_match(d);
			}
			break;
		}
	}
	return !d->bad;
}

bool cps_lz4_dec_done(const cps_lz4_dec_t *d)
{
	// A block ends with a literals-only sequence: the decoder then waits for an offset that never comes
	return !d->bad && d->state == S_OFF0 && d->o == d->size;
}
