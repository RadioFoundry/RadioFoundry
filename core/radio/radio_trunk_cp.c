// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tier III systems and trunked channels from the codeplug (radio_trunk.h, docs/spec/dmr-trunking-codeplug.md): system
// type 1 and channel kind 1 of the generic layer (codeplug_records.h, codeplug_tag.h)
#include <string.h>
#include "radio/radio_trunk.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_records.h"
#include "codeplug/codeplug_tag.h"

#if CONFIG_DMR_TRUNK
_Static_assert(RADIO_T3_SYSTEMS == CODEPLUG_SYSTEMS_RUN, "an imported system lands where the radio runs it");

// The options a codeplug may set: the rest are the debug link's (monitor, learning the system code). Listen only is
// the system header's transmit flag, never an option bit
#define CP_OPTS		(T3_OPT_ALLCALL | T3_OPT_EMERGENCY_ANY | T3_OPT_PRIVATE | T3_OPT_REG_TG | T3_OPT_ALS | T3_OPT_KILL | \
			 T3_OPT_NO_ROAM | T3_OPT_STUN)
#define OPT_IP_GW	0x1000u		// off-net IP through the system's IP gateway (the radio core's, not the engine's)

// The type's block (1): the fields up to the site count, then the sites
#define BLOCK_MIN	23u
#define BLOCK_LEN	(BLOCK_MIN + 2u * T3_SITES_MAX)
#define PLAN_TX_LOW	0x01u
#define PLAN_NO_LEARN	0x02u
#define CHAN_BODY	11u		// a system channel's body (2): LCN, MS receive Hz, MS transmit Hz, color code
#define KEY_LEN		16u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

// System n's header, and its block into v (at most cap octets; v holds CODEPLUG_SYS_HDR more): the block's stored
// length, -1 when n is past the systems the radio runs, absent or not a Tier III one
static int sys_read(uint8_t n, codeplug_system_t *h, uint8_t *v, uint16_t cap)
{
	if (n >= RADIO_T3_SYSTEMS) {
		return -1;
	}
	int len = codeplug_system(n, h, v, cap);
	return len >= 0 && h->type == CODEPLUG_SYSTYPE_DMR_TRUNK ? len : -1;
}

bool radio_t3_name(uint8_t n, char *name)
{
	uint8_t v[CODEPLUG_SYS_HDR];
	codeplug_system_t h;
	name[0] = '\0';
	if (sys_read(n, &h, v, 0) < 0) {
		return false;
	}
	memcpy(name, h.name, CODEPLUG_NAME_LEN + 1);
	return true;
}

// The key of system n into cfg: false when it has none
static bool load_key(uint8_t n, t3_cfg_t *c)
{
	uint8_t v[KEY_LEN + 3u];
	int len = codeplug_system_secret(n, v, sizeof v);
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
	uint8_t v[CODEPLUG_SYS_HDR + BLOCK_LEN];
	codeplug_system_t h;
	radio_t3_reset(s);
	int len = sys_read(n, &h, v, BLOCK_LEN);
	if (len < (int)BLOCK_MIN) {
		return false;
	}
	t3_cfg_t *c = &s->cfg;
	s->sys = n;
	c->auth.model = v[0] & 0x03u;
	c->auth.net = rd16(&v[1]);
	c->auth.category = v[3] == T3_PAR_B ? T3_PAR_B : T3_PAR_A;
	s->cc = v[4] & 0x0Fu;
	uint16_t opts = rd16(&v[5]);
	// The transmit side is the header's flag: a record without it (a zeroed one too) never registers or transmits
	c->opts = (uint16_t)((opts & CP_OPTS) | ((h.flags & CODEPLUG_SYS_TX) != 0 ? 0u : T3_OPT_LISTEN_ONLY));
	s->ip_gw = (opts & OPT_IP_GW) != 0;
	s->allcall = (opts & T3_OPT_ALLCALL) != 0;
	c->own_id = h.own_id <= CODEPLUG_DMR_ID_MAX ? h.own_id : 0;
	s->fb_hold_s = v[7];
	s->fb_chan = h.fb_chan;
	s->fb_check = h.fb_check;
	c->plan.fixed.base_hz = rd32(&v[8]);
	c->plan.fixed.first = rd16(&v[12]);
	c->plan.fixed.sep = v[14];
	c->plan.fixed.tx_low = (v[15] & PLAN_TX_LOW) != 0;
	c->plan.learn = (v[15] & PLAN_NO_LEARN) == 0;
	c->plan.fixed.split_hz = rd32(&v[16]);
	c->leave_dbm = (int8_t)v[20];
	c->ps_rq = v[21] <= 7u ? v[21] : 0;
	// The sites the block holds (a record cut short names fewer)
	for (uint8_t i = 0; i < v[22] && i < T3_SITES_MAX && (int)(BLOCK_MIN + 2u * i + 2u) <= len; i++) {
		c->sites[c->auth.nsites++] = rd16(&v[BLOCK_MIN + 2u * i]);
	}
	bool keyed = load_key(n, c);
	// The short data poll (5): UDT_Format, the blocks asked for, then the reply's octets in that format
	uint8_t p[2u + T3_UDT_MAX];
	// (a get gives the stored length, which may pass what it copied: a record is cut to the buffer)
	int plen = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_POLL + n), p, sizeof p);
	plen = plen > (int)sizeof p ? (int)sizeof p : plen;
	if (plen >= 2) {
		c->poll_format = p[0] & 0x0Fu;
		s->poll_blocks = p[1] >= 1u && p[1] <= T3_UDT_BLOCKS_MAX ? p[1] : 0u;
		c->poll_len = (uint8_t)(plen - 2);
		memcpy(s->poll_reply, p + 2, c->poll_len);
	}
#if CONFIG_DMR_TRUNK_SUPL
	// Supplementary user data offered with each call (5): UDT_Format, then the octets (the poll's buffer)
	plen = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_SUPL + n), p, 1u + T3_UDT_MAX);
	plen = plen > (int)(1u + T3_UDT_MAX) ? (int)(1u + T3_UDT_MAX) : plen;
	if (plen >= 2) {
		c->supl_format = p[0] & 0x0Fu;
		c->supl_len = (uint8_t)(plen - 1);
		memcpy(s->supl, p + 1, c->supl_len);
	}
#endif

	// Channels in entry order: the plan's table, and the control channels among them
	codeplug_sys_entry_t e;
	uint8_t r[CHAN_BODY];
	int at = -1;
	while (s->nchans < RADIO_T3_CHANS && codeplug_sys_entry_next(&at, n, CODEPLUG_ENTRY_CHANNEL, &e, r, sizeof r) >= 0) {
		if (e.len < CHAN_BODY) {
			continue;
		}
		t3_chan_t ch = { .lcn = rd16(&r[0]), .rx_hz = rd32(&r[2]), .tx_hz = rd32(&r[6]), .cc = r[10] };
		s->chans[s->nchans++] = ch;
		if ((e.flags & CODEPLUG_ENTRY_CONTROL) != 0 && c->nctrl < T3_CTRL_MAX) {
			c->ctrl[c->nctrl++] = ch;
		}
	}
	radio_t3_fix(s);
	return c->nctrl > 0 && (keyed || (h.flags & CODEPLUG_SYS_NEEDS_KEY) == 0);
}

// The checks of radio_t3_load without its buffer: the record, the key if one is needed, a control channel
static bool usable(uint8_t n)
{
	uint8_t v[CODEPLUG_SYS_HDR], k[KEY_LEN];
	codeplug_system_t h;
	if (sys_read(n, &h, v, 0) < (int)BLOCK_MIN) {
		return false;
	}
	if ((h.flags & CODEPLUG_SYS_NEEDS_KEY) != 0 && codeplug_system_secret(n, k, sizeof k) < (int)KEY_LEN) {
		return false;
	}
	// Among the channels radio_t3_load takes: a control channel past them is never hunted on
	codeplug_sys_entry_t e;
	int at = -1;
	uint8_t chans = 0;
	while (chans < RADIO_T3_CHANS && codeplug_sys_entry_next(&at, n, CODEPLUG_ENTRY_CHANNEL, &e, k, 0) >= 0) {
		if (e.len < CHAN_BODY) {
			continue;
		}
		if ((e.flags & CODEPLUG_ENTRY_CONTROL) != 0) {
			return true;
		}
		chans++;
	}
	return false;
}

bool radio_t3_usable(uint8_t n)
{
	// Asked for each trunked row of a list drawn and each channel stepped past, on the UI and radio tasks: kept per
	// system until the codeplug changes. One word (serial << 24, set << 16, systems known << 8, usable), stored
	// whole: a store another task loses only costs that task a check again
	static volatile uint32_t cache;
	if (n >= RADIO_T3_SYSTEMS) {
		return false;
	}
	uint32_t serial = codeplug_kv_serial(), c = cache, bit = 1u << n;
	if ((c >> 24) != serial || (c & 0x10000u) == 0u) {
		c = (serial << 24) | 0x10000u;	// set: a serial of 0 doesn't match a zeroed word
	}
	if ((c & (bit << 8)) == 0u) {
		c |= (bit << 8) | (usable(n) ? bit : 0u);
		cache = c;
	}
	return (c & bit) != 0u;
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
	c->opts = (uint16_t)((c->opts & ~T3_OPT_ALLCALL) | (s->allcall ? T3_OPT_ALLCALL : 0u));	// the last channel's goes
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
			if (tg_add(c, ct.id, T3_TG_TALK | T3_TG_LISTEN) == NULL) {
				// A full table: the talkgroup PTT calls takes the RX list's last place
				c->tg[T3_TG_MAX - 1u] = (t3_tg_t){ .id = ct.id, .flags = T3_TG_TALK | T3_TG_LISTEN };
			}
		} else {
			s->talk_id = ct.id;	// an individual call, or the all-call ID
			s->talk_private = ct.type == CONTACT_PRIVATE;
		}
	} else if (c->ntg > 0) {
		c->tg[0].flags |= T3_TG_TALK;
	}
}

uint16_t radio_t3_fallback(uint16_t number, const radio_t3_setup_t *s, channel_t *out)
{
	codeplug_tag_t t;
	uint16_t n = s->fb_chan, check = s->fb_check;
	if (codeplug_tag_read(number, &t)) {
		if ((t.flags & RADIO_T3_TAG_NO_FALLBACK) != 0) {
			return 0;
		}
		if ((t.flags & CODEPLUG_TAG_FALLBACK) != 0) {
			n = t.fb_chan;		// the channel's own, 0: none
			check = t.fb_check;
		}
	}
	if (n == 0 || !codeplug_channel(n, out) || codeplug_name_check(out->name) != check ||
	    out->kind != CODEPLUG_KIND_NONE) {
		return 0;	// none, gone, renamed (renumbered), or a channel of a kind (itself trunked)
	}
	return n;
}

#endif
