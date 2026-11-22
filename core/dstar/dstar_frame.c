// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV framing (dstar_frame.h)
#include <string.h>
#include "dstar/dstar_frame.h"

// The patterns as the standard writes them, left to right = first on air, packed in air order:
//   frame sync 111011001010000 (4.1.1 b)
//   resync 1010101010 1101000 1101000 (4.1.2 d)
//   end: 32 bits of 1010..., 000100110101111, 0 (4.1.2 h)
const uint8_t dstar_frame_sync[2] = { 0x37u, 0x05u };
const uint8_t dstar_resync[3] = { 0x55u, 0x2Du, 0x16u };
const uint8_t dstar_end[6] = { 0x55u, 0x55u, 0x55u, 0x55u, 0xC8u, 0x7Au };

const uint8_t dstar_voice_silence[DSTAR_VOICE_BYTES] = { 0x9Eu, 0x8Du, 0x32u, 0x88u, 0x26u, 0x1Au, 0x3Fu, 0x61u, 0xE8u };

// Mismatches allowed. The resync pattern alone (24 bits) starts a late entry, so it is held tighter there than when
// checked at the place it is due; the end pattern (48 bits) is looked for at every bit while voice frames come in
#define RESYNC_ENTRY_ERRS	1u
#define RESYNC_TRACK_ERRS	4u
#define END_ERRS		5u
// Resync patterns missed in a row before the transmission counts as lost (a fade, or a transmitter gone without its
// end pattern): 3 superframes, 1.26 s, about what a squelch tail lets through
#define SYNC_MISS_LOST		3u

static void pad(char *dst, const char *src, size_t n)
{
	size_t i = 0;
	for (; src && src[i] && i < n; i++) {
		dst[i] = src[i];
	}
	for (; i < n; i++) {
		dst[i] = ' ';
	}
}

void dstar_header_set(dstar_header_t *h, uint8_t flag1, const char *rpt2, const char *rpt1, const char *ur,
		      const char *my, const char *suffix)
{
	memset(h, 0, sizeof(*h));
	h->flag[0] = flag1;
	pad(h->rpt2, rpt2, DSTAR_CALL_LEN);
	pad(h->rpt1, rpt1, DSTAR_CALL_LEN);
	pad(h->ur, ur, DSTAR_CALL_LEN);
	pad(h->my, my, DSTAR_CALL_LEN);
	pad(h->suffix, suffix, DSTAR_SUFFIX_LEN);
}

void dstar_header_pack(const dstar_header_t *h, uint8_t out[DSTAR_HEADER_BYTES])
{
	memcpy(&out[0], h->flag, 3);
	memcpy(&out[3], h->rpt2, DSTAR_CALL_LEN);
	memcpy(&out[11], h->rpt1, DSTAR_CALL_LEN);
	memcpy(&out[19], h->ur, DSTAR_CALL_LEN);
	memcpy(&out[27], h->my, DSTAR_CALL_LEN);
	memcpy(&out[35], h->suffix, DSTAR_SUFFIX_LEN);
	dstar_header_crc_set(out);
}

bool dstar_header_unpack(const uint8_t in[DSTAR_HEADER_BYTES], dstar_header_t *h)
{
	memcpy(h->flag, &in[0], 3);
	memcpy(h->rpt2, &in[3], DSTAR_CALL_LEN);
	memcpy(h->rpt1, &in[11], DSTAR_CALL_LEN);
	memcpy(h->ur, &in[19], DSTAR_CALL_LEN);
	memcpy(h->my, &in[27], DSTAR_CALL_LEN);
	memcpy(h->suffix, &in[35], DSTAR_SUFFIX_LEN);
	return dstar_header_crc_ok(in);
}

void dstar_field_text(char *out, const char *field, size_t n)
{
	while (n > 0 && (field[n - 1] == ' ' || field[n - 1] == '\0')) {
		n--;
	}
	for (size_t i = 0; i < n; i++) {
		uint8_t c = (uint8_t)field[i];
		out[i] = c >= 0x20u && c <= 0x7Eu ? (char)c : '?';
	}
	out[n] = '\0';
}

// --- slow data ---

void dstar_slow_tx_init(dstar_slow_tx_t *s, const uint8_t header[DSTAR_HEADER_BYTES], const char *msg)
{
	memset(s, 0, sizeof(*s));
	memcpy(s->header, header, DSTAR_HEADER_BYTES);
	if (msg && msg[0]) {
		pad(s->msg, msg, DSTAR_MSG_LEN);
		s->has_msg = true;
	}
}

static void slow_block(dstar_slow_tx_t *s, uint32_t sf, uint8_t block)
{
	uint8_t *b = s->block;
	memset(b, DSTAR_MINI_NONE, DSTAR_SLOW_BLOCK);
	if (s->has_msg && (sf & 1u) == 0) {
		if (block < 4u) {
			b[0] = (uint8_t)(DSTAR_MINI_MESSAGE | block);
			memcpy(&b[1], &s->msg[block * 5u], 5);
		}
		return;
	}
	uint32_t at = block * 5u;
	if (at < DSTAR_HEADER_BYTES) {
		uint32_t n = DSTAR_HEADER_BYTES - at < 5u ? DSTAR_HEADER_BYTES - at : 5u;
		b[0] = (uint8_t)(DSTAR_MINI_HEADER | n);
		memcpy(&b[1], &s->header[at], n);
	}
}

void dstar_slow_tx_frame(dstar_slow_tx_t *s, uint32_t sf, uint8_t frame, uint8_t out[3])
{
	uint8_t block = (uint8_t)((frame - 1u) / 2u);
	if (((frame - 1u) & 1u) == 0) {
		slow_block(s, sf, block);
		memcpy(out, &s->block[0], 3);
	} else {
		memcpy(out, &s->block[3], 3);
	}
}

void dstar_slow_rx_init(dstar_slow_rx_t *s)
{
	memset(s, 0, sizeof(*s));
	memset(s->msg, ' ', DSTAR_MSG_LEN);
}

static dstar_slow_event_t slow_rx_block(dstar_slow_rx_t *s, const uint8_t *b)
{
	uint8_t kind = b[0] & 0xF0u, n = b[0] & 0x0Fu;
	if (kind == DSTAR_MINI_MESSAGE && n < 4u) {
		// A message block that differs from what we hold starts a new message: the sender changed it, or this is a
		// different station after a fade
		if ((s->msg_blocks & (1u << n)) && memcmp(&s->msg[n * 5u], &b[1], 5) != 0) {
			s->msg_blocks = 0;
		}
		memcpy(&s->msg[n * 5u], &b[1], 5);
		bool was_whole = s->msg_blocks == 0x0Fu;
		s->msg_blocks |= (uint8_t)(1u << n);
		// Delivered once per message, when the last missing block comes in
		return s->msg_blocks == 0x0Fu && !was_whole ? DSTAR_SLOW_MESSAGE : DSTAR_SLOW_NOTHING;
	}
	if (kind == DSTAR_MINI_HEADER && n >= 1u && n <= 5u && !s->hdr_bad) {
		if (s->hdr_len + n > DSTAR_HEADER_BYTES) {
			s->hdr_bad = true;
			return DSTAR_SLOW_NOTHING;
		}
		memcpy(&s->hdr[s->hdr_len], &b[1], n);
		s->hdr_len = (uint8_t)(s->hdr_len + n);
		if (s->hdr_len == DSTAR_HEADER_BYTES) {
			s->hdr_bad = true;	// one per superframe
			return dstar_header_crc_ok(s->hdr) ? DSTAR_SLOW_HEADER : DSTAR_SLOW_NOTHING;
		}
		return DSTAR_SLOW_NOTHING;
	}
	if (s->hdr_len) {
		s->hdr_bad = true;	// the header's run of blocks broke
	}
	return DSTAR_SLOW_NOTHING;
}

dstar_slow_event_t dstar_slow_rx_frame(dstar_slow_rx_t *s, uint8_t frame, const uint8_t d[3], bool ok)
{
	if (frame == 0) {
		// The repeated header starts at the first block after the resync frame (standard 6.4)
		s->have_half = false;
		s->hdr_len = 0;
		s->hdr_bad = false;
		return DSTAR_SLOW_NOTHING;
	}
	bool first = ((frame - 1u) & 1u) == 0;
	if (!ok) {
		s->have_half = false;
		if (s->hdr_len) {
			s->hdr_bad = true;
		}
		return DSTAR_SLOW_NOTHING;
	}
	if (first) {
		memcpy(s->half, d, 3);
		s->have_half = true;
		return DSTAR_SLOW_NOTHING;
	}
	if (!s->have_half) {
		if (s->hdr_len) {
			s->hdr_bad = true;
		}
		return DSTAR_SLOW_NOTHING;
	}
	s->have_half = false;
	uint8_t b[DSTAR_SLOW_BLOCK];
	memcpy(b, s->half, 3);
	memcpy(&b[3], d, 3);
	return slow_rx_block(s, b);
}

// --- transmit ---

void dstar_tx_init(dstar_tx_t *t, const dstar_header_t *h, const char *msg, uint16_t preamble_bits,
		   dstar_voice_pull_t pull, void *ctx)
{
	memset(t, 0, sizeof(*t));
	t->preamble = preamble_bits < DSTAR_PREAMBLE_MIN ? (uint16_t)DSTAR_PREAMBLE_MIN : preamble_bits;
	t->pull = pull;
	t->ctx = ctx;
	uint8_t hdr[DSTAR_HEADER_BYTES];
	dstar_header_pack(h, hdr);
	dstar_slow_tx_init(&t->slow, hdr, msg);
	dstar_header_encode(hdr, t->air);
}

// The next frame into t->air (voice then data); false when the speaker is done
static bool next_frame(dstar_tx_t *t)
{
	uint8_t *v = t->air, *d = &t->air[DSTAR_VOICE_BYTES];
	if (t->pull) {
		if (!t->pull(t->ctx, v)) {
			return false;
		}
	} else {
		memcpy(v, dstar_voice_silence, DSTAR_VOICE_BYTES);
	}
	uint8_t frame = (uint8_t)(t->frames % DSTAR_SUPERFRAME);
	if (frame == 0) {
		memcpy(d, dstar_resync, 3);	// the resync pattern isn't scrambled (appendix 1.3)
	} else {
		dstar_slow_tx_frame(&t->slow, t->frames / DSTAR_SUPERFRAME, frame, d);
		dstar_data_scramble(d);
	}
	t->frames++;
	return true;
}

int dstar_tx_bit(dstar_tx_t *t)
{
	for (;;) {
		switch (t->state) {
		case DSTAR_TX_PREAMBLE:
			// 1010...: the preamble ends on a 0 so the frame sync's leading 1 follows a transition
			if (t->bit < t->preamble) {
				uint32_t b = t->bit++;
				return (int)(((t->preamble - b) & 1u) == 0 ? 1 : 0);
			}
			t->state = DSTAR_TX_SYNC;
			t->bit = 0;
			break;
		case DSTAR_TX_SYNC:
			if (t->bit < DSTAR_FRAME_SYNC_BITS) {
				return dstar_bit(dstar_frame_sync, t->bit++);
			}
			t->state = DSTAR_TX_HEADER;
			t->bit = 0;
			break;
		case DSTAR_TX_HEADER:
			if (t->bit < DSTAR_HEADER_BITS) {
				return dstar_bit(t->air, t->bit++);
			}
			t->state = DSTAR_TX_VOICE;
			t->bit = DSTAR_FRAME_BITS;
			break;
		case DSTAR_TX_VOICE:
			if (t->bit >= DSTAR_FRAME_BITS) {
				if (!next_frame(t)) {
					t->state = DSTAR_TX_END;
					t->bit = 0;
					break;
				}
				t->bit = 0;
			}
			return dstar_bit(t->air, t->bit++);
		case DSTAR_TX_END:
			if (t->bit < DSTAR_END_BITS) {
				return dstar_bit(dstar_end, t->bit++);
			}
			t->state = DSTAR_TX_DONE;
			break;
		case DSTAR_TX_DONE:
		default:
			return -1;
		}
	}
}

// --- receive ---

void dstar_rx_init(dstar_rx_t *r, const dstar_rx_cb_t *cb)
{
	memset(r, 0, sizeof(*r));
	r->polarity = 1;
	if (cb) {
		r->cb = *cb;
	}
	dstar_slow_rx_init(&r->slow);
}

// The patterns as the window holds them, newest bit lowest: the standard's left-to-right strings read as binary. The
// window is compared with an XOR and a count of ones: the bit-by-bit loop it replaces was estimated at 3 % of the CPU at
// 168 MHz while searching
#define W_FRAME_SYNC		0x7650ull		// 111011001010000
#define W_RESYNC		0xAAB468ull		// 1010101010 1101000 1101000
#define W_END			0xAAAAAAAA135Eull	// (10) x 16, 000100110101111, 0
#define W_ALTERNATING		0xAAAAAAull		// 1010...

static uint32_t ones32(uint32_t v)
{
	v = v - ((v >> 1) & 0x55555555u);
	v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
	return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}

// Mismatches between the window's last n bits (n < 64) and a pattern word, or its inverse
static uint32_t distance(uint64_t win, uint64_t pat, uint32_t n, bool invert)
{
	uint64_t x = (win ^ pat ^ (invert ? ~0ull : 0ull)) & ((1ull << n) - 1u);
	return ones32((uint32_t)x) + ones32((uint32_t)(x >> 32));
}

// The frame sync, with 24 bits of preamble before it. The preamble is matched in either phase: the standard's 64 bits
// of 1010 end on a 0 (and ours do), but nothing is lost by not relying on it. The polarity is the frame sync's
#define PREAMBLE_CHECK_BITS	24u
#define PREAMBLE_ERRS		2u
#define FRAME_SYNC_ERRS		1u

static void start_voice(dstar_rx_t *r, uint8_t frame, bool confirmed)
{
	r->state = DSTAR_RX_VOICE;
	r->frame = frame;
	r->bit = 0;
	r->sync_miss = 0;
	r->confirmed = confirmed;
	dstar_slow_rx_init(&r->slow);
}

static void end(dstar_rx_t *r, dstar_end_t why)
{
	bool announced = r->confirmed;
	r->state = DSTAR_RX_SEARCH;
	r->header_seen = false;
	r->confirmed = false;
	if (!announced) {
		return;		// a late entry that never confirmed: noise that looked like a resync pattern
	}
	if (why == DSTAR_END_PATTERN) {
		r->ends++;
	} else {
		r->losts++;
	}
	if (r->cb.end) {
		r->cb.end(r->cb.ctx, why);
	}
}

static void search(dstar_rx_t *r)
{
	uint64_t pre = r->win >> DSTAR_FRAME_SYNC_BITS;
	if (distance(pre, W_ALTERNATING, PREAMBLE_CHECK_BITS, false) <= PREAMBLE_ERRS ||
	    distance(pre, W_ALTERNATING, PREAMBLE_CHECK_BITS, true) <= PREAMBLE_ERRS) {
		for (int inv = 0; inv < 2; inv++) {
			if (distance(r->win, W_FRAME_SYNC, DSTAR_FRAME_SYNC_BITS, inv != 0) <= FRAME_SYNC_ERRS) {
				r->polarity = inv ? -1 : 1;
				r->state = DSTAR_RX_HEADER;
				r->bit = 0;
				return;
			}
		}
	}
	for (int inv = 0; inv < 2; inv++) {
		if (distance(r->win, W_RESYNC, DSTAR_DATA_BITS, inv != 0) <= RESYNC_ENTRY_ERRS) {
			// Late entry: the resync pattern ends frame 0, so the next bit starts frame 1's voice. Nothing is handed
			// on until the next resync pattern turns up where it is due (420 ms): noise matches a 24-bit pattern
			// about once a minute. The header comes from the slow data
			r->polarity = inv ? -1 : 1;
			start_voice(r, 1, false);
			return;
		}
	}
}

static void header_done(dstar_rx_t *r)
{
	uint8_t hdr[DSTAR_HEADER_BYTES];
	dstar_header_result_t res = dstar_header_decode(r->soft, hdr);
	r->last_corrected = res.corrected;
	if (!res.crc_ok) {
		// A bad header still leaves voice frames to follow: look for the first resync pattern as a late entry
		r->headers_bad++;
		r->state = DSTAR_RX_SEARCH;
		return;
	}
	r->headers++;
	r->header_seen = true;
	if (r->cb.header) {
		dstar_header_t h;
		dstar_header_unpack(hdr, &h);
		r->cb.header(r->cb.ctx, &h, false);
	}
	// The header is followed by frame 0, whose data frame is the resync pattern
	start_voice(r, 0, true);
}

static void frame_done(dstar_rx_t *r)
{
	uint8_t *v = r->cur, *d = &r->cur[DSTAR_VOICE_BYTES];
	if (r->confirmed) {
		r->frames++;
		if (r->cb.voice) {
			r->cb.voice(r->cb.ctx, v, r->frame);
		}
	}
	if (r->frame != 0) {
		dstar_data_scramble(d);
	}
	dstar_slow_event_t ev = dstar_slow_rx_frame(&r->slow, r->frame, d, true);
	if (ev == DSTAR_SLOW_MESSAGE && r->confirmed && r->cb.message) {
		r->cb.message(r->cb.ctx, r->slow.msg);
	} else if (ev == DSTAR_SLOW_HEADER) {
		r->slow_headers++;
		if (!r->header_seen && r->confirmed) {
			r->header_seen = true;
			if (r->cb.header) {
				dstar_header_t h;
				dstar_header_unpack(r->slow.hdr, &h);
				r->cb.header(r->cb.ctx, &h, true);
			}
		}
	}
	r->frame = (uint8_t)((r->frame + 1u) % DSTAR_SUPERFRAME);
	r->bit = 0;
}

void dstar_rx_bit(dstar_rx_t *r, int16_t soft, int16_t soft_unit)
{
	r->win = (r->win << 1) | (soft > 0 ? 1u : 0u);
	int32_t s = (int32_t)soft * r->polarity;
	bool b = s > 0;

	switch (r->state) {
	case DSTAR_RX_SEARCH:
		search(r);
		break;
	case DSTAR_RX_HEADER: {
		int32_t q = soft_unit > 0 ? (s * DSTAR_SOFT_MAX) / soft_unit : (b ? DSTAR_SOFT_MAX : -DSTAR_SOFT_MAX);
		if (q == 0) {
			q = b ? 1 : -1;	// keep the decision; the magnitude says it is weak
		}
		dstar_soft_put(r->soft, r->bit, (int8_t)(q > DSTAR_SOFT_MAX ? DSTAR_SOFT_MAX : q < -DSTAR_SOFT_MAX ? -DSTAR_SOFT_MAX : q));
		if (++r->bit == DSTAR_HEADER_BITS) {
			header_done(r);
		}
		break;
	}
	case DSTAR_RX_VOICE:
		if (distance(r->win, W_END, DSTAR_END_BITS, r->polarity < 0) <= END_ERRS) {
			end(r, DSTAR_END_PATTERN);
			break;
		}
		dstar_bit_put(r->cur, r->bit, b);
		if (++r->bit < DSTAR_FRAME_BITS) {
			break;
		}
		if (r->frame == 0) {
			if (distance(r->win, W_RESYNC, DSTAR_DATA_BITS, r->polarity < 0) <= RESYNC_TRACK_ERRS) {
				if (!r->confirmed) {
					r->late_entries++;
				}
				r->confirmed = true;
				r->syncs++;
				r->sync_miss = 0;
			} else {
				r->sync_misses++;
				if (!r->confirmed || ++r->sync_miss >= SYNC_MISS_LOST) {
					end(r, DSTAR_END_LOST);
					break;
				}
			}
		}
		frame_done(r);
		break;
	}
}
