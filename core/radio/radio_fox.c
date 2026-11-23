// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fox mode's record and schedule (radio_fox.h, docs/spec/radio-features.md 3.8)
#include "config.h"
#if CONFIG_FOX	// the whole file
#include <string.h>
#include "radio/radio_fox.h"
#include "radio/morse.h"
#include "radio/sigtone.h"

// The default tune: a rising major arpeggio and a held top note, written for this (frequencies on the SIG_HZ_UNIT grid)
static const beep_seg_t s_tune[] = {
	{ 520, 120 }, { 660, 120 }, { 780, 120 }, { 0, 60 }, { 1050, 240 }, { 0, 80 }, { 780, 120 }, { 1050, 360 },
};

static const char s_msg[] = "FOX";

void fox_rec_default(fox_rec_t *r)
{
	memset(r, 0, sizeof(*r));
	r->interval = FOX_INTERVAL_DEFAULT;
	r->flags = FOX_F_MSG | FOX_F_JINGLE;
	memcpy(r->msg, s_msg, sizeof(s_msg) - 1u);
}

bool fox_msg_char_ok(char c)
{
	return c == ' ' || morse_code(c) != 0;
}

uint8_t fox_msg_len(const fox_rec_t *r)
{
	uint8_t n = 0;
	while (n < FOX_MSG_MAX && r->msg[n] != '\0') {
		n++;
	}
	return n;
}

// Byte offsets in the record: interval, flags, message, the frequencies, the lengths
#define REC_MSG		2u
#define REC_HZ		(REC_MSG + FOX_MSG_MAX)
#define REC_MS		(REC_HZ + 2u * FOX_STEPS)

bool fox_rec_decode(const uint8_t *v, int len, fox_rec_t *r)
{
	uint8_t b[FOX_REC_LEN] = { 0 };
	if (len < 1) {
		return false;
	}
	memcpy(b, v, (unsigned)len < FOX_REC_LEN ? (unsigned)len : FOX_REC_LEN);
	r->interval = b[0];
	r->flags = b[1];
	if (r->interval >= FOX_INTERVALS || (r->flags & ~FOX_F_MASK) != 0) {
		return false;
	}
	bool end = false;
	for (unsigned i = 0; i < FOX_MSG_MAX; i++) {
		char c = (char)b[REC_MSG + i];
		r->msg[i] = c;
		if (c == '\0') {
			end = true;
		} else if (end || !fox_msg_char_ok(c)) {
			return false;	// text after the padding, or a character with no Morse
		}
	}
	for (unsigned i = 0; i < FOX_STEPS; i++) {
		r->hz[i] = (uint16_t)(b[REC_HZ + 2u * i] | b[REC_HZ + 2u * i + 1u] << 8);
		r->ms10[i] = b[REC_MS + i];
		if (r->hz[i] > SIG_HZ_MAX || r->hz[i] % SIG_HZ_UNIT != 0) {
			return false;
		}
	}
	return true;
}

void fox_rec_encode(const fox_rec_t *r, uint8_t out[FOX_REC_LEN])
{
	out[0] = r->interval;
	out[1] = r->flags;
	memcpy(&out[REC_MSG], r->msg, FOX_MSG_MAX);
	for (unsigned i = 0; i < FOX_STEPS; i++) {
		out[REC_HZ + 2u * i] = (uint8_t)(r->hz[i] & 0xFFu);
		out[REC_HZ + 2u * i + 1u] = (uint8_t)(r->hz[i] >> 8);
		out[REC_MS + i] = r->ms10[i];
	}
}

uint32_t fox_interval_ms(uint8_t code)
{
	static const uint16_t s[FOX_INTERVALS] = { 15, 30, 60, 120, 300, 600 };
	return code < FOX_INTERVALS ? s[code] * 1000u : 0;
}

uint8_t fox_jingle(const fox_rec_t *r, beep_seg_t out[FOX_STEPS])
{
	uint8_t n = sig_chain_n(r->hz, r->ms10, FOX_STEPS, FOX_MAX_MS, out);
	if (n != 0) {
		return n;
	}
	_Static_assert(sizeof(s_tune) / sizeof(s_tune[0]) <= FOX_STEPS, "the default tune fits a jingle");
	memcpy(out, s_tune, sizeof(s_tune));
	return (uint8_t)(sizeof(s_tune) / sizeof(s_tune[0]));
}

uint32_t fox_cycle_ms(const fox_rec_t *r, const char *call, uint8_t call_len, uint8_t wpm)
{
	uint32_t dot = morse_dot_ms(wpm);
	uint32_t t = FOX_LEAD_MS;
	bool sent = false;
	uint8_t n = fox_msg_len(r);
	if ((r->flags & FOX_F_MSG) != 0 && n != 0) {
		t += morse_units(r->msg, n) * dot;
		sent = true;
	}
	if ((r->flags & FOX_F_JINGLE) != 0) {
		beep_seg_t seg[FOX_STEPS];
		uint8_t m = fox_jingle(r, seg);
		t += sent ? FOX_GAP_MS : 0;
		for (uint8_t i = 0; i < m; i++) {
			t += seg[i].ms;
		}
		sent = true;
	}
	t += sent ? FOX_GAP_MS : 0;
	t += morse_units(call, call_len) * dot;
	return t + FOX_TAIL_MS;
}

// --- The schedule ---

void radio_fox_start(radio_fox_t *f, uint8_t parts, uint32_t interval_ms, uint32_t now_ms)
{
	f->phase = FOX_WAIT;
	f->parts = parts & FOX_F_MASK;
	f->interval_ms = interval_ms;
	f->at = now_ms;
	f->cycles = 0;
	f->cut = 0;
}

void radio_fox_stop(radio_fox_t *f)
{
	f->phase = FOX_OFF;
}

static bool due(const radio_fox_t *f, uint32_t now_ms)
{
	return (int32_t)(now_ms - f->at) >= 0;
}

// The phase after p in this run, skipping the parts it doesn't send (and their gaps): a gap goes before a part only
// when something was sent before it
static uint8_t next_phase(const radio_fox_t *f, uint8_t p)
{
	bool msg = (f->parts & FOX_F_MSG) != 0, jingle = (f->parts & FOX_F_JINGLE) != 0;
	switch (p) {
	case FOX_LEAD:
		return msg ? FOX_MSG : jingle ? FOX_JINGLE : FOX_ID;
	case FOX_MSG:
		return jingle ? FOX_GAP_JINGLE : FOX_GAP_ID;
	case FOX_GAP_JINGLE:
		return FOX_JINGLE;
	case FOX_JINGLE:
		return FOX_GAP_ID;
	case FOX_GAP_ID:
		return FOX_ID;
	case FOX_ID:
		return FOX_TAIL;
	default:
		return FOX_WAIT;
	}
}

fox_act_t radio_fox_poll(radio_fox_t *f, bool part_done, uint32_t now_ms)
{
	switch (f->phase) {
	case FOX_WAIT:
		return due(f, now_ms) ? FOX_ACT_KEY : FOX_ACT_NONE;
	case FOX_LEAD:
	case FOX_GAP_JINGLE:
	case FOX_GAP_ID:
	case FOX_TAIL:
		if (!due(f, now_ms)) {
			return FOX_ACT_NONE;
		}
		break;
	case FOX_MSG:
	case FOX_JINGLE:
	case FOX_ID:
		if (!part_done) {
			return FOX_ACT_NONE;
		}
		break;
	default:
		return FOX_ACT_NONE;
	}
	f->phase = next_phase(f, f->phase);
	switch (f->phase) {
	case FOX_MSG:
		return FOX_ACT_MSG;
	case FOX_JINGLE:
		return FOX_ACT_JINGLE;
	case FOX_ID:
		return FOX_ACT_ID;
	case FOX_GAP_JINGLE:
	case FOX_GAP_ID:
		f->at = now_ms + FOX_GAP_MS;
		return FOX_ACT_QUIET;
	case FOX_TAIL:
		f->at = now_ms + FOX_TAIL_MS;
		return FOX_ACT_QUIET;
	default:	// FOX_WAIT: the cycle is over
		f->cycles++;
		f->at = now_ms + f->interval_ms;
		return FOX_ACT_UNKEY;
	}
}

void radio_fox_keyed(radio_fox_t *f, uint32_t now_ms)
{
	if (f->phase == FOX_WAIT && due(f, now_ms)) {	// only the key-up FOX_ACT_KEY asked for
		f->phase = FOX_LEAD;
		f->at = now_ms + FOX_LEAD_MS;
	}
}

void radio_fox_retry(radio_fox_t *f, uint32_t now_ms)
{
	if (f->phase != FOX_OFF) {
		f->phase = FOX_WAIT;
		f->at = now_ms + FOX_BUSY_RETRY_MS;
	}
}

void radio_fox_cut(radio_fox_t *f, uint32_t now_ms)
{
	if (f->phase != FOX_OFF) {
		f->cut++;
		f->phase = FOX_WAIT;
		f->at = now_ms + f->interval_ms;
	}
}

bool radio_fox_on_air(const radio_fox_t *f)
{
	return f->phase >= FOX_LEAD && f->phase <= FOX_TAIL;
}

uint32_t radio_fox_wait_left(const radio_fox_t *f, uint32_t now_ms)
{
	if (f->phase != FOX_WAIT || due(f, now_ms)) {
		return 0;
	}
	return f->at - now_ms;
}

#endif	// CONFIG_FOX
