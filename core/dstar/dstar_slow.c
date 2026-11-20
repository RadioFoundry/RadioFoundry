// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR slow data (dstar.h; JARL standard chapter 6): the data fields of a superframe's frames 1..20 joined in
// pairs into 6-byte blocks, each opened by a mini-header that says what it carries and how much of it is valid.
#include "dstar.h"
#include <string.h>

void dstar_slow_rx_reset(dstar_slow_rx_t *s)
{
	memset(s, 0, sizeof *s);
}

static uint8_t block(dstar_slow_rx_t *s)
{
	uint8_t mh = s->block[0], kind = mh & 0xF0, n = mh & 0x0F, done = 0;

	switch (kind) {
	case DSTAR_MINI_MSG:
		if (n < 4) {
			memcpy(s->msg + 5 * n, s->block + 1, 5);
			s->msg_parts |= (uint8_t)(1u << n);
			if (s->msg_parts == 0x0F) {
				s->msg_parts = 0;
				done |= DSTAR_SLOW_MSG;
			}
		}
		break;
	case DSTAR_MINI_HEADER:
		// The header in five-byte pieces, one piece a block, the first after a superframe's start or after a block
		// of anything else
		if (n >= 1 && n <= 5) {
			if (s->hdr_len + n > DSTAR_HDR_BYTES)
				s->hdr_len = 0;
			memcpy(s->hdr + s->hdr_len, s->block + 1, n);
			s->hdr_len = (uint8_t)(s->hdr_len + n);
			if (s->hdr_len == DSTAR_HDR_BYTES) {
				dstar_header_t h;

				s->hdr_len = 0;
				if (dstar_header_unpack(s->hdr, &h))
					done |= DSTAR_SLOW_HEADER;
			}
		}
		break;
	case DSTAR_MINI_DATA:
		for (uint8_t i = 1; i <= n && i <= 5; i++) {
			char ch = (char)s->block[i];

			if (ch == '\r' || ch == '\n') {
				if (s->line_len) {
					memcpy(s->text, s->line, s->line_len);
					s->text[s->line_len] = '\0';
					s->line_len = 0;
					done |= DSTAR_SLOW_TEXT;
				}
			} else if (ch >= ' ' && ch <= '~' && s->line_len < DSTAR_TEXT_LEN - 1) {
				s->line[s->line_len++] = ch;
			}
		}
		break;
	default:
		break;
	}
	if (kind != DSTAR_MINI_HEADER)
		s->hdr_len = 0;
	return done;
}

uint8_t dstar_slow_rx_frame(dstar_slow_rx_t *s, const uint8_t data[DSTAR_DATA_BYTES], uint8_t n)
{
	if (n == 1)
		s->hdr_len = 0;		// a header sent again starts at a superframe's start
	if (n & 1) {
		memcpy(s->block, data, DSTAR_DATA_BYTES);
		s->have = DSTAR_DATA_BYTES;
		return 0;
	}
	if (s->have != DSTAR_DATA_BYTES)
		return 0;		// its first half was lost
	memcpy(s->block + 3, data, DSTAR_DATA_BYTES);
	s->have = 0;
	return block(s);
}

void dstar_slow_tx_init(dstar_slow_tx_t *s, const uint8_t hdr[DSTAR_HDR_BYTES], const char *msg)
{
	memset(s, 0, sizeof *s);
	memcpy(s->hdr, hdr, DSTAR_HDR_BYTES);
	if (msg != NULL && msg[0] != '\0') {
		uint8_t i = 0;

		for (; msg[i] != '\0' && i < DSTAR_MSG_LEN; i++)
			s->msg[i] = (msg[i] >= ' ' && msg[i] <= '~') ? msg[i] : ' ';
		for (; i < DSTAR_MSG_LEN; i++)
			s->msg[i] = ' ';
		s->have_msg = true;
	}
}

// Block b (0..9) of the superframe in hand: the message's four blocks in the first ten frames of an even superframe,
// the header's nine (five bytes each, one in the last) in the others; "no data" after them
static void tx_block(dstar_slow_tx_t *s, uint8_t b)
{
	memset(s->block, DSTAR_MINI_NONE, sizeof s->block);
	if (s->have_msg && (s->sf & 1) == 0) {
		if (b < 4) {
			s->block[0] = (uint8_t)(DSTAR_MINI_MSG | b);
			memcpy(s->block + 1, s->msg + 5 * b, 5);
		}
		return;
	}
	if (b < 9) {
		uint8_t at = (uint8_t)(5 * b), n = (uint8_t)(DSTAR_HDR_BYTES - at < 5 ? DSTAR_HDR_BYTES - at : 5);

		s->block[0] = (uint8_t)(DSTAR_MINI_HEADER | n);
		memcpy(s->block + 1, s->hdr + at, n);
	}
}

void dstar_slow_tx_frame(dstar_slow_tx_t *s, uint8_t n, uint8_t data[DSTAR_DATA_BYTES])
{
	if (n & 1)
		tx_block(s, (uint8_t)((n - 1) / 2));
	memcpy(data, s->block + ((n & 1) ? 0 : 3), DSTAR_DATA_BYTES);
	if (n == DSTAR_SUPERFRAME - 1)
		s->sf++;
}
