// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// System Fusion channels in the codeplug (ysf_codeplug.h, docs/spec/ysf-codeplug.md)
#include <string.h>
#include "config.h"
#include "ysf/ysf_codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"

#if CONFIG_YSF

void ysf_chan_decode(const uint8_t *b, int len, ysf_chan_t *out)
{
	uint8_t tx = len >= 1 ? b[0] : 0u, rx = len >= 2 ? b[1] : 0u;

	out->tx_dgid = tx <= YSF_CP_DGID_MAX ? tx : 0u;
	out->rx_dgid = rx <= YSF_CP_DGID_MAX ? rx : 0u;
}

void ysf_chan_encode(const ysf_chan_t *c, uint8_t out[YSF_CP_CHAN_LEN])
{
	out[0] = c->tx_dgid <= YSF_CP_DGID_MAX ? c->tx_dgid : 0u;
	out[1] = c->rx_dgid <= YSF_CP_DGID_MAX ? c->rx_dgid : 0u;
}

// A stored text field: up to the first NUL, trailing spaces dropped; "" when any character is outside `ok`
static void text_get(char *out, const uint8_t *b, uint8_t n, bool (*ok)(uint8_t c))
{
	uint8_t len = 0;

	while (len < n && b[len] != 0)
		len++;
	while (len > 0 && b[len - 1] == ' ')
		len--;
	for (uint8_t i = 0; i < len; i++) {
		if (!ok(b[i])) {
			out[0] = '\0';
			return;
		}
		out[i] = (char)b[i];
	}
	out[len] = '\0';
}

static bool call_char(uint8_t c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '-' || c == '/';
}

void ysf_radio_decode(const uint8_t *b, int len, ysf_radio_t *out)
{
	uint8_t rec[YSF_CP_CALL_LEN] = { 0 };

	if (len > 0)
		memcpy(rec, b, (size_t)len < sizeof rec ? (size_t)len : sizeof rec);
	text_get(out->callsign, rec, YSF_CP_CALL_LEN, call_char);
}

void ysf_radio_encode(const ysf_radio_t *r, uint8_t out[YSF_CP_CALL_LEN])
{
	memset(out, 0, YSF_CP_CALL_LEN);
	memcpy(out, r->callsign, strnlen(r->callsign, YSF_CP_CALL_LEN));
}

void ysf_radio_load(ysf_radio_t *out)
{
	uint8_t b[YSF_CP_CALL_LEN + 8];

	ysf_radio_decode(b, codeplug_mode_settings(CODEPLUG_KIND_YSF, b, sizeof b), out);
}

void ysf_callsign_from_name(const char *name, char out[YSF_CP_CALL_LEN + 1])
{
	uint8_t n = 0, letters = 0, digits = 0;

	out[0] = '\0';
	if (name == NULL)
		return;
	while (name[n] != '\0' && name[n] != ' ' && name[n] != '-' && name[n] != '/') {
		char c = name[n];

		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		if (n >= YSF_CP_CALL_LEN || !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
			out[0] = '\0';	// too long, or not a callsign's character: no callsign
			return;
		}
		letters += c >= 'A';
		digits += c <= '9';
		out[n++] = c;
	}
	out[n] = '\0';
	if (n < 3 || !letters || !digits)
		out[0] = '\0';
}

bool ysf_chan_load(uint16_t number, const channel_t *ch, ysf_chan_t *out)
{
	uint8_t b[YSF_CP_CHAN_LEN + 8u + CODEPLUG_TAG_HEAD_MAX];
	const int cap = (int)(sizeof b - CODEPLUG_TAG_HEAD_MAX);
	int len;

	if (ch->kind != CODEPLUG_KIND_YSF)
		return false;
	len = codeplug_tag_params(number, ch, b, (uint16_t)cap);
	if (len < 0)
		return false;
	ysf_chan_decode(b, len < cap ? len : cap, out);
	return true;
}

bool ysf_chan_save(uint16_t number, const channel_t *ch, const ysf_chan_t *yc)
{
	channel_t c = *ch;
	uint8_t blk[YSF_CP_CHAN_LEN];
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_YSF, .system = CODEPLUG_SYSTEM_NONE,
			     .flags = ch->rx_only ? 0u : CODEPLUG_TAG_TX };

	// The safe stand-in: DMR, receive only (the tag carries transmit allowed)
	c.kind = CODEPLUG_KIND_YSF;
	c.system = CODEPLUG_SYSTEM_NONE;
	c.mode = RADIO_MODE_DMR;
	c.rx_only = true;
	ysf_chan_encode(yc, blk);
	return codeplug_set_channel(number, &c) && (codeplug_tag_rewrite(number, c.name, &t, blk, sizeof blk) ||
						     codeplug_tag_write(number, c.name, &t, blk, sizeof blk));
}

#endif	// CONFIG_YSF
