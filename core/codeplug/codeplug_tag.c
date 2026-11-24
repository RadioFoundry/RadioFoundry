// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Channel tags (codeplug_tag.h, docs/spec/channel-modes.md 2). No kind's fields here: the parameter block is bytes
// for the kind's mode module
#include <string.h>
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_kv.h"
#include "storage/storage.h"

#define TAG_PARAMS_MAX	64u	// the longest parameter block this firmware writes (it reads and keeps any length)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t tag_key(uint16_t number) { return (uint16_t)(CODEPLUG_KEY_CH_TAG + number - 1u); }
static bool number_ok(uint16_t number) { return number >= 1u && number <= CODEPLUG_CHANNELS_MAX; }

uint16_t codeplug_name_check(const char *name)
{
	uint32_t h = 2166136261u;
	bool end = false;
	for (unsigned i = 0; i < CODEPLUG_NAME_LEN; i++) {
		uint8_t c = end ? 0u : (uint8_t)name[i];
		if (c == 0xFFu) {
			c = 0u;		// a record's padding: the name ends there
		}
		end = end || c == 0u;
		h = (h ^ c) * 16777619u;
	}
	return (uint16_t)(h ^ (h >> 16));
}

// A value's header into t: false when it is no tag (too short, kind 0 or 0xFF, a fallback field cut off)
static bool parse(const uint8_t *v, int len, codeplug_tag_t *t)
{
	if (len < (int)CODEPLUG_TAG_HDR || v[0] == 0u || v[0] == 0xFFu ||
	    ((v[4] & CODEPLUG_TAG_FALLBACK) != 0 && len < (int)CODEPLUG_TAG_HEAD_MAX)) {
		return false;
	}
	t->kind = v[0];
	t->system = v[1];
	t->check = rd16(&v[2]);
	t->flags = v[4];
	t->fb_chan = (t->flags & CODEPLUG_TAG_FALLBACK) != 0 ? rd16(&v[5]) : 0u;
	t->fb_check = (t->flags & CODEPLUG_TAG_FALLBACK) != 0 ? rd16(&v[7]) : 0u;
	t->len = (uint16_t)len;
	return true;
}

bool codeplug_tag_read(uint16_t number, codeplug_tag_t *t)
{
	uint8_t v[CODEPLUG_TAG_HEAD_MAX];
	return number_ok(number) && parse(v, codeplug_kv_get(tag_key(number), v, sizeof v), t);
}

bool codeplug_tag_live(const codeplug_tag_t *t, const char *name, uint8_t mode, bool rx_only)
{
	return t->check == codeplug_name_check(name) &&
	       ((t->flags & CODEPLUG_TAG_BASE_OWN) != 0 || (mode == RADIO_MODE_DMR && rx_only));
}

int codeplug_tag_params(uint16_t number, const channel_t *ch, uint8_t *buf, uint16_t cap)
{
	codeplug_tag_t t;
	if (ch->kind == CODEPLUG_KIND_NONE || !number_ok(number)) {
		return -1;
	}
	int len = codeplug_kv_get(tag_key(number), buf, (uint16_t)(cap + CODEPLUG_TAG_HEAD_MAX));
	if (!parse(buf, len, &t) || t.kind != ch->kind || t.check != codeplug_name_check(ch->name)) {
		return -1;
	}
	uint16_t at = codeplug_tag_params_at(&t);
	int n = len - (int)at;
	memmove(buf, buf + at, (size_t)(n < (int)cap ? n : (int)cap));
	return n;
}

bool codeplug_tag_write(uint16_t number, const char *name, const codeplug_tag_t *t, const uint8_t *params, uint16_t len)
{
	uint8_t v[CODEPLUG_TAG_HEAD_MAX + TAG_PARAMS_MAX];
	if (!number_ok(number) || t->kind == 0u || t->kind == 0xFFu || len > TAG_PARAMS_MAX) {
		return false;
	}
	v[0] = t->kind;
	v[1] = t->system;
	wr16(&v[2], codeplug_name_check(name));
	v[4] = t->flags;
	uint16_t at = CODEPLUG_TAG_HDR;
	if ((t->flags & CODEPLUG_TAG_FALLBACK) != 0) {
		wr16(&v[5], t->fb_chan);
		wr16(&v[7], t->fb_chan != 0 ? t->fb_check : 0u);
		at = CODEPLUG_TAG_HEAD_MAX;
	}
	if (len != 0) {
		memcpy(&v[at], params, len);
	}
	return codeplug_kv_set(tag_key(number), v, (uint16_t)(at + len));
}

static bool rewrite_checked(uint16_t number, uint16_t check, const char *name, const codeplug_tag_t *t, const uint8_t *params, uint16_t len)
{
	uint8_t v[CODEPLUG_TAG_HEAD_MAX + TAG_PARAMS_MAX];
	codeplug_tag_t old;
	if (!number_ok(number) || len > TAG_PARAMS_MAX ||
	    !parse(v, codeplug_kv_get(tag_key(number), v, CODEPLUG_TAG_HEAD_MAX), &old) || old.kind != t->kind ||
	    old.check != check) {
		return false;
	}
	uint16_t at = codeplug_tag_params_at(&old);
	v[1] = t->system;
	wr16(&v[2], codeplug_name_check(name));
	v[4] = (uint8_t)((old.flags & ~CODEPLUG_TAG_TX) | (t->flags & CODEPLUG_TAG_TX));
	if (len != 0) {
		memcpy(&v[at], params, len);
	}
	// From the front: a block longer than this firmware writes keeps its later bytes
	codeplug_kv_op_t op = { tag_key(number), (uint16_t)((at + len) | CODEPLUG_KV_HEAD), v };
	return codeplug_kv_write(&op, 1);
}

bool codeplug_tag_rewrite(uint16_t number, const char *name, const codeplug_tag_t *t, const uint8_t *params, uint16_t len)
{
	return rewrite_checked(number, codeplug_name_check(name), name, t, params, len);
}

bool codeplug_tag_copy(uint16_t from, uint16_t to, const channel_t *ch)
{
	uint8_t v[CODEPLUG_TAG_HEAD_MAX + TAG_PARAMS_MAX];
	char name[CODEPLUG_NAME_LEN + 1];
	codeplug_tag_t t;
	if (ch->kind == CODEPLUG_KIND_NONE || !number_ok(from) || !number_ok(to) || from == to ||
	    !codeplug_channel_name(from, name)) {
		return false;
	}
	int len = codeplug_kv_get(tag_key(from), v, sizeof v);
	if (len > (int)sizeof v || !parse(v, len, &t) || t.kind != ch->kind || t.check != codeplug_name_check(name)) {
		return false;
	}
	// A source whose record is a channel of its own: the copy was written as a stand-in (no tag followed it), so it
	// takes that record's mode, receive-only bit and bandwidth before the tag says so. Where that fails the copy's
	// tag says what its record is, a stand-in
	if ((t.flags & CODEPLUG_TAG_BASE_OWN) != 0 && !codeplug_base_copy(from, to)) {
		t.flags &= (uint8_t)~CODEPLUG_TAG_BASE_OWN;
	}
	v[1] = ch->system;
	wr16(&v[2], codeplug_name_check(ch->name));
	v[4] = (uint8_t)((t.flags & ~CODEPLUG_TAG_TX) | (ch->rx_only ? 0u : CODEPLUG_TAG_TX));
	return codeplug_kv_set(tag_key(to), v, (uint16_t)len);
}

bool codeplug_tag_remove(uint16_t number)
{
	// Nothing stored, nothing to write: a codeplug without tags (or an area a later firmware wrote, never ours to
	// rewrite) still deletes its channels
	uint8_t b;
	return !number_ok(number) || codeplug_kv_get(tag_key(number), &b, 1) < 0 || codeplug_kv_remove(tag_key(number));
}

// Whether the area holds any tag, found once per change of codeplug_kv_serial(): a codeplug without tags pays no
// lookup per channel read. Under storage_lock
static uint8_t seen_serial;
static uint8_t any_state;	// 0 not looked, 1 none, 2 some

static bool any_tag(void)
{
	uint8_t s = codeplug_kv_serial();
	if (any_state == 0u || s != seen_serial) {
		codeplug_kv_cursor_t c;
		uint16_t key;
		uint8_t b;
		codeplug_kv_begin(&c, CODEPLUG_KEY_CH_TAG, (uint16_t)(CODEPLUG_KEY_CH_TAG + CODEPLUG_CHANNELS_MAX - 1u));
		any_state = codeplug_kv_next(&c, &key, &b, 1) >= 0 ? 2u : 1u;
		seen_serial = s;
	}
	return any_state == 2u;
}

void codeplug_tag_apply(uint16_t number, channel_t *ch)
{
	codeplug_tag_t t;
	if (!any_tag() || !codeplug_tag_read(number, &t) || !codeplug_tag_live(&t, ch->name, ch->mode, ch->rx_only)) {
		return;
	}
	ch->kind = t.kind;
	ch->system = t.system;
#if CONFIG_DMR_AES
	if (t.kind == CODEPLUG_KIND_DMR_AES) {
		uint8_t block[CODEPLUG_TAG_HEAD_MAX + 2];
		ch->aes_ref = codeplug_tag_params(number, ch, block, 2) >= 2 && block[0] == 0x25 ?
			      (uint16_t)(1u + block[1]) : 0;
	}
#endif
	ch->rx_only = (t.flags & CODEPLUG_TAG_TX) == 0;
}

uint8_t codeplug_tag_before_write(uint16_t number, const channel_t *ch, bool in_use, const char *old_name,
				  codeplug_tag_t *t, bool *safe_shape)
{
	uint8_t action = CODEPLUG_TAG_W_NONE;
	if (any_tag() && codeplug_tag_read(number, t)) {
		if (!in_use) {
			action = CODEPLUG_TAG_W_REMOVE;		// a free number's tag is no channel's: a new channel starts clean
		} else if (ch->kind != CODEPLUG_KIND_NONE && t->kind == ch->kind && t->check == codeplug_name_check(old_name)) {
			action = CODEPLUG_TAG_W_FOLLOW;
		} else if (t->check == codeplug_name_check(old_name) || t->check == codeplug_name_check(ch->name)) {
			action = CODEPLUG_TAG_W_REMOVE;		// this channel's, made plain, or one that would go live on it
		} else {
			action = CODEPLUG_TAG_W_KEEP;		// a stale tag: kept as found, never made live
		}
	}
	// A channel of a tagged kind keeps the base record every reader takes as harmless, also where its tag isn't (a
	// copy at another number): DMR, receive only
	*safe_shape = ch->kind != CODEPLUG_KIND_NONE &&
		      !(action == CODEPLUG_TAG_W_FOLLOW && (t->flags & CODEPLUG_TAG_BASE_OWN) != 0);
	return action;
}

bool codeplug_tag_after_write(uint16_t number, const channel_t *ch, uint8_t action, const codeplug_tag_t *t)
{
#if CONFIG_DMR_AES
	if (ch->kind == CODEPLUG_KIND_DMR_AES && ch->aes_ref >= 1 && ch->aes_ref <= 256) {
		codeplug_tag_t aes = { .kind = ch->kind, .system = ch->system, .flags = ch->rx_only ? 0 : CODEPLUG_TAG_TX };
		uint8_t params[] = { 0x25, (uint8_t)(ch->aes_ref - 1) };
		if (action == CODEPLUG_TAG_W_FOLLOW)
			return rewrite_checked(number, t->check, ch->name, &aes, params, sizeof params);
		return codeplug_tag_write(number, ch->name, &aes, params, sizeof params);
	}
#endif
	if (action == CODEPLUG_TAG_W_REMOVE) {
		return codeplug_tag_remove(number);
	}
	if (action != CODEPLUG_TAG_W_FOLLOW) {
		return true;
	}
	// The header only, from the front: the fallback and the parameter block stay as stored
	uint8_t v[CODEPLUG_TAG_HDR];
	v[0] = t->kind;
	v[1] = ch->system;
	wr16(&v[2], codeplug_name_check(ch->name));
	v[4] = (uint8_t)((t->flags & ~CODEPLUG_TAG_TX) | (ch->rx_only ? 0u : CODEPLUG_TAG_TX));
	codeplug_kv_op_t op = { tag_key(number), (uint16_t)(CODEPLUG_TAG_HDR | CODEPLUG_KV_HEAD), v };
	return codeplug_kv_write(&op, 1);
}
