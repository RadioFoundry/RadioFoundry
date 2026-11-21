// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR channels in the codeplug (dpmr_codeplug.h, docs/spec/dpmr.md 6)
#include <string.h>
#include "config.h"
#include "dpmr/dpmr_codeplug.h"
#include "dpmr/dpmr.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"

#if CONFIG_DPMR

// Bytes past a short block read 0, and multi-byte fields are little endian, as every record of the codeplug's
// channel-mode layer (channel-modes.md 2)
static uint8_t byte_at(const uint8_t *b, int len, int at)
{
	return b != NULL && at < len ? b[at] : 0u;
}

static uint32_t id_at(const uint8_t *b, int len, int at)
{
	return (uint32_t)byte_at(b, len, at) | (uint32_t)byte_at(b, len, at + 1) << 8 |
	       (uint32_t)byte_at(b, len, at + 2) << 16;
}

void dpmr_chan_decode(const uint8_t *b, int len, dpmr_chan_t *out)
{
	uint8_t cc = byte_at(b, len, 3), f = byte_at(b, len, 4);

	out->called = id_at(b, len, 0);
	if (out->called == 0)
		out->called = DPMR_ID_ALL;	// common ID 0 is reserved (8.1.1): a zeroed block calls all
	out->cc = cc <= DPMR_CP_CC_MAX ? cc : (uint8_t)DPMR_CP_CC_AUTO;
	// Erased: none. Bits this firmware doesn't know ride along to the next save
	out->flags = f == 0xFFu ? 0u : (uint8_t)(f & DPMR_CP_FLAGS_KEPT);
}

void dpmr_chan_encode(const dpmr_chan_t *c, uint8_t out[DPMR_CP_CHAN_LEN])
{
	uint32_t id = c->called & 0xFFFFFFu;

	out[0] = (uint8_t)id;
	out[1] = (uint8_t)(id >> 8);
	out[2] = (uint8_t)(id >> 16);
	out[3] = c->cc <= DPMR_CP_CC_MAX ? c->cc : (uint8_t)DPMR_CP_CC_AUTO;
	out[4] = (uint8_t)(c->flags & DPMR_CP_FLAGS_KEPT);
}

uint32_t dpmr_own_decode(const uint8_t *b, int len)
{
	uint32_t id = len >= (int)DPMR_CP_STATION_LEN ? id_at(b, len, 0) : 0u;

	return id == 0 || id == 0xFFFFFFu ? DPMR_CP_ID_NONE : id;
}

uint16_t dpmr_chan_cc(const dpmr_chan_t *c, uint32_t tx_hz)
{
	if (c->cc == DPMR_CP_CC_AUTO || c->cc > DPMR_CP_CC_MAX)
		return dpmr_cc_for_hz(tx_hz);
	return dpmr_cc_group(c->cc > 16u, (uint8_t)((c->cc - 1u) & 15u));
}

bool dpmr_chan_load(uint16_t number, const channel_t *ch, dpmr_chan_t *out)
{
	uint8_t b[DPMR_CP_CHAN_LEN + CODEPLUG_TAG_HEAD_MAX];
	int len;

	if (ch->kind != CODEPLUG_KIND_DPMR)
		return false;
	len = codeplug_tag_params(number, ch, b, DPMR_CP_CHAN_LEN);
	if (len < 0)
		return false;
	dpmr_chan_decode(b, len < (int)DPMR_CP_CHAN_LEN ? len : (int)DPMR_CP_CHAN_LEN, out);
	return true;
}

uint32_t dpmr_own_load(void)
{
	uint8_t b[DPMR_CP_STATION_LEN];
	int len = codeplug_mode_settings(CODEPLUG_KIND_DPMR, b, sizeof b);

	return dpmr_own_decode(b, len < (int)sizeof b ? len : (int)sizeof b);
}

bool dpmr_chan_save(uint16_t number, const channel_t *ch, const dpmr_chan_t *c)
{
	channel_t r = *ch;
	uint8_t blk[DPMR_CP_CHAN_LEN];
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_DPMR, .system = CODEPLUG_SYSTEM_NONE,
			     .flags = ch->rx_only ? 0u : CODEPLUG_TAG_TX };

	// The safe stand-in: DMR, receive only (the tag carries transmit allowed), as NXDN's (nxdn_conv_save). Under a tag
	// that says the base record is a channel of its own, codeplug_set_channel keeps that channel's mode instead
	r.kind = CODEPLUG_KIND_DPMR;
	r.system = CODEPLUG_SYSTEM_NONE;
	r.mode = RADIO_MODE_DMR;
	r.rx_only = true;
	dpmr_chan_encode(c, blk);
	return codeplug_set_channel(number, &r) && (codeplug_tag_rewrite(number, r.name, &t, blk, sizeof blk) ||
						     codeplug_tag_write(number, r.name, &t, blk, sizeof blk));
}

#endif	// CONFIG_DPMR
