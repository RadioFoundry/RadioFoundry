// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// KISS framing (kiss.h)
#include <string.h>
#include "aprs/kiss.h"

size_t kiss_encode(uint8_t type, const uint8_t *data, size_t n, uint8_t *out, size_t max)
{
	size_t k = 0;
	if (max < 3) {
		return 0;
	}
	out[k++] = KISS_FEND;
	for (size_t i = 0; i <= n; i++) {
		uint8_t b = i == 0 ? type : data[i - 1u];
		if (b == KISS_FEND || b == KISS_FESC) {
			if (k + 2u >= max) {
				return 0;
			}
			out[k++] = KISS_FESC;
			out[k++] = b == KISS_FEND ? KISS_TFEND : KISS_TFESC;
		} else {
			if (k + 1u >= max) {
				return 0;
			}
			out[k++] = b;
		}
	}
	out[k++] = KISS_FEND;
	return k;
}

void kiss_dec_init(kiss_dec_t *k)
{
	memset(k, 0, sizeof(*k));
}

size_t kiss_dec_byte(kiss_dec_t *k, uint8_t b)
{
	if (b == KISS_FEND) {
		size_t n = 0;
		if (k->in_frame && k->len > 0 && !k->overflow) {
			k->frames++;
			n = k->len;
		}
		k->in_frame = true;	// a FEND both ends a frame and may start the next
		k->len = 0;
		k->esc = false;
		k->overflow = false;
		return n;
	}
	if (!k->in_frame) {
		return 0;		// noise before the first FEND
	}
	if (k->esc) {
		k->esc = false;
		if (b == KISS_TFEND) {
			b = KISS_FEND;
		} else if (b == KISS_TFESC) {
			b = KISS_FESC;
		} else {
			k->bad_escapes++;	// the spec says: drop the FESC, keep the byte
		}
	} else if (b == KISS_FESC) {
		k->esc = true;
		return 0;
	}
	if (k->len >= sizeof(k->buf)) {
		if (!k->overflow) {
			k->overflows++;
		}
		k->overflow = true;
		return 0;
	}
	k->buf[k->len++] = b;
	return 0;
}
