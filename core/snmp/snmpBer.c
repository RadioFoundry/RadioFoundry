// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// BER subset for SNMP (see snmpBer.h). Written from ITU-T X.690 clauses 8.1 (identifier and length octets), 8.3
// (integer), 8.7 (octet string), 8.8 (null) and 8.19 (object identifier).
#include <string.h>
#include "snmp/snmpBer.h"

void berReaderInit(berReader_t *r, const uint8_t *p, int len)
{
	r->p = p;
	r->len = len;
	r->pos = 0;
	r->err = false;
}

bool berAtEnd(const berReader_t *r)
{
	return r->err || (r->pos >= r->len);
}

bool berGet(berReader_t *r, uint8_t *tag, const uint8_t **val, int *len)
{
	int left = r->len - r->pos;
	const uint8_t *q = r->p + r->pos;

	if (r->err || (left < 2) || ((q[0] & 0x1Fu) == 0x1Fu)) {
		r->err = true;	// short, or a multi-octet tag (SNMP never uses one)
		return false;
	}
	int n = q[1];
	int hdr = 2;

	if (n & 0x80) {
		int octets = n & 0x7F;

		// Indefinite (0x80) is not allowed in SNMP; more than two length octets never fits a datagram we take
		if ((octets == 0) || (octets > 2) || (left < (2 + octets))) {
			r->err = true;
			return false;
		}
		n = 0;
		for (int i = 0; i < octets; i++) {
			n = (n << 8) | q[2 + i];
		}
		hdr += octets;
	}
	if (n > (left - hdr)) {
		r->err = true;
		return false;
	}
	*tag = q[0];
	*val = q + hdr;
	*len = n;
	r->pos += hdr + n;
	return true;
}

bool berEnter(berReader_t *r, uint8_t tag, berReader_t *inner)
{
	uint8_t t;
	const uint8_t *v;
	int n;

	if (!berGet(r, &t, &v, &n) || (t != tag)) {
		r->err = true;
		return false;
	}
	berReaderInit(inner, v, n);
	return true;
}

bool berDecodeInt(const uint8_t *p, int len, int32_t *v)
{
	if ((len < 1) || (len > 4)) {
		return false;
	}
	uint32_t u = (p[0] & 0x80) ? 0xFFFFFFFFu : 0;

	for (int i = 0; i < len; i++) {
		u = (u << 8) | p[i];
	}
	*v = (int32_t)u;
	return true;
}

bool berDecodeUnsigned(const uint8_t *p, int len, uint32_t *v)
{
	if ((len < 1) || (len > 5) || (p[0] & 0x80) || ((len == 5) && (p[0] != 0))) {
		return false;	// a negative value, or more than 32 bits
	}
	uint32_t u = 0;

	for (int i = 0; i < len; i++) {
		u = (u << 8) | p[i];
	}
	*v = u;
	return true;
}

bool berGetInt(berReader_t *r, int32_t *v)
{
	uint8_t t;
	const uint8_t *p;
	int n;

	if (!berGet(r, &t, &p, &n) || (t != BER_INTEGER) || !berDecodeInt(p, n, v)) {
		r->err = true;
		return false;
	}
	return true;
}

bool berOidValid(const uint8_t *p, int len)
{
	if ((len < 1) || (len > BER_OID_MAX) || (p[len - 1] & 0x80)) {
		return false;
	}
	bool start = true;
	int octets = 0;

	for (int i = 0; i < len; i++) {
		if (start && (p[i] == 0x80)) {
			return false;	// a leading 0x80 octet: not the shortest form (X.690 8.19.2)
		}
		octets++;
		// Five octets carry 35 bits: only 0x8F and below in the first leaves a 32-bit value
		if ((octets > 5) || ((octets == 5) && (p[i - 4] > 0x8F))) {
			return false;
		}
		start = (p[i] & 0x80) == 0;
		if (start) {
			octets = 0;
		}
	}
	return true;
}

int berOidCompare(const uint8_t *a, int alen, const uint8_t *b, int blen)
{
	int n = (alen < blen) ? alen : blen;
	int c = memcmp(a, b, (size_t)n);

	if (c != 0) {
		return c;
	}
	return alen - blen;
}

void berWriterInit(berWriter_t *w, uint8_t *p, int cap)
{
	w->p = p;
	w->cap = cap;
	w->pos = 0;
	w->err = false;
}

static void put(berWriter_t *w, uint8_t b)
{
	if (w->pos >= w->cap) {
		w->err = true;
		return;
	}
	w->p[w->pos++] = b;
}

int berBegin(berWriter_t *w, uint8_t tag)
{
	put(w, tag);
	put(w, 0);	// the length: one octet for now
	return w->pos;
}

void berEnd(berWriter_t *w, int mark)
{
	if (w->err) {
		return;
	}
	int n = w->pos - mark;
	int extra = (n < 0x80) ? 0 : ((n < 0x100) ? 1 : 2);

	if ((w->pos + extra) > w->cap) {
		w->err = true;
		return;
	}
	if (extra) {
		memmove(w->p + mark + extra, w->p + mark, (size_t)n);
		w->p[mark - 1] = (uint8_t)(0x80 | extra);
		if (extra == 2) {
			w->p[mark] = (uint8_t)(n >> 8);
		}
		w->p[mark + extra - 1] = (uint8_t)n;
		w->pos += extra;
	} else {
		w->p[mark - 1] = (uint8_t)n;
	}
}

void berPutInt(berWriter_t *w, uint8_t tag, int32_t v)
{
	uint32_t u = (uint32_t)v;
	int n = 4;

	// Shortest two's complement form: drop a leading octet while it only repeats the next one's sign bit
	while (n > 1) {
		uint32_t top = (u >> (8 * (n - 1))) & 0xFFu;
		uint32_t sign = (u >> (8 * (n - 1) - 1)) & 1u;

		if (!(((top == 0x00u) && !sign) || ((top == 0xFFu) && sign))) {
			break;
		}
		n--;
	}
	put(w, tag);
	put(w, (uint8_t)n);
	for (int i = n - 1; i >= 0; i--) {
		put(w, (uint8_t)(u >> (8 * i)));
	}
}

void berPutUnsigned(berWriter_t *w, uint8_t tag, uint32_t v)
{
	if (v & 0x80000000u) {
		put(w, tag);	// a leading zero octet keeps it positive
		put(w, 5);
		put(w, 0);
		for (int i = 3; i >= 0; i--) {
			put(w, (uint8_t)(v >> (8 * i)));
		}
		return;
	}
	berPutInt(w, tag, (int32_t)v);
}

void berPutOctets(berWriter_t *w, uint8_t tag, const void *p, int len)
{
	int mark = berBegin(w, tag);

	if ((len < 0) || ((w->pos + len) > w->cap)) {
		w->err = true;
		return;
	}
	memcpy(w->p + w->pos, p, (size_t)len);
	w->pos += len;
	berEnd(w, mark);
}

void berPutNull(berWriter_t *w, uint8_t tag)
{
	put(w, tag);
	put(w, 0);
}
