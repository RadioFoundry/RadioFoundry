// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 channels and systems in the codeplug (p25_codeplug.h, docs/spec/p25-codeplug.md)
#include <string.h>
#include "config.h"
#include "p25/p25_codeplug.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"

#if CONFIG_P25

static uint16_t rd16(const uint8_t *b, int len, int at)
{
	return at + 2 <= len ? (uint16_t)(b[at] | (b[at + 1] << 8)) : 0u;
}

static uint32_t rd32(const uint8_t *b, int len, int at)
{
	return at + 4 <= len ? (uint32_t)rd16(b, len, at) | ((uint32_t)rd16(b, len, at + 2) << 16) : 0u;
}

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// A NAC field: its value when set, else dflt. A value the field can't hold reads as dflt too
static uint16_t nac_field(uint16_t f, uint16_t dflt)
{
	return (f & P25_CP_NAC_SET) != 0 && (f & 0x7000u) == 0 ? (uint16_t)(f & 0x0FFFu) : dflt;
}

void p25_conv_decode(const uint8_t *b, int len, p25_conv_t *out)
{
	uint8_t sq = len >= 1 ? (uint8_t)(b[0] & 0x03u) : 0u;
	out->squelch = sq == P25_SQUELCH_SELECTIVE ? P25_SQUELCH_SELECTIVE : P25_SQUELCH_NORMAL;
	out->air_contacts = len >= 1 && (b[0] & P25_CONV_AIR_CONTACTS) != 0;
	uint16_t rx = nac_field(rd16(b, len, 1), P25_NAC_DEFAULT);
	if (rx == P25_NAC_REPEAT_ANY) {
		rx = P25_NAC_DEFAULT;		// a repeater's value, never a radio's
	}
	uint16_t tx_dflt = rx == P25_NAC_ANY ? P25_NAC_DEFAULT : rx;
	uint16_t tx = nac_field(rd16(b, len, 3), tx_dflt);
	if (tx == P25_NAC_ANY || tx == P25_NAC_REPEAT_ANY) {
		tx = tx_dflt;			// "any" is no value to send
	}
	out->rx_nac = rx;
	out->tx_nac = tx;
}

void p25_conv_encode(const p25_conv_t *c, uint8_t out[P25_CP_CONV_LEN])
{
	out[0] = (c->squelch == P25_SQUELCH_SELECTIVE ? P25_SQUELCH_SELECTIVE : P25_SQUELCH_NORMAL) |
		 (c->air_contacts ? P25_CONV_AIR_CONTACTS : 0u);
	uint16_t tx_dflt = c->rx_nac == P25_NAC_ANY ? P25_NAC_DEFAULT : c->rx_nac;
	wr16(&out[1], c->rx_nac == P25_NAC_DEFAULT ? 0u : (uint16_t)(P25_CP_NAC_SET | (c->rx_nac & 0x0FFFu)));
	wr16(&out[3], c->tx_nac == tx_dflt ? 0u : (uint16_t)(P25_CP_NAC_SET | (c->tx_nac & 0x0FFFu)));
}

uint32_t p25_unit_id_decode(const uint8_t *b, int len, uint32_t dmr_id)
{
	uint32_t id = len >= 4 ? rd32(b, len, 0) : 0u;
	return id >= 1u && id <= P25_CP_UNIT_MAX ? id : dmr_id;
}

uint32_t p25_radio_unit_id(uint32_t dmr_id)
{
	uint8_t b[8];
	return p25_unit_id_decode(b, codeplug_mode_settings(CODEPLUG_KIND_P25, b, sizeof b), dmr_id);
}

// Our unit ID: the channel's (the record's own ID), else own (a system's, 0 = none), else the radio-wide P25 one, else the
// DMR ID
static uint32_t unit_id(const channel_t *ch, uint32_t own)
{
	if (ch->dmr_id != 0u) {
		return ch->dmr_id;
	}
	if (own >= 1u && own <= P25_CP_UNIT_MAX) {
		return own;
	}
	return p25_radio_unit_id(codeplug_dmr_id());
}

// The talk target from the record's contact (2): a group, a unit, all call; none = talk group 1. air: the contact is a
// P25 contact slot (else a DMR contact: the one field means two things, so every reader goes through here). rx_list: with
// no contact, the first group of the RX group list, as for a DMR channel
static void talk_target(const channel_t *ch, bool air, bool rx_list, p25_talk_t *t)
{
	contact_t c;
	t->id = P25_CP_TG_DEFAULT;
	t->unit = false;
	t->set = false;
	t->air = air;
	t->scoped = false;
	t->scoped_rx = false;
	t->name[0] = '\0';
	if (air) {
		// A slot of the air mode contact table (air-contacts.md): a group, a unit, all call. One that is empty, invalid
		// or of another mode is no target
		air_contact_t a;
		bool ok = false;
		bool invalid_scope = false;
		uint8_t system = ch->kind == CODEPLUG_KIND_P25_TRUNK ? ch->system : CODEPLUG_SYSTEM_NONE;
		if (ch->contact != 0u) {
			t->id = 0;	// an explicit dangling slot cannot silently select the legacy default
			ok = codeplug_air_contact(ch->contact, CODEPLUG_KIND_P25, system, &a);
			invalid_scope = !ok && a.scoped;
		} else if (rx_list && ch->listen_list != 0u) {
			listen_list_t list;	// on the stack: the radio and UI tasks both call this
			if (codeplug_listen_list(ch->listen_list, &list)) {
				if (list.count != 0 || list.malformed)
					t->id = 0;	// an explicit list with no usable matching group is not an unspecified target
				for (uint8_t i = 0; i < list.count && !ok; i++) {
					ok = codeplug_air_contact(list.contacts[i], CODEPLUG_KIND_P25, system, &a) && a.type == AIR_CONTACT_GROUP;
					invalid_scope |= !ok && a.scoped;
				}
			} else t->id = 0;
		}
		if (ok) {
			t->unit = a.type == AIR_CONTACT_UNIT;
			t->id = a.type == AIR_CONTACT_ALL ? P25_CP_TG_ALL : a.id;
			t->set = true;
			t->scoped = a.scoped;
			memcpy(t->name, a.name, sizeof t->name);
		} else if (invalid_scope) {
			t->id = 0;
			t->scoped = true;
		}
		return;
	}
#if CONFIG_DMR
	uint16_t idx = rx_list ? codeplug_channel_talk_contact(ch) : ch->contact;	// the contact, else the RX list's first group
#else
	uint16_t idx = ch->contact;
#endif
	if (idx == 0u || !codeplug_contact(idx, &c)) {
		return;
	}
	if (c.type == CONTACT_ALL_CALL) {
		t->id = P25_CP_TG_ALL;
	} else if (c.type == CONTACT_PRIVATE) {
		t->id = c.id;
		t->unit = true;
	} else if (c.id >= 1u && c.id <= 0xFFFFu) {
		t->id = c.id;
	} else {
		return;
	}
	t->set = true;
	memcpy(t->name, c.name, sizeof t->name);
}

bool p25_conv_params(uint16_t number, const channel_t *ch, p25_conv_t *out)
{
	uint8_t b[P25_CP_CONV_LEN + 8u + CODEPLUG_TAG_HEAD_MAX];
	const int cap = (int)(sizeof b - CODEPLUG_TAG_HEAD_MAX);
	if (ch->kind != CODEPLUG_KIND_P25) {
		return false;
	}
	int len = codeplug_tag_params(number, ch, b, (uint16_t)cap);
	if (len < 0) {
		return false;
	}
	p25_conv_decode(b, len < cap ? len : cap, out);
	return true;
}

// A separate frame keeps this list off talk_target's nested reads on the UI's zone-export path.
static bool __attribute__((noinline)) scoped_rx(const channel_t *ch)
{
	listen_list_t list;
	air_contact_t a;
	if (codeplug_listen_list(ch->listen_list, &list)) {
		for (uint8_t i = 0; i < list.count; i++) {
			codeplug_air_contact(list.contacts[i], CODEPLUG_KIND_P25,
				ch->kind == CODEPLUG_KIND_P25_TRUNK ? ch->system : CODEPLUG_SYSTEM_NONE, &a);
			if (a.scoped)
				return true;
		}
	}
	return false;
}

bool p25_channel_talk(uint16_t number, const channel_t *ch, bool rx_list, p25_talk_t *out)
{
	bool air = false;
	if (ch->kind == CODEPLUG_KIND_P25) {
		p25_conv_t conv;
		if (!p25_conv_params(number, ch, &conv)) {
			return false;
		}
		air = conv.air_contacts;
#if CONFIG_P25_TRUNK
	} else if (ch->kind == CODEPLUG_KIND_P25_TRUNK) {
		uint8_t p[1 + CODEPLUG_TAG_HEAD_MAX];
		air = codeplug_tag_params(number, ch, p, 1) >= 1 && (p[0] & P25_TCH_AIR_CONTACTS) != 0;
#endif
	} else {
		return false;
	}
	talk_target(ch, air, rx_list, out);
	if (air && rx_list && ch->listen_list != 0u)
		out->scoped_rx = scoped_rx(ch);
	return true;
}

bool p25_conv_load(uint16_t number, const channel_t *ch, p25_conv_cfg_t *out)
{
	if (!p25_conv_params(number, ch, &out->conv)) {
		return false;
	}
	out->unit_id = unit_id(ch, 0u);
	p25_talk_t t;
	talk_target(ch, out->conv.air_contacts, true, &t);
	out->target = t.id;
	out->target_unit = t.unit;
	// Talkaround (2): the record's direct mode turns it on; clear, it is on where RX and TX are one frequency
	out->talkaround = ch->rx_hz == ch->tx_hz || ch->dmo;
	return true;
}

bool p25_conv_save(uint16_t number, const channel_t *ch, const p25_conv_t *conv)
{
	channel_t c = *ch;
	uint8_t blk[P25_CP_CONV_LEN];
	// A channel of the kind already keeps the system it names
	uint8_t system = ch->kind == CODEPLUG_KIND_P25 ? ch->system : CODEPLUG_SYSTEM_NONE;
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_P25, .system = system, .flags = ch->rx_only ? 0u : CODEPLUG_TAG_TX };
	// The safe stand-in: DMR, receive only (the tag carries transmit allowed). Under a tag that says the base record
	// is a channel of its own, codeplug_set_channel keeps that channel's mode instead
	c.kind = CODEPLUG_KIND_P25;
	c.system = system;
	c.mode = RADIO_MODE_DMR;
	c.rx_only = true;
	p25_conv_encode(conv, blk);
	// A tag of the kind that followed the record keeps its fallback, its other flags and what it stores after this
	// block (an editor's or a later firmware's); else a new one
	return codeplug_set_channel(number, &c) && (codeplug_tag_rewrite(number, c.name, &t, blk, sizeof blk) ||
						     codeplug_tag_write(number, c.name, &t, blk, sizeof blk));
}

#if CONFIG_P25_TRUNK

static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }

void p25_tsys_decode(const uint8_t *b, int len, p25_tsys_t *out)
{
	uint32_t w = len >= 3 ? (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) : 0u;
	out->wacn = w == P25_CP_WACN_ANY ? w : (w & 0xFFFFFu);
	uint16_t s = rd16(b, len, 3);
	out->sysid = s == P25_CP_SYSID_ANY ? s : (uint16_t)(s & 0x0FFFu);
	out->cc_nac = nac_field(rd16(b, len, 5), P25_NAC_ANY);
	out->options = len >= 8 ? (uint8_t)(b[7] & (P25_TSYS_SKIP_ENC | P25_TSYS_PLAN_ONLY | P25_TSYS_NO_REG)) : 0u;
	out->hold_s = len >= 9 ? b[8] : 0u;
	out->roam_dbm = len >= 10 ? (int8_t)b[9] : 0;
}

void p25_tsys_encode(const p25_tsys_t *s, uint8_t out[P25_CP_TSYS_LEN])
{
	out[0] = (uint8_t)s->wacn;
	out[1] = (uint8_t)(s->wacn >> 8);
	out[2] = (uint8_t)(s->wacn >> 16);
	wr16(&out[3], s->sysid);
	wr16(&out[5], s->cc_nac == P25_NAC_ANY ? 0u : (uint16_t)(P25_CP_NAC_SET | (s->cc_nac & 0x0FFFu)));
	out[7] = s->options;
	out[8] = s->hold_s;
	out[9] = (uint8_t)s->roam_dbm;
}

void p25_site_decode(const uint8_t *b, int len, p25_site_t *out)
{
	out->rfss = len >= 1 ? b[0] : P25_CP_UNKNOWN;
	out->site = len >= 2 ? b[1] : P25_CP_UNKNOWN;
	uint16_t n = rd16(b, len, 2);
	out->nac_set = len >= 4 && (n == 0 || ((n & 0xF000u) == P25_CP_NAC_SET));
	out->nac = out->nac_set ? nac_field(n, P25_NAC_ANY) : P25_NAC_ANY;
	if (out->nac == P25_NAC_REPEAT_ANY) {
		out->nac_set = false;
		out->nac = P25_NAC_ANY;
	}
}

void p25_ctl_decode(const uint8_t *b, int len, p25_ctl_t *out)
{
	out->chan = rd16(b, len, 0);
	out->rx_hz = rd32(b, len, 2);
	out->tx_hz = rd32(b, len, 6);
}

void p25_iden_decode(const uint8_t *b, int len, p25_iden_t *out)
{
	out->base_hz = rd32(b, len, 0);
	out->spacing_hz = rd32(b, len, 4);
	out->tx_offset_hz = (int32_t)rd32(b, len, 8);
	out->bw_hz = rd32(b, len, 12);
	out->type = len >= 17 && b[16] == P25_IDEN_TDMA ? P25_IDEN_TDMA : P25_IDEN_FDMA;
	out->slots = out->type == P25_IDEN_TDMA ? (len >= 18 && b[17] != 0u ? b[17] : 2u) : 1u;
}

void p25_iden_encode(const p25_iden_t *i, uint8_t out[P25_CP_IDEN_LEN])
{
	wr32(&out[0], i->base_hz);
	wr32(&out[4], i->spacing_hz);
	wr32(&out[8], (uint32_t)i->tx_offset_hz);
	wr32(&out[12], i->bw_hz);
	out[16] = i->type;
	out[17] = i->type == P25_IDEN_TDMA ? i->slots : 0u;
}

bool p25_trunk_load(uint16_t number, const channel_t *ch, p25_trunk_cfg_t *out)
{
	uint8_t b[24 + CODEPLUG_SYS_HDR];
	codeplug_system_t sy;
	codeplug_tag_t t;
	if (ch->kind != CODEPLUG_KIND_P25_TRUNK || !codeplug_tag_read(number, &t) || t.kind != ch->kind) {
		return false;
	}
	int len = codeplug_system(ch->system, &sy, b, 24);
	if (len < 0 || sy.type != CODEPLUG_SYSTYPE_P25_TRUNK) {
		return false;
	}
	memset(out, 0, sizeof *out);
	p25_tsys_decode(b, len < 24 ? len : 24, &out->sys);
	out->listen_only = (sy.flags & CODEPLUG_SYS_TX) == 0;
	out->unit_id = unit_id(ch, sy.own_id);

	codeplug_sys_entry_t e;
	int at = -1;
	while (out->ctl_count < P25_CP_CTL_MAX &&
	       codeplug_sys_entry_next(&at, ch->system, CODEPLUG_ENTRY_CHANNEL, &e, b, 24) >= 0) {
		if ((e.flags & CODEPLUG_ENTRY_CONTROL) != 0) {
			out->ctl_site[out->ctl_count] = e.group;
			p25_ctl_decode(b, e.len, &out->ctl[out->ctl_count++]);
		}
	}
	at = -1;
	for (uint8_t i = 0; i < out->ctl_count; i++)
		p25_site_decode(NULL, 0, &out->ctl_identity[i]);
	while (codeplug_sys_entry_next(&at, ch->system, CODEPLUG_ENTRY_SITE, &e, b, 24) >= 0) {
		for (uint8_t i = 0; i < out->ctl_count; i++)
			if (out->ctl_site[i] == e.group)
				p25_site_decode(b, e.len < 24 ? e.len : 24, &out->ctl_identity[i]);
	}
	at = -1;
	while (codeplug_sys_entry_next(&at, ch->system, CODEPLUG_ENTRY_PLAN, &e, b, 24) >= 0) {
		if (e.group < P25_CP_IDEN_MAX && (out->iden_set & (1u << e.group)) == 0) {
			out->iden_set |= (uint16_t)(1u << e.group);
			p25_iden_decode(b, e.len, &out->iden[e.group]);
		}
	}

	uint8_t p[1 + CODEPLUG_TAG_HEAD_MAX];
	int n = codeplug_tag_params(number, ch, p, 1);
	out->follow_all = n >= 1 && (p[0] & P25_TCH_FOLLOW_ALL) != 0;
	bool air = n >= 1 && (p[0] & P25_TCH_AIR_CONTACTS) != 0;
	if (air && ch->listen_list != 0u) {
		// The RX list's group slots are the groups followed too; an empty, invalid or not group one is skipped
		listen_list_t list;
		air_contact_t a;
		if (codeplug_listen_list(ch->listen_list, &list)) {
			for (uint8_t i = 0; i < list.count && out->group_count < CODEPLUG_LIST_LEN; i++) {
				if (codeplug_air_contact(list.contacts[i], CODEPLUG_KIND_P25, ch->system, &a) && a.type == AIR_CONTACT_GROUP) {
					out->groups[out->group_count++] = (uint16_t)a.id;
				}
			}
		}
	}
	// The existing target reader owns fallback selection too, including invalid/nonempty RX lists.
	p25_talk_t tk;
	talk_target(ch, air, true, &tk);
	out->talk_group = tk.unit ? (tk.air ? 0u : (uint16_t)P25_CP_TG_DEFAULT) : (uint16_t)tk.id;
	if ((t.flags & P25_TAG_NO_FALLBACK) == 0) {
		bool own = (t.flags & CODEPLUG_TAG_FALLBACK) != 0;
		uint16_t fb = own ? t.fb_chan : sy.fb_chan;
		uint16_t check = own ? t.fb_check : sy.fb_check;
		// the fallback is a plain channel of that name still
		channel_t f;
		if (fb != 0u && codeplug_channel(fb, &f) && f.kind == CODEPLUG_KIND_NONE &&
		    codeplug_name_check(f.name) == check) {
			out->fallback = fb;
		}
	}
	return true;
}
#endif
#endif
