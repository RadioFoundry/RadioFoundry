// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion channels in the codeplug (ysf_codeplug.h, docs/spec/ysf-codeplug.md)
#include <string.h>
#include "config.h"
#include "ysf/ysf_codeplug.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"

#if CONFIG_YSF

// The characters a callsign field keeps (1): A-Z, 0-9, space, '/' and '-'. Lower case is taken as upper, everything
// else (NUL and 0xFF padding included) is a space: a byte another tool stored never puts a control character or a
// non-ASCII byte on air or on another station's screen
static char call_char(uint8_t c)
{
	if (c >= 'a' && c <= 'z') {
		c = (uint8_t)(c - ('a' - 'A'));
	}
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' ? (char)c : ' ';
}

static void field_read(char *dst, const uint8_t *b, int len, int at, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		int k = at + (int)i;
		dst[i] = call_char(b != NULL && k < len ? b[k] : 0u);
	}
}

static void field_write(uint8_t *out, const char *src, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		out[i] = (uint8_t)call_char((uint8_t)src[i]);
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

// A DG-ID byte: 0xFF (erased) and anything past 99 read as 0
static uint8_t dgid(const uint8_t *b, int len, int at)
{
	uint8_t v = b != NULL && at < len ? b[at] : 0u;
	return v <= YSF_CP_DGID_MAX ? v : 0u;
}

void ysf_chan_decode(const uint8_t *b, int len, ysf_chan_t *out)
{
	field_read(out->dest, b, len, 0, YSF_CALLSIGN_LEN);
	if (blank(out->dest, YSF_CALLSIGN_LEN)) {
		memcpy(out->dest, YSF_CP_DEST_ALL, YSF_CALLSIGN_LEN);	// a zeroed block is a group call
	}
	out->tx_dgid = dgid(b, len, YSF_CALLSIGN_LEN);
	out->rx_dgid = dgid(b, len, YSF_CALLSIGN_LEN + 1);
	uint8_t f = b != NULL && len >= (int)YSF_CP_CHAN_LEN ? b[YSF_CP_CHAN_LEN - 1u] : 0u;
	out->flags = f == 0xFFu ? 0u : (uint8_t)(f & YSF_CP_FLAGS);	// an erased byte is no flag
}

void ysf_chan_encode(const ysf_chan_t *c, uint8_t out[YSF_CP_CHAN_LEN])
{
	field_write(&out[0], c->dest, YSF_CALLSIGN_LEN);
	out[YSF_CALLSIGN_LEN] = c->tx_dgid <= YSF_CP_DGID_MAX ? c->tx_dgid : 0u;
	out[YSF_CALLSIGN_LEN + 1u] = c->rx_dgid <= YSF_CP_DGID_MAX ? c->rx_dgid : 0u;
	out[YSF_CP_CHAN_LEN - 1u] = (uint8_t)(c->flags & YSF_CP_FLAGS);
}

void ysf_station_decode(const uint8_t *b, int len, ysf_station_t *out)
{
	field_read(out->call, b, len, 0, YSF_CALLSIGN_LEN);
	field_read(out->radio_id, b, len, YSF_CALLSIGN_LEN, YSF_RADIO_ID_LEN);
}

void ysf_station_encode(const ysf_station_t *s, uint8_t out[YSF_CP_STATION_LEN])
{
	field_write(&out[0], s->call, YSF_CALLSIGN_LEN);
	field_write(&out[YSF_CALLSIGN_LEN], s->radio_id, YSF_RADIO_ID_LEN);
}

bool ysf_chan_load(uint16_t number, const channel_t *ch, ysf_chan_t *out)
{
	uint8_t b[YSF_CP_CHAN_LEN + CODEPLUG_TAG_HEAD_MAX];
	if (ch->kind != CODEPLUG_KIND_YSF) {
		return false;
	}
	int len = codeplug_tag_params(number, ch, b, YSF_CP_CHAN_LEN);
	if (len < 0) {
		return false;
	}
	ysf_chan_decode(b, len < (int)YSF_CP_CHAN_LEN ? len : (int)YSF_CP_CHAN_LEN, out);
	return true;
}

bool ysf_station_load(ysf_station_t *out)
{
	uint8_t b[YSF_CP_STATION_LEN];
	int len = codeplug_mode_settings(CODEPLUG_KIND_YSF, b, sizeof b);
	ysf_station_decode(b, len < 0 ? -1 : (len < (int)sizeof b ? len : (int)sizeof b), out);
	return len >= 0;
}

void ysf_field_set(char *dst, size_t len, const char *src)
{
	size_t i = 0;
	for (; src != NULL && i < len && src[i] != '\0'; i++) {
		dst[i] = call_char((uint8_t)src[i]);
	}
	for (; i < len; i++) {
		dst[i] = ' ';
	}
}

// The longest stored block kept on a rewrite: the most the generic layer writes (channel-modes.md 2.1)
#define KEEP_MAX	64u

bool ysf_chan_write(uint16_t number, const char *name, const ysf_chan_t *c, bool tx)
{
	uint8_t b[KEEP_MAX + CODEPLUG_TAG_HEAD_MAX];
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_YSF, .system = CODEPLUG_SYSTEM_NONE };
	uint16_t len = YSF_CP_CHAN_LEN;
	uint8_t keep_tag = 0u;		// tag flags kept: reserved bit 3, the kind's bits 4-7
	uint8_t keep_blk = 0u;		// block flags kept: the reserved bits 2-7

	// This channel's own tag, live under this name: keep what a later firmware or the editor put there. Any other tag
	// at the number is replaced whole. Base-own and fallback are dropped: the caller wrote the stand-in record
	codeplug_tag_t old;
	if (codeplug_tag_read(number, &old) && old.kind == CODEPLUG_KIND_YSF) {
		channel_t ch;
		memset(&ch, 0, sizeof ch);
		ch.kind = CODEPLUG_KIND_YSF;
		strncpy(ch.name, name, CODEPLUG_NAME_LEN);
		int n = codeplug_tag_params(number, &ch, b, KEEP_MAX);
		if (n >= 0) {
			keep_tag = (uint8_t)(old.flags & ~(CODEPLUG_TAG_TX | CODEPLUG_TAG_BASE_OWN | CODEPLUG_TAG_FALLBACK));
			if (n >= (int)YSF_CP_CHAN_LEN && b[YSF_CP_CHAN_LEN - 1u] != 0xFFu) {
				keep_blk = (uint8_t)(b[YSF_CP_CHAN_LEN - 1u] & ~YSF_CP_FLAGS);
			}
			if (n > (int)YSF_CP_CHAN_LEN) {
				len = (uint16_t)(n < (int)KEEP_MAX ? n : (int)KEEP_MAX);	// the tail stays in b
			}
		}
	}
	ysf_chan_encode(c, b);
	b[YSF_CP_CHAN_LEN - 1u] |= keep_blk;
	t.flags = (uint8_t)(keep_tag | (tx ? CODEPLUG_TAG_TX : 0u));
	return codeplug_tag_write(number, name, &t, b, len);
}

#endif
