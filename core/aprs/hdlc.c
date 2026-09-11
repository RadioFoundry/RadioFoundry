// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HDLC framing (hdlc.h)
#include <string.h>
#include "aprs/hdlc.h"

void hdlc_enc_init(hdlc_enc_t *e, uint8_t *buf, size_t bytes)
{
	e->buf = buf;
	e->max_bits = (uint32_t)bytes * 8u;
	e->nbits = 0;
	e->level = 1;
	e->ones = 0;
	memset(buf, 0, bytes);
}

// One data bit, NRZI coded: a 0 changes the tone, a 1 keeps it
static bool put(hdlc_enc_t *e, bool bit)
{
	if (e->nbits >= e->max_bits) {
		return false;
	}
	if (!bit) {
		e->level ^= 1u;
	}
	uint8_t m = (uint8_t)(1u << (e->nbits & 7u));
	if (e->level) {
		e->buf[e->nbits >> 3] |= m;
	} else {
		e->buf[e->nbits >> 3] &= (uint8_t)~m;	// written both ways: an encoder state copied back can rewrite
	}
	e->nbits++;
	return true;
}

bool hdlc_enc_flags(hdlc_enc_t *e, uint16_t n)
{
	while (n--) {
		for (int i = 0; i < 8; i++) {
			if (!put(e, (HDLC_FLAG >> i) & 1u)) {
				return false;
			}
		}
	}
	e->ones = 0;
	return true;
}

static bool put_byte(hdlc_enc_t *e, uint8_t b)
{
	for (int i = 0; i < 8; i++) {
		bool bit = (b >> i) & 1u;
		if (!put(e, bit)) {
			return false;
		}
		if (bit) {
			if (++e->ones == 5) {
				if (!put(e, false)) {	// a 0 after five 1s, so data never looks like a flag
					return false;
				}
				e->ones = 0;
			}
		} else {
			e->ones = 0;
		}
	}
	return true;
}

bool hdlc_enc_frame(hdlc_enc_t *e, const uint8_t *frame, size_t n)
{
	uint16_t fcs = ax25_fcs(frame, n);
	e->ones = 0;
	for (size_t i = 0; i < n; i++) {
		if (!put_byte(e, frame[i])) {
			return false;
		}
	}
	return put_byte(e, (uint8_t)fcs) && put_byte(e, (uint8_t)(fcs >> 8));
}

uint16_t hdlc_flags_for_ms(uint16_t ms, uint16_t baud, uint16_t min)
{
	uint32_t n = ((uint32_t)ms * baud + 7999u) / 8000u;	// 8 bits a flag
	return n < min ? min : (uint16_t)n;
}

void hdlc_dec_init(hdlc_dec_t *d)
{
	memset(d, 0, sizeof(*d));
}

static void append(hdlc_dec_t *d, bool bit)
{
	if (d->nbits >= HDLC_BUF_MAX * 8u) {
		d->in_frame = false;	// too long: wait for the next flag
		d->nbits = 0;
		return;
	}
	if ((d->nbits & 7u) == 0) {
		d->buf[d->nbits >> 3] = 0;
	}
	if (bit) {
		d->buf[d->nbits >> 3] |= (uint8_t)(1u << (d->nbits & 7u));
	}
	d->nbits++;
}

size_t hdlc_dec_bit(hdlc_dec_t *d, bool mark)
{
	bool bit = (uint8_t)mark == d->last;	// NRZI: no change = 1
	d->last = (uint8_t)mark;
	size_t out = 0;

	if (bit) {
		if (++d->ones >= 7) {		// seven 1s: abort (or an idle channel)
			if (d->in_frame && d->ones == 7) {
				d->aborts++;
			}
			d->in_frame = false;
			d->nbits = 0;
			return 0;
		}
		if (d->in_frame) {
			append(d, true);
		}
		return 0;
	}
	if (d->ones == 6) {			// 0111111 0: a flag
		if (d->in_frame && d->nbits >= 7u) {
			uint32_t n = d->nbits - 7u;	// the flag's first seven bits went in
			if ((n & 7u) == 0 && n / 8u >= HDLC_FRAME_MIN) {
				size_t len = n / 8u;
				uint16_t fcs = (uint16_t)(d->buf[len - 2u] | (d->buf[len - 1u] << 8));
				if (ax25_fcs(d->buf, len - 2u) == fcs) {
					d->frames++;
					out = len - 2u;
				} else {
					d->fcs_errors++;
				}
			}
		}
		d->in_frame = true;
		d->nbits = 0;
	} else if (d->ones == 5) {
		// a stuffed 0: dropped
	} else if (d->in_frame) {
		append(d, false);
	}
	d->ones = 0;
	return out;
}
