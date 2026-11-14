// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tier III systems in zone shares (radio_trunk.h, docs/spec/zone-share.md 4.2): a system out of the codeplug into a
// share record, and a record's system into the codeplug. Runs in the storage job that imports, or on the UI task that
// exports; never the radio task
#include <string.h>
#include "radio/radio_trunk.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "radio/radio_trunk_rec.h"

#if CONFIG_DMR_TRUNK && CONFIG_ZONE_SHARE

_Static_assert(ZS_SYS_CHANS_MAX >= RADIO_T3_CHANS, "a share carries every channel a radio loads of a system");

bool radio_t3_share_out(uint8_t n, zsSystem_t *s, zsSysChan_t *chan, uint16_t *fb_chan)
{
	uint8_t v[SYS_LEN];
	memset(s, 0, sizeof *s);
	*fb_chan = 0;
	if (n >= RADIO_T3_SYSTEMS || codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + n), v, sizeof v) < (int)SYS_LEN ||
	    (v[SYS_SHARING] != ZS_SYS_IDENTITY && v[SYS_SHARING] != ZS_SYS_FULL)) {
		return false;
	}
	s->level = v[SYS_SHARING];
	for (uint8_t i = 0; i < ZS_NAME_MAX && v[i] != 0; i++) {
		s->name[i] = (char)v[i];
	}
	s->model = v[16] & 0x03u;
	s->net = rd16(&v[17]);
	s->category = v[19] == T3_PAR_B ? 2u : 1u;
	s->cc = v[20] & 0x0Fu;
	s->opts = rd16(&v[21]) & ZS_SYS_OPTS;
	if (s->level != ZS_SYS_FULL) {
		return true;
	}
	s->holdS = v[28];
	*fb_chan = rd16(&v[SYS_FB_CHAN]);
	s->planBaseHz = rd32(&v[31]);
	s->planFirst = rd16(&v[35]);
	s->planSep = v[37];
	s->planFlags = v[38] & (PLAN_TX_LOW | PLAN_NO_LEARN);
	s->planSplitHz = rd32(&v[39]);
	s->leaveDbm = (int8_t)v[43];
	s->psRq = v[44] <= 7u ? v[44] : 0;
	for (uint8_t i = 0; i < v[45] && i < ZS_SYS_SITES_MAX; i++) {
		s->site[s->sites++] = rd16(&v[46 + 2 * i]);
	}
	uint8_t r[CHAN_LEN];
	for (uint16_t key = radio_t3_next_chan(n, CODEPLUG_KEY_T3_CHAN - 1u, r); key != 0 && s->chans < ZS_SYS_CHANS_MAX;
	     key = radio_t3_next_chan(n, key, r)) {
		zsSysChan_t *c = &chan[s->chans];
		c->ctrl = (r[1] & CHAN_CTRL) != 0;
		c->lcn = rd16(&r[2]);
		c->rxHz = rd32(&r[4]);
		c->txHz = rd32(&r[8]);
		c->cc = r[12] <= 15u ? r[12] : 0xFFu;
		if (c->rxHz != 0 && c->rxHz <= 0x7FFFFFFFu && c->txHz != 0 && c->txHz <= 0x7FFFFFFFu) {
			s->chans++;	// one the record can't carry is left out
		}
	}
	if (s->chans == 0) {
		s->level = ZS_SYS_IDENTITY;	// nothing to set it up with: its identity only
		*fb_chan = 0;
		return true;
	}
	uint8_t k[KEY_LEN + 3u];
	int len = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_KEY + n), k, sizeof k);
	if (len >= (int)KEY_LEN) {
		s->hasKey = true;
		memcpy(s->key, k, KEY_LEN);
		s->hasPsn = len >= (int)KEY_LEN + 3;
		s->psn = s->hasPsn ? ((uint32_t)k[16] << 16) | ((uint32_t)k[17] << 8) | k[18] : 0u;
	}
	return true;
}

int radio_t3_find(uint8_t model, uint16_t net, uint8_t category)
{
	uint8_t v[SYS_LEN];
	for (uint8_t n = 0; n < RADIO_T3_SYSTEMS; n++) {
		if (codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + n), v, sizeof v) >= (int)SYS_LEN &&
		    (v[16] & 0x03u) == model && rd16(&v[17]) == net && v[19] == category) {
			return n;
		}
	}
	return -1;
}

void radio_t3_remove(uint8_t n)
{
	codeplug_kv_cursor_t c;
	uint8_t r[CHAN_LEN];
	uint16_t key;
	int len;
	if (n >= RADIO_T3_SYSTEMS) {
		return;
	}
	// One channel at a time: a removal moves the area under the cursor
	for (bool more = true; more;) {
		more = false;
		codeplug_kv_begin(&c, CODEPLUG_KEY_T3_CHAN, CODEPLUG_KEY_T3_CHAN + 0xFFu);
		while ((len = codeplug_kv_next(&c, &key, r, sizeof r)) >= 0) {
			if (len >= 1 && r[0] == n) {
				more = codeplug_kv_remove(key);
				break;
			}
		}
	}
	(void)codeplug_kv_remove((uint16_t)(CODEPLUG_KEY_T3_KEY + n));
	(void)codeplug_kv_remove((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + n));
}

int radio_t3_share_in(const zsRecord_t *rec, int index, const zsSystem_t *s)
{
	uint8_t v[SYS_LEN], probe[1];
	int n = -1;
	for (uint8_t i = 0; i < RADIO_T3_SYSTEMS && n < 0; i++) {
		if (codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + i), probe, sizeof probe) < 0) {
			n = i;
		}
	}
	if (n < 0) {
		return -1;
	}
	memset(v, 0, sizeof v);
	memcpy(v, s->name, strnlen(s->name, ZS_NAME_MAX));
	v[16] = s->model;
	wr16(&v[17], s->net);
	v[19] = s->category;
	v[20] = s->cc;
	wr16(&v[21], s->opts & ZS_SYS_OPTS);
	// Own ID 0 (the radio's), sharing off: the receiver chooses whether to pass it on
	if (s->level == ZS_SYS_FULL) {
		v[28] = s->holdS;
		wr32(&v[31], s->planBaseHz);
		wr16(&v[35], s->planFirst);
		v[37] = s->planSep;
		v[38] = s->planFlags;
		wr32(&v[39], s->planSplitHz);
		v[43] = (uint8_t)s->leaveDbm;
		v[44] = s->psRq;
		v[45] = s->sites;
		for (uint8_t i = 0; i < s->sites && i < ZS_SYS_SITES_MAX; i++) {
			wr16(&v[46 + 2 * i], s->site[i]);
		}
	}
	if (!codeplug_kv_set((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + n), v, SYS_LEN)) {
		radio_t3_remove((uint8_t)n);
		return -1;
	}
	// Channels at the lowest free keys: the used ones from one walk of the area
	uint8_t used[32];
	uint16_t key;
	codeplug_kv_cursor_t cur;
	memset(used, 0, sizeof used);
	codeplug_kv_begin(&cur, CODEPLUG_KEY_T3_CHAN, CODEPLUG_KEY_T3_CHAN + 0xFFu);
	while (codeplug_kv_next(&cur, &key, probe, sizeof probe) >= 0) {
		used[(key & 0xFFu) >> 3] |= (uint8_t)(1u << (key & 7u));
	}
	unsigned at = 0;
	for (uint8_t i = 0; i < s->chans && s->level == ZS_SYS_FULL; i++) {
		while (at < 256u && (used[at >> 3] & (1u << (at & 7u))) != 0) {
			at++;
		}
		key = (uint16_t)(CODEPLUG_KEY_T3_CHAN + at++);
		uint8_t r[CHAN_LEN];
		zsSysChan_t c;
		bool ok = zsSysChanAt(rec, index, i, &c);
		r[0] = (uint8_t)n;
		r[1] = c.ctrl ? CHAN_CTRL : 0u;
		wr16(&r[2], c.lcn);
		wr32(&r[4], c.rxHz);
		wr32(&r[8], c.txHz);
		r[12] = c.cc;
		if (!ok || at > 256u || !codeplug_kv_set(key, r, CHAN_LEN)) {
			radio_t3_remove((uint8_t)n);
			return -1;
		}
	}
	if (s->level == ZS_SYS_FULL && s->hasKey) {
		uint8_t k[KEY_LEN + 3u];
		memcpy(k, s->key, KEY_LEN);
		k[16] = (uint8_t)(s->psn >> 16);
		k[17] = (uint8_t)(s->psn >> 8);
		k[18] = (uint8_t)s->psn;
		if (!codeplug_kv_set((uint16_t)(CODEPLUG_KEY_T3_KEY + n), k, (uint16_t)(s->hasPsn ? KEY_LEN + 3u : KEY_LEN))) {
			radio_t3_remove((uint8_t)n);
			return -1;
		}
	}
	return n;
}

bool radio_t3_set_fallback(uint8_t n, uint16_t c, const char *name)
{
	uint8_t v[SYS_LEN];
	if (n >= RADIO_T3_SYSTEMS || codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + n), v, sizeof v) < (int)SYS_LEN) {
		return false;
	}
	wr16(&v[SYS_FB_CHAN], c);
	wr16(&v[SYS_FB_CHECK], c != 0 ? radio_t3_name_check(name) : 0u);
	return codeplug_kv_set((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + n), v, SYS_LEN);
}

bool radio_t3_tag_set(uint16_t c, uint8_t n, const char *name)
{
	uint8_t v[TAG_LEN] = { n, 0 };
	if (c == 0 || c > CODEPLUG_CHANNELS_MAX) {
		return false;
	}
	wr16(&v[2], radio_t3_name_check(name));
	return codeplug_kv_set((uint16_t)(CODEPLUG_KEY_T3_TAG + c - 1u), v, TAG_LEN);
}

void radio_t3_tag_remove(uint16_t c)
{
	if (c != 0 && c <= CODEPLUG_CHANNELS_MAX) {
		(void)codeplug_kv_remove((uint16_t)(CODEPLUG_KEY_T3_TAG + c - 1u));
	}
}
#endif
