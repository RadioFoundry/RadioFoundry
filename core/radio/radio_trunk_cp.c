// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tier III systems and trunked channels from the codeplug (radio_trunk.h, docs/spec/dmr-trunking.md 12)
#include <string.h>
#include "radio/radio_trunk.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "radio/radio_trunk_rec.h"

#if CONFIG_DMR_TRUNK
// The options a codeplug may set: the rest are the debug link's (monitor, learning the system code)
#define CP_OPTS		(T3_OPT_LISTEN_ONLY | T3_OPT_ALLCALL | T3_OPT_EMERGENCY_ANY | T3_OPT_PRIVATE | T3_OPT_REG_TG | \
			 T3_OPT_ALS | T3_OPT_KILL | T3_OPT_NO_ROAM | T3_OPT_STUN)


uint16_t radio_t3_name_check(const char *name)
{
	uint32_t h = 2166136261u;
	bool end = false;
	for (int i = 0; i < CODEPLUG_NAME_LEN; i++) {
		uint8_t c = end ? 0u : (uint8_t)name[i];
		end = end || c == 0u;
		h = (h ^ c) * 16777619u;
	}
	return (uint16_t)(h ^ (h >> 16));
}

bool radio_t3_tag(uint16_t c, const char *name, radio_t3_tag_t *tag)
{
	uint8_t v[TAG_LEN];
	if (c == 0 || c > CODEPLUG_CHANNELS_MAX ||
	    codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_TAG + c - 1u), v, sizeof v) < (int)TAG_LEN ||
	    v[0] >= RADIO_T3_SYSTEMS || rd16(&v[2]) != radio_t3_name_check(name)) {
		return false;
	}
	tag->sys = v[0];
	tag->flags = v[1];
	tag->fb_chan = rd16(&v[4]);
	tag->fb_check = rd16(&v[6]);
	return true;
}

bool radio_t3_name(uint8_t n, char *name)
{
	uint8_t v[CODEPLUG_NAME_LEN];
	name[0] = '\0';
	if (n >= RADIO_T3_SYSTEMS || codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + n), v, sizeof v) < (int)SYS_LEN) {
		return false;
	}
	memcpy(name, v, CODEPLUG_NAME_LEN);
	name[CODEPLUG_NAME_LEN] = '\0';
	return true;
}

uint16_t radio_t3_next_chan(uint8_t n, uint16_t after, uint8_t *rec)
{
	codeplug_kv_cursor_t c;
	uint8_t r[CHAN_LEN];
	uint16_t key, best = 0;
	int len;
	codeplug_kv_begin(&c, (uint16_t)(after + 1u), CODEPLUG_KEY_T3_CHAN + 0xFFu);
	while ((len = codeplug_kv_next(&c, &key, r, sizeof r)) >= 0) {
		if (len >= (int)CHAN_LEN && r[0] == n && (best == 0 || key < best)) {
			best = key;
			memcpy(rec, r, CHAN_LEN);
		}
	}
	return best;
}

// The key of system n into cfg: false when it has none
static bool load_key(uint8_t n, t3_cfg_t *c)
{
	uint8_t v[KEY_LEN + 3u];
	int len = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_KEY + n), v, sizeof v);
	if (len < (int)KEY_LEN) {
		return false;
	}
	memcpy(c->auth_key, v, KEY_LEN);
	c->auth_key_set = true;
	c->psn = len >= (int)KEY_LEN + 3 ? ((uint32_t)v[16] << 16) | ((uint32_t)v[17] << 8) | v[18] : 0u;
	return true;
}

bool radio_t3_load(uint8_t n, radio_t3_setup_t *s)
{
	uint8_t v[SYS_LEN];
	radio_t3_reset(s);
	if (n >= RADIO_T3_SYSTEMS || codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + n), v, sizeof v) < (int)SYS_LEN) {
		return false;
	}
	t3_cfg_t *c = &s->cfg;
	s->sys = n;
	c->auth.model = v[16] & 0x03u;
	c->auth.net = rd16(&v[17]);
	c->auth.category = v[19] == T3_PAR_B ? T3_PAR_B : T3_PAR_A;
	s->cc = v[20] & 0x0Fu;
	uint16_t opts = rd16(&v[21]);
	c->opts = opts & CP_OPTS;
	c->own_id = rd32(&v[23]) <= CODEPLUG_DMR_ID_MAX ? rd32(&v[23]) : 0;
	s->fb_hold_s = v[28];
	s->fb_chan = rd16(&v[29]);
	s->fb_check = rd16(&v[62]);
	c->plan.fixed.base_hz = rd32(&v[31]);
	c->plan.fixed.first = rd16(&v[35]);
	c->plan.fixed.sep = v[37];
	c->plan.fixed.tx_low = (v[38] & PLAN_TX_LOW) != 0;
	c->plan.learn = (v[38] & PLAN_NO_LEARN) == 0;
	c->plan.fixed.split_hz = rd32(&v[39]);
	c->leave_dbm = (int8_t)v[43];
	c->ps_rq = v[44] <= 7u ? v[44] : 0;
	for (uint8_t i = 0; i < v[45] && i < T3_SITES_MAX; i++) {
		c->sites[c->auth.nsites++] = rd16(&v[46 + 2 * i]);
	}
	bool keyed = load_key(n, c);

	// Channels in key order: the plan's table, and the control channels among them
	uint8_t r[CHAN_LEN];
	for (uint16_t key = radio_t3_next_chan(n, CODEPLUG_KEY_T3_CHAN - 1u, r); key != 0 && s->nchans < RADIO_T3_CHANS;
	     key = radio_t3_next_chan(n, key, r)) {
		t3_chan_t ch = { .lcn = rd16(&r[2]), .rx_hz = rd32(&r[4]), .tx_hz = rd32(&r[8]), .cc = r[12] };
		s->chans[s->nchans++] = ch;
		if ((r[1] & CHAN_CTRL) != 0 && c->nctrl < T3_CTRL_MAX) {
			c->ctrl[c->nctrl++] = ch;
		}
	}
	radio_t3_fix(s);
	return c->nctrl > 0 && (keyed || (opts & SYS_AUTHENTICATES) == 0);
}

bool radio_t3_usable(uint8_t n)
{
	// The checks of radio_t3_load without its buffer: the record, a control channel, the key if one is needed
	uint8_t v[SYS_LEN], r[CHAN_LEN], k[KEY_LEN];
	if (n >= RADIO_T3_SYSTEMS || codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + n), v, sizeof v) < (int)SYS_LEN) {
		return false;
	}
	if ((rd16(&v[21]) & SYS_AUTHENTICATES) != 0 &&
	    codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_KEY + n), k, sizeof k) < (int)KEY_LEN) {
		return false;
	}
	codeplug_kv_cursor_t c;
	uint16_t key;
	int len;
	codeplug_kv_begin(&c, CODEPLUG_KEY_T3_CHAN, CODEPLUG_KEY_T3_CHAN + 0xFFu);
	while ((len = codeplug_kv_next(&c, &key, r, sizeof r)) >= 0) {
		if (len >= (int)CHAN_LEN && r[0] == n && (r[1] & CHAN_CTRL) != 0) {
			return true;
		}
	}
	return false;
}

static t3_tg_t *tg_add(t3_cfg_t *c, uint32_t id, uint8_t flags)
{
	for (uint8_t i = 0; i < c->ntg; i++) {
		if (c->tg[i].id == id) {
			c->tg[i].flags |= flags;
			return &c->tg[i];
		}
	}
	if (c->ntg >= T3_TG_MAX) {
		return NULL;
	}
	c->tg[c->ntg].id = id;
	c->tg[c->ntg].flags = flags;
	return &c->tg[c->ntg++];
}

void radio_t3_talk(const channel_t *ch, radio_t3_setup_t *s)
{
	t3_cfg_t *c = &s->cfg;
	c->ntg = 0;
	s->talk_id = 0;
	s->talk_private = false;
	// The RX group list's groups are followed (an all-call contact there follows all-calls); the contact is what PTT
	// calls, else the first group
	listen_list_t l;
	contact_t ct;
	if (ch->listen_list != 0 && codeplug_listen_list(ch->listen_list, &l)) {
		for (uint8_t i = 0; i < l.count && i < CODEPLUG_LIST_LEN; i++) {
			if (!codeplug_contact(l.contacts[i], &ct)) {
				continue;
			}
			if (ct.type == CONTACT_GROUP) {
				(void)tg_add(c, ct.id, T3_TG_LISTEN);
			} else if (ct.type == CONTACT_ALL_CALL) {
				c->opts |= T3_OPT_ALLCALL;
			}
		}
	}
	if (ch->contact != 0 && codeplug_contact(ch->contact, &ct)) {
		if (ct.type == CONTACT_GROUP) {
			(void)tg_add(c, ct.id, T3_TG_TALK | T3_TG_LISTEN);
		} else {
			s->talk_id = ct.id;	// an individual call, or the all-call ID
			s->talk_private = ct.type == CONTACT_PRIVATE;
		}
	} else if (c->ntg > 0) {
		c->tg[0].flags |= T3_TG_TALK;
	}
}

uint16_t radio_t3_fallback(const radio_t3_tag_t *tag, const radio_t3_setup_t *s, channel_t *out)
{
	if ((tag->flags & RADIO_T3_TAG_NO_FALLBACK) != 0) {
		return 0;
	}
	uint16_t n = tag->fb_chan != 0 ? tag->fb_chan : s->fb_chan;
	uint16_t check = tag->fb_chan != 0 ? tag->fb_check : s->fb_check;
	radio_t3_tag_t t;
	if (n == 0 || !codeplug_channel(n, out) || radio_t3_name_check(out->name) != check ||
	    radio_t3_tag(n, out->name, &t)) {
		return 0;	// none, gone, renamed (renumbered), or itself trunked
	}
	return n;
}

#endif
