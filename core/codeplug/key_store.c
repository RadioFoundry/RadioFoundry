// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stored keys as one list with their check values (codeplug/key_store.h, docs/spec/key-check.md 4)
#include "codeplug/key_store.h"
#if KEY_STORE	// the whole file
#include <string.h>
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_records.h"
#include "settings/settings.h"
#if CONFIG_DMR_PRIVACY
#include "dmr/dmrPrivacy.h"
#endif

#define TRAFFIC_RECORDS	0x0400u		// the family's keys, 0x1900-0x1CFF (channel-modes.md 3)

#if CONFIG_DMR_PRIVACY
// Privacy slot entry n (0 = the first slot holding a valid key): true and *e filled, else how many there are in *count
static bool privacy_entry(uint16_t n, key_entry_t *e, uint8_t *key, uint16_t *count)
{
	// one field at a time (settings_read_field), not a whole settings_t on the caller's stack
	uint16_t seen = 0;
	bool found = false;
	uint32_t value = 0;
	for (uint8_t i = 0; i < SETTINGS_PRIVACY_KEYS && !found; i++) {
		uint8_t type = SETTINGS_FIELD(privacy_type[i]);
		value = SETTINGS_FIELD(privacy_key[i]);
		if (type == DMR_PRIVACY_OFF || type >= DMR_PRIVACY_NUM_TYPES || value > 0xFFFFu ||
		    !dmrPrivacyKeyIsValid((dmrPrivacyType_t)type, (uint16_t)value))
			continue;
		if (seen++ != n)
			continue;
		memset(e, 0, sizeof *e);
		e->cls = KCV_CLASS_PRIVACY;
		e->slot = (uint8_t)(i + 1u);
		e->type = type;
		e->len = 4;
		e->kcv_ok = kcv_privacy(type, value, e->kcv);
		if (key != NULL) {
			key[0] = (uint8_t)(value >> 24);
			key[1] = (uint8_t)(value >> 16);
			key[2] = (uint8_t)(value >> 8);
			key[3] = (uint8_t)value;
		}
		found = true;
	}
	crypto_wipe(&value, sizeof value);	// the key was in this copy
	*count = seen;
	return found;
}
#endif

#if CODEPLUG_MODE_RECORDS
// The systems one walk has looked up: a few systems hold all the keys, and each lookup checks the whole area
typedef struct {
	uint8_t n;
	uint8_t id[4];
	bool ok[4];
	uint8_t type[4];			// only what an entry takes from its system: the type and the name (a whole
	char name[4][CODEPLUG_NAME_LEN + 1];	// codeplug_system_t each was on the UI task's deepest path; stack-check.md)
} sys_cache_t;

// System id's slot in the cache, looked up once per walk; -1 when the system doesn't exist
static int cached_system(sys_cache_t *c, uint8_t id)
{
	for (uint8_t i = 0; i < c->n; i++) {
		if (c->id[i] == id)
			return c->ok[i] ? i : -1;
	}
	codeplug_system_t s;
	uint8_t block[CODEPLUG_SYS_HDR];
	memset(&s, 0, sizeof s);	// the name's tail too: an entry copies it whole
	bool ok = codeplug_system(id, &s, block, 0) >= 0;
	uint8_t slot = c->n < 4u ? c->n++ : (uint8_t)(id & 3u);
	c->id[slot] = id;
	c->ok[slot] = ok;
	c->type[slot] = s.type;
	memcpy(c->name[slot], s.name, sizeof c->name[slot]);
	return ok ? slot : -1;
}
#endif

#if CODEPLUG_MODE_RECORDS
// Entry *e from traffic record k (its value v, len octets, read whole up to KEY_STORE_MAX_LEN of key) of the system in
// cache slot si: the one fill for a single entry and for a page
static void traffic_fill(key_entry_t *e, const sys_cache_t *sc, int si, const uint8_t *v, int len, uint16_t k)
{
	memset(e, 0, sizeof *e);
	e->cls = KCV_CLASS_TRAFFIC;
	e->type = sc->type[si];
	e->system = v[0];
	e->alg = v[1];
	e->key_id = (uint16_t)(v[2] | v[3] << 8);
	e->flags = v[4];
	e->record = k;
	e->len = (uint16_t)(len - (int)CODEPLUG_TRAFFIC_HDR);
	memcpy(e->name, sc->name[si], sizeof e->name);
	// A key longer than the walk copied has no check value here (none in this tree is)
	e->kcv_ok = e->len <= KEY_STORE_MAX_LEN && kcv_traffic(e->type, e->alg, &v[CODEPLUG_TRAFFIC_HDR], e->len, e->kcv);
}

// Traffic key entry n, in the area's order, counting only records whose system exists: as privacy_entry
static bool traffic_entry(uint16_t n, key_entry_t *e, uint8_t *key, uint16_t *count)
{
	uint8_t v[CODEPLUG_TRAFFIC_HDR + KEY_STORE_MAX_LEN];
	codeplug_kv_cursor_t c;
	uint16_t k, seen = 0;
	int len;
	bool found = false;
	sys_cache_t sc;
	sc.n = 0;
	codeplug_kv_begin(&c, CODEPLUG_KEY_TRAFFIC, (uint16_t)(CODEPLUG_KEY_TRAFFIC + TRAFFIC_RECORDS - 1u));
	while (!found && (len = codeplug_kv_next(&c, &k, v, sizeof v)) >= 0) {
		int si;
		if (len <= (int)CODEPLUG_TRAFFIC_HDR || (si = cached_system(&sc, v[0])) < 0)
			continue;
		if (seen++ != n)
			continue;
		traffic_fill(e, &sc, si, v, len, k);
		if (key != NULL)
			memcpy(key, &v[CODEPLUG_TRAFFIC_HDR], e->len < KEY_STORE_MAX_LEN ? e->len : KEY_STORE_MAX_LEN);
		found = true;
	}
	crypto_wipe(v, sizeof v);
	*count = seen;
	return found;
}
#endif

bool key_store_get(uint16_t i, key_entry_t *e, uint8_t *key)
{
	uint16_t n = 0;
#if CONFIG_DMR_PRIVACY
	if (privacy_entry(i, e, key, &n))
		return true;
	i = (uint16_t)(i - n);
#endif
#if CODEPLUG_MODE_RECORDS
	if (traffic_entry(i, e, key, &n))
		return true;
#endif
	(void)n;
	return false;
}


// Entry *e cut to row *r, its system's name kept in *names while there is room
static void row_from(key_row_t *r, const key_entry_t *e, key_names_t *names)
{
	memset(r, 0, sizeof *r);
	r->cls = e->cls;
	r->kcv_ok = e->kcv_ok;
	r->num = e->cls == KCV_CLASS_PRIVACY ? e->slot : e->system;
	r->type = e->type;
	memcpy(r->kcv, e->kcv, KCV_LEN);
	r->key_id = e->key_id;
	if (e->cls != KCV_CLASS_TRAFFIC || e->name[0] == 0)
		return;
	for (uint8_t i = 0; i < names->n; i++) {
		if (names->system[i] == e->system)
			return;
	}
	if (names->n < KEY_NAMES) {
		names->system[names->n] = e->system;
		memcpy(names->name[names->n++], e->name, CODEPLUG_NAME_LEN);
	}
}

bool key_names_get(const key_names_t *names, uint8_t system, char out[CODEPLUG_NAME_LEN + 1])
{
	for (uint8_t i = 0; i < names->n; i++) {
		if (names->system[i] == system) {
			memcpy(out, names->name[i], CODEPLUG_NAME_LEN);
			out[CODEPLUG_NAME_LEN] = 0;
			return true;
		}
	}
	return false;
}

uint16_t key_store_window(uint16_t first, key_row_t *out, uint16_t max, uint16_t *total, key_names_t *names)
{
	uint16_t filled = 0, at = 0;
	key_entry_t e;	// one entry at a time, cut to a row
	names->n = 0;
#if CONFIG_DMR_PRIVACY
	uint16_t np = 0;
	(void)privacy_entry(0xFFFFu, &e, NULL, &np);
	for (uint16_t i = first; i < np && filled < max; i++) {
		if (privacy_entry(i, &e, NULL, &at))
			row_from(&out[filled++], &e, names);
	}
	at = np;
#endif
#if CODEPLUG_MODE_RECORDS
	uint8_t v[CODEPLUG_TRAFFIC_HDR + KEY_STORE_MAX_LEN];
	sys_cache_t sc;
	codeplug_kv_cursor_t c;
	uint16_t k;
	int len;
	sc.n = 0;
	codeplug_kv_begin(&c, CODEPLUG_KEY_TRAFFIC, (uint16_t)(CODEPLUG_KEY_TRAFFIC + TRAFFIC_RECORDS - 1u));
	while ((len = codeplug_kv_next(&c, &k, v, sizeof v)) >= 0) {
		int si;
		if (len <= (int)CODEPLUG_TRAFFIC_HDR || (si = cached_system(&sc, v[0])) < 0)
			continue;
		uint16_t n = at++;
		if (n < first || filled >= max)
			continue;
		traffic_fill(&e, &sc, si, v, len, k);
		row_from(&out[filled++], &e, names);
	}
	crypto_wipe(v, sizeof v);
#endif
	(void)e;
	*total = at;
	return filled;
}

uint16_t key_store_count(void)
{
	key_entry_t e;
	uint16_t n = 0, total = 0;
#if CONFIG_DMR_PRIVACY
	(void)privacy_entry(0xFFFFu, &e, NULL, &n);
	total = (uint16_t)(total + n);
#endif
#if CODEPLUG_MODE_RECORDS
	(void)traffic_entry(0xFFFFu, &e, NULL, &n);
	total = (uint16_t)(total + n);
#endif
	(void)e;
	(void)n;
	return total;
}

#endif	// KEY_STORE
