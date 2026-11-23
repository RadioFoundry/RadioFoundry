// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Readers of the mode-scoped record families and the systems (codeplug_records.h, docs/spec/channel-modes.md 5, 7)
#include <string.h>
#include "codeplug/codeplug_records.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_kv.h"

#if CODEPLUG_MODE_RECORDS

#define TARGETS_MAX	0x0800u
#define TLISTS_MAX	0x0100u
#define TRAFFIC_MAX	0x0400u

static uint16_t rec_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

codeplug_ref_t codeplug_ref_at(const uint8_t *p)
{
	codeplug_ref_t r = { rec_rd16(p), rec_rd16(p + 2) };
	return r;
}

int codeplug_mode_settings(uint8_t kind, uint8_t *buf, uint16_t cap)
{
	if (kind == CODEPLUG_KIND_NONE || kind == 0xFFu) {
		return -1;
	}
	return codeplug_kv_get((uint16_t)(CODEPLUG_KEY_MODE_SET + kind), buf, cap);
}

// The record at base + ref.index - 1, read whole into buf (cap + CODEPLUG_REC_HDR octets): its header into *h and
// the rest moved to buf's start (at most cap octets). The rest's stored length, -1 when ref doesn't resolve to a record
// of kind
static int record(uint16_t base, uint16_t count, codeplug_ref_t ref, uint8_t kind, codeplug_rec_head_t *h,
		  uint8_t *buf, uint16_t cap)
{
	if (ref.index == 0u || ref.index > count) {
		return -1;
	}
	int len = codeplug_kv_get((uint16_t)(base + ref.index - 1u), buf, (uint16_t)(cap + CODEPLUG_REC_HDR));
	if (len < (int)CODEPLUG_REC_HDR || buf[0] != kind || codeplug_name_check((const char *)&buf[2]) != ref.check) {
		return -1;
	}
	h->kind = buf[0];
	h->flags = buf[1];
	unsigned n = 0;
	while (n < CODEPLUG_NAME_LEN && buf[2 + n] != 0u && buf[2 + n] != 0xFFu) {
		h->name[n] = (char)buf[2 + n];
		n++;
	}
	h->name[n] = '\0';
	int rest = len - (int)CODEPLUG_REC_HDR;
	memmove(buf, buf + CODEPLUG_REC_HDR, (size_t)(rest < (int)cap ? rest : (int)cap));
	return rest;
}

int codeplug_target(codeplug_ref_t ref, uint8_t kind, codeplug_rec_head_t *h, uint8_t *addr, uint16_t cap)
{
	return record(CODEPLUG_KEY_TARGET, TARGETS_MAX, ref, kind, h, addr, cap);
}

int codeplug_target_list(codeplug_ref_t ref, uint8_t kind, codeplug_rec_head_t *h, codeplug_ref_t *refs, uint16_t max)
{
	// The value whole on the stack: a list longer than this reader holds is cut to max (its count still says how many)
	uint8_t buf[CODEPLUG_REC_HDR + 64u * CODEPLUG_REF_LEN];
	uint16_t cap = (uint16_t)(sizeof buf - CODEPLUG_REC_HDR);
	int len = record(CODEPLUG_KEY_TLIST, TLISTS_MAX, ref, kind, h, buf, cap);
	if (len < 0) {
		return -1;
	}
	int count = len / (int)CODEPLUG_REF_LEN;
	for (int i = 0; i < count && i < (int)max && (i + 1) * (int)CODEPLUG_REF_LEN <= (int)cap; i++) {
		refs[i] = codeplug_ref_at(&buf[i * CODEPLUG_REF_LEN]);
	}
	return count;
}

int codeplug_system(uint8_t n, codeplug_system_t *s, uint8_t *block, uint16_t cap)
{
	if (n == CODEPLUG_SYSTEM_NONE) {
		return -1;
	}
	int len = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_SYSTEM + n), block, (uint16_t)(cap + CODEPLUG_SYS_HDR));
	if (len < (int)CODEPLUG_SYS_HDR || block[0] == 0u || block[0] == 0xFFu) {
		return -1;
	}
	s->type = block[0];
	s->flags = block[1];
	s->sharing = block[2];
	s->id_len = block[3];
	s->own_id = (uint32_t)rec_rd16(&block[4]) | ((uint32_t)rec_rd16(&block[6]) << 16);
	s->fb_chan = rec_rd16(&block[8]);
	s->fb_check = rec_rd16(&block[10]);
	unsigned i = 0;
	while (i < CODEPLUG_NAME_LEN && block[12 + i] != 0u && block[12 + i] != 0xFFu) {
		s->name[i] = (char)block[12 + i];
		i++;
	}
	s->name[i] = '\0';
	int rest = len - (int)CODEPLUG_SYS_HDR;
	memmove(block, block + CODEPLUG_SYS_HDR, (size_t)(rest < (int)cap ? rest : (int)cap));
	return rest;
}

int codeplug_sys_entry_next(int *at, uint8_t system, uint8_t cls, codeplug_sys_entry_t *e, uint8_t *body,
			    uint16_t cap)
{
	uint8_t v[CODEPLUG_SYS_ENTRY_HDR + 64u];
	codeplug_kv_cursor_t c;
	uint16_t k;
	int len, best = -1;
	if (*at >= 0xFF) {
		return -1;
	}
	// The area holds entries in no order: the lowest matching number after *at. Entries are few and read when a
	// system starts
	codeplug_kv_begin(&c, (uint16_t)(CODEPLUG_KEY_SYS_CHAN + *at + 1), (uint16_t)(CODEPLUG_KEY_SYS_CHAN + 0xFFu));
	while ((len = codeplug_kv_next(&c, &k, v, sizeof v)) >= 0) {
		int n = k - CODEPLUG_KEY_SYS_CHAN;
		if (len < (int)CODEPLUG_SYS_ENTRY_HDR || v[0] != system || v[1] != cls || (best >= 0 && n > best)) {
			continue;
		}
		best = n;
		e->system = v[0];
		e->cls = v[1];
		e->group = v[2];
		e->flags = v[3];
		e->len = (uint16_t)(len - (int)CODEPLUG_SYS_ENTRY_HDR);
		int fit = len > (int)sizeof v ? (int)sizeof v - (int)CODEPLUG_SYS_ENTRY_HDR : (int)e->len;
		memcpy(body, &v[CODEPLUG_SYS_ENTRY_HDR], (size_t)(fit < (int)cap ? fit : (int)cap));
	}
	if (best >= 0) {
		*at = best;
	}
	return best;
}

int codeplug_system_secret(uint8_t n, uint8_t *buf, uint16_t cap)
{
	return n == CODEPLUG_SYSTEM_NONE ? -1 : codeplug_kv_get((uint16_t)(CODEPLUG_KEY_SYS_KEY + n), buf, cap);
}

int codeplug_traffic_key(uint8_t system, uint8_t alg, uint16_t key_id, uint8_t *key, uint16_t cap)
{
	// A walk over the family: keys are few, and only a call that needs one looks
	uint8_t v[CODEPLUG_TRAFFIC_HDR + 64u];
	codeplug_kv_cursor_t c;
	uint16_t k;
	int len;
	codeplug_kv_begin(&c, CODEPLUG_KEY_TRAFFIC, (uint16_t)(CODEPLUG_KEY_TRAFFIC + TRAFFIC_MAX - 1u));
	while ((len = codeplug_kv_next(&c, &k, v, sizeof v)) >= 0) {
		if (len >= (int)CODEPLUG_TRAFFIC_HDR && v[0] == system && v[1] == alg && rec_rd16(&v[2]) == key_id) {
			int n = len - (int)CODEPLUG_TRAFFIC_HDR;
			int fit = n < (int)(sizeof v - CODEPLUG_TRAFFIC_HDR) ? n : (int)(sizeof v - CODEPLUG_TRAFFIC_HDR);
			memcpy(key, &v[CODEPLUG_TRAFFIC_HDR], (size_t)(fit < (int)cap ? fit : (int)cap));
			return n;
		}
	}
	return -1;
}
#endif
