// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV channels in the codeplug (dstar_codeplug.h, docs/spec/dstar-codeplug.md)
#include <string.h>
#include "config.h"
#include "dstar/dstar_codeplug.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"

#if CONFIG_DSTAR

// The characters a callsign field keeps (1): A-Z, 0-9, space and '/'. Lower case is taken as upper, everything else
// (NUL and 0xFF padding included) is a space. Callsigns and the gateway commands need no more, and a narrower set
// means a stored byte another tool wrote can never put a control character or a non-ASCII byte into a header
static char call_char(uint8_t c)
{
	if (c >= 'a' && c <= 'z') {
		c = (uint8_t)(c - ('a' - 'A'));
	}
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' ? (char)c : ' ';
}

// The message keeps printable ASCII as written: it is free text another radio shows as sent
static char msg_char(uint8_t c)
{
	return c >= 0x20u && c <= 0x7Eu ? (char)c : ' ';
}

// n stored bytes from at into dst through clean; bytes past len read as 0, a space
static void field_read(char *dst, const uint8_t *b, int len, int at, unsigned n, char (*clean)(uint8_t))
{
	for (unsigned i = 0; i < n; i++) {
		int k = at + (int)i;
		dst[i] = clean(b != NULL && k < len ? b[k] : 0u);
	}
}

static void field_write(uint8_t *out, const char *src, unsigned n, char (*clean)(uint8_t))
{
	for (unsigned i = 0; i < n; i++) {
		out[i] = (uint8_t)clean((uint8_t)src[i]);
	}
}

static bool blank(const char *f, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		if (f[i] != ' ') {
			return false;
		}
	}
	return true;
}

void dstar_chan_decode(const uint8_t *b, int len, dstar_chan_t *out)
{
	field_read(out->ur, b, len, 0, DSTAR_CALL_LEN, call_char);
	field_read(out->rpt1, b, len, DSTAR_CALL_LEN, DSTAR_CALL_LEN, call_char);
	field_read(out->rpt2, b, len, 2 * DSTAR_CALL_LEN, DSTAR_CALL_LEN, call_char);
	if (blank(out->ur, DSTAR_CALL_LEN)) {
		memcpy(out->ur, DSTAR_CP_UR_CQ, DSTAR_CALL_LEN);	// a zeroed block calls CQ
	}
	// An erased byte is no flag: 0xFF would otherwise read as "via a repeater" on a block nobody wrote
	uint8_t f = b != NULL && len >= (int)DSTAR_CP_CHAN_LEN ? b[DSTAR_CP_CHAN_LEN - 1u] : 0u;
	out->flags = f == 0xFFu ? 0u : (uint8_t)(f & DSTAR_CP_VIA_RPT);
}

void dstar_chan_encode(const dstar_chan_t *c, uint8_t out[DSTAR_CP_CHAN_LEN])
{
	field_write(&out[0], c->ur, DSTAR_CALL_LEN, call_char);
	field_write(&out[DSTAR_CALL_LEN], c->rpt1, DSTAR_CALL_LEN, call_char);
	field_write(&out[2 * DSTAR_CALL_LEN], c->rpt2, DSTAR_CALL_LEN, call_char);
	out[DSTAR_CP_CHAN_LEN - 1u] = (uint8_t)(c->flags & DSTAR_CP_VIA_RPT);
}

void dstar_station_decode(const uint8_t *b, int len, dstar_station_t *out)
{
	field_read(out->my, b, len, 0, DSTAR_CALL_LEN, call_char);
	field_read(out->suffix, b, len, DSTAR_CALL_LEN, DSTAR_SUFFIX_LEN, call_char);
	field_read(out->msg, b, len, DSTAR_CALL_LEN + DSTAR_SUFFIX_LEN, DSTAR_MSG_LEN, msg_char);
}

void dstar_station_encode(const dstar_station_t *s, uint8_t out[DSTAR_CP_STATION_LEN])
{
	field_write(&out[0], s->my, DSTAR_CALL_LEN, call_char);
	field_write(&out[DSTAR_CALL_LEN], s->suffix, DSTAR_SUFFIX_LEN, call_char);
	field_write(&out[DSTAR_CALL_LEN + DSTAR_SUFFIX_LEN], s->msg, DSTAR_MSG_LEN, msg_char);
}

bool dstar_chan_load(uint16_t number, const channel_t *ch, dstar_chan_t *out)
{
	uint8_t b[DSTAR_CP_CHAN_LEN + CODEPLUG_TAG_HEAD_MAX];
	if (ch->kind != CODEPLUG_KIND_DSTAR) {
		return false;
	}
	int len = codeplug_tag_params(number, ch, b, DSTAR_CP_CHAN_LEN);
	if (len < 0) {
		return false;
	}
	dstar_chan_decode(b, len < (int)DSTAR_CP_CHAN_LEN ? len : (int)DSTAR_CP_CHAN_LEN, out);
	return true;
}

bool dstar_station_load(dstar_station_t *out)
{
	uint8_t b[DSTAR_CP_STATION_LEN];
	int len = codeplug_mode_settings(CODEPLUG_KIND_DSTAR, b, sizeof b);
	dstar_station_decode(b, len < 0 ? -1 : (len < (int)sizeof b ? len : (int)sizeof b), out);
	return len >= 0;
}

static void text_set(char *dst, size_t len, const char *src, char (*clean)(uint8_t))
{
	size_t i = 0;
	for (; src != NULL && i < len && src[i] != '\0'; i++) {
		dst[i] = clean((uint8_t)src[i]);
	}
	for (; i < len; i++) {
		dst[i] = ' ';
	}
}

void dstar_field_set(char *dst, size_t len, const char *src)
{
	text_set(dst, len, src, call_char);
}

void dstar_msg_set(char dst[DSTAR_MSG_LEN], const char *src)
{
	text_set(dst, DSTAR_MSG_LEN, src, msg_char);
}

// The longest stored block kept on a rewrite: the most the generic layer writes (channel-modes.md 2.1)
#define KEEP_MAX	64u

bool dstar_chan_write(uint16_t number, const char *name, const dstar_chan_t *c, bool tx)
{
	uint8_t b[KEEP_MAX + CODEPLUG_TAG_HEAD_MAX];
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_DSTAR, .system = CODEPLUG_SYSTEM_NONE };
	uint16_t len = DSTAR_CP_CHAN_LEN;
	uint8_t keep_tag = 0u;		// tag flags kept: reserved bit 3, the kind's bits 4-7
	uint8_t keep_blk = 0u;		// block flags kept: the reserved bits 1-7

	// This channel's own tag, live under this name: keep what a later firmware or the editor put there that this one
	// doesn't know. Any other tag at the number (stale, another kind) is replaced whole. Base-own and fallback are
	// dropped: the caller wrote the stand-in record, and D-STAR has no fallback
	codeplug_tag_t old;
	if (codeplug_tag_read(number, &old) && old.kind == CODEPLUG_KIND_DSTAR) {
		channel_t ch;
		memset(&ch, 0, sizeof ch);
		ch.kind = CODEPLUG_KIND_DSTAR;
		strncpy(ch.name, name, CODEPLUG_NAME_LEN);
		int n = codeplug_tag_params(number, &ch, b, KEEP_MAX);
		if (n >= 0) {
			keep_tag = (uint8_t)(old.flags & ~(CODEPLUG_TAG_TX | CODEPLUG_TAG_BASE_OWN | CODEPLUG_TAG_FALLBACK));
			if (n >= (int)DSTAR_CP_CHAN_LEN && b[DSTAR_CP_CHAN_LEN - 1u] != 0xFFu) {
				keep_blk = (uint8_t)(b[DSTAR_CP_CHAN_LEN - 1u] & ~DSTAR_CP_VIA_RPT);
			}
			if (n > (int)DSTAR_CP_CHAN_LEN) {
				len = (uint16_t)(n < (int)KEEP_MAX ? n : (int)KEEP_MAX);	// the tail stays in b
			}
		}
	}
	dstar_chan_encode(c, b);
	b[DSTAR_CP_CHAN_LEN - 1u] |= keep_blk;
	t.flags = (uint8_t)(keep_tag | (tx ? CODEPLUG_TAG_TX : 0u));
	return codeplug_tag_write(number, name, &t, b, len);
}

#endif
