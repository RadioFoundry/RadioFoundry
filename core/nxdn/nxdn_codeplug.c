// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN conventional channels in the codeplug (nxdn_codeplug.h, docs/spec/nxdn-codeplug.md)
#include <string.h>
#include "config.h"
#include "nxdn/nxdn_codeplug.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"

#if CONFIG_NXDN

void nxdn_conv_decode(const uint8_t *b, int len, nxdn_conv_t *out)
{
	uint8_t f = len >= 1 ? b[0] : 0u;
	uint8_t rx = len >= 2 ? b[1] : 0u;
	uint8_t tx = len >= 3 ? b[2] : 0u;

	out->squelch = (f & 0x03u) == NXDN_SQUELCH_SELECTIVE ? NXDN_SQUELCH_SELECTIVE : NXDN_SQUELCH_RAN;
	out->rate9600 = (f & NXDN_CP_4800) == 0;
	if (rx > NXDN_CP_RAN_MAX)
		rx = 0;			// every RAN
	out->rx_ran = rx;
	// A TX RAN field: its value when set, else the RX RAN (RAN 1 when that is every RAN); bit 6 set is no value
	if ((tx & 0xC0u) == NXDN_CP_TX_RAN_SET)
		out->tx_ran = (uint8_t)(tx & NXDN_CP_RAN_MAX);
	else
		out->tx_ran = rx != 0 ? rx : (uint8_t)NXDN_CP_TX_RAN_DEFAULT;
}

void nxdn_conv_encode(const nxdn_conv_t *c, uint8_t out[NXDN_CP_CONV_LEN])
{
	uint8_t rx = c->rx_ran <= NXDN_CP_RAN_MAX ? c->rx_ran : 0u;
	uint8_t tx_dflt = rx != 0 ? rx : (uint8_t)NXDN_CP_TX_RAN_DEFAULT;

	out[0] = (uint8_t)((c->squelch == NXDN_SQUELCH_SELECTIVE ? NXDN_SQUELCH_SELECTIVE : NXDN_SQUELCH_RAN) |
			   (c->rate9600 ? 0u : NXDN_CP_4800));
	out[1] = rx;
	out[2] = c->tx_ran == tx_dflt ? 0u : (uint8_t)(NXDN_CP_TX_RAN_SET | (c->tx_ran & NXDN_CP_RAN_MAX));
}

static bool unit_ok(uint32_t id)
{
	return id >= 1u && id <= NXDN_CP_UNIT_MAX;
}

uint16_t nxdn_unit_id_decode(const uint8_t *b, int len, uint32_t dmr_id)
{
	uint32_t id = len >= 2 ? (uint32_t)(b[0] | (b[1] << 8)) : 0u;

	if (unit_ok(id))
		return (uint16_t)id;
	return unit_ok(dmr_id) ? (uint16_t)dmr_id : 1u;
}

// Our unit ID: the channel's (the record's own ID), else the radio-wide NXDN one, else the DMR ID, else 1
static uint16_t unit_id(const channel_t *ch)
{
	uint8_t b[8];

	if (unit_ok(ch->dmr_id))
		return (uint16_t)ch->dmr_id;
	return nxdn_unit_id_decode(b, codeplug_mode_settings(CODEPLUG_KIND_NXDN, b, sizeof b), codeplug_dmr_id());
}

// The talk target from the record's contact (2): a group, a unit, all call; none or an ID out of range = group 1
static void talk_target(const channel_t *ch, uint16_t *target, bool *unit)
{
	contact_t c;
#if CONFIG_DMR
	uint16_t idx = codeplug_channel_talk_contact(ch);	// the contact, else the RX group list's first group
#else
	uint16_t idx = ch->contact;
#endif

	*target = NXDN_CP_GROUP_DEFAULT;
	*unit = false;
	if (idx == 0u || !codeplug_contact(idx, &c))
		return;
	if (c.type == CONTACT_ALL_CALL) {
		*target = NXDN_CP_ID_ALL;
	} else if (c.type == CONTACT_PRIVATE) {
		if (unit_ok(c.id)) {
			*target = (uint16_t)c.id;
			*unit = true;
		}
	} else if (c.id >= 1u && c.id <= 0xFFFFu) {
		*target = (uint16_t)c.id;
	}
}

bool nxdn_conv_load(uint16_t number, const channel_t *ch, nxdn_conv_cfg_t *out)
{
	uint8_t b[NXDN_CP_CONV_LEN + 8u + CODEPLUG_TAG_HEAD_MAX];
	const int cap = (int)(sizeof b - CODEPLUG_TAG_HEAD_MAX);
	int len;

	if (ch->kind != CODEPLUG_KIND_NXDN)
		return false;
	len = codeplug_tag_params(number, ch, b, (uint16_t)cap);
	if (len < 0)
		return false;
	nxdn_conv_decode(b, len < cap ? len : cap, &out->conv);
	out->unit_id = unit_id(ch);
	talk_target(ch, &out->target, &out->target_unit);
	return true;
}

bool nxdn_conv_save(uint16_t number, const channel_t *ch, const nxdn_conv_t *conv)
{
	channel_t c = *ch;
	uint8_t blk[NXDN_CP_CONV_LEN];
	// A channel of the kind already keeps the system it names
	uint8_t system = ch->kind == CODEPLUG_KIND_NXDN ? ch->system : CODEPLUG_SYSTEM_NONE;
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_NXDN, .system = system, .flags = ch->rx_only ? 0u : CODEPLUG_TAG_TX };

	// The safe stand-in: DMR, receive only (the tag carries transmit allowed). Under a tag that says the base record
	// is a channel of its own, codeplug_set_channel keeps that channel's mode instead
	c.kind = CODEPLUG_KIND_NXDN;
	c.system = system;
	c.mode = RADIO_MODE_DMR;
	c.rx_only = true;
	nxdn_conv_encode(conv, blk);
	// A tag of the kind that followed the record keeps its fallback, its other flags and what it stores after this
	// block (an editor's or a later firmware's); else a new one
	return codeplug_set_channel(number, &c) && (codeplug_tag_rewrite(number, c.name, &t, blk, sizeof blk) ||
						     codeplug_tag_write(number, c.name, &t, blk, sizeof blk));
}

#endif	// CONFIG_NXDN
