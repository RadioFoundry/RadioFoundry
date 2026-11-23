// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stored keys as one list with their check values (codeplug/key_store.h, docs/spec/key-check.md 4)
#include "codeplug/key_store.h"
#if KEY_STORE	// the whole file
#include <string.h>
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_records.h"
#include "settings/settings.h"
#include "support/hkdf.h"
#if CONFIG_DMR_PRIVACY
#include "dmr/dmrPrivacy.h"
#include "radio/radio.h"
#include "hal_time.h"
#endif

#define TRAFFIC_RECORDS	0x0400u		// the family's keys, 0x1900-0x1CFF (channel-modes.md 3)
#define SYSTEMS		0xFFu		// system records 0..254; 0xFF is CODEPLUG_SYSTEM_NONE
#define IDENTITY_MAX	32u		// the longest identity key_store_identity reads (a payload carries 8)

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
	codeplug_system_t s[4];
} sys_cache_t;

static bool cached_system(sys_cache_t *c, uint8_t id, codeplug_system_t *out)
{
	for (uint8_t i = 0; i < c->n; i++) {
		if (c->id[i] == id) {
			*out = c->s[i];
			return c->ok[i];
		}
	}
	uint8_t block[CODEPLUG_SYS_HDR];
	memset(out, 0, sizeof *out);	// the name's tail too: an entry copies it whole
	bool ok = codeplug_system(id, out, block, 0) >= 0;
	uint8_t slot = c->n < 4u ? c->n++ : (uint8_t)(id & 3u);
	c->id[slot] = id;
	c->ok[slot] = ok;
	c->s[slot] = *out;
	return ok;
}
#endif

#if CODEPLUG_MODE_RECORDS
// Traffic key entry n, in the area's order, counting only records whose system exists: as privacy_entry
static bool traffic_entry(uint16_t n, uint16_t want_record, key_entry_t *e, uint8_t *key, uint16_t *count)
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
		codeplug_system_t s;
		if (len <= (int)CODEPLUG_TRAFFIC_HDR || !cached_system(&sc, v[0], &s))
			continue;
		if (want_record != 0 ? k != want_record : seen++ != n)
			continue;
		memset(e, 0, sizeof *e);
		e->cls = KCV_CLASS_TRAFFIC;
		e->type = s.type;
		e->system = v[0];
		e->alg = v[1];
		e->key_id = (uint16_t)(v[2] | v[3] << 8);
		e->flags = v[4];
		e->record = k;
		e->len = (uint16_t)(len - (int)CODEPLUG_TRAFFIC_HDR);
		memcpy(e->name, s.name, sizeof e->name);
		// A key longer than the walk copied has no check value here (none in this tree is)
		e->kcv_ok = e->len <= KEY_STORE_MAX_LEN &&
			kcv_traffic(s.type, e->alg, &v[CODEPLUG_TRAFFIC_HDR], e->len, e->kcv);
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
	if (traffic_entry(i, 0, e, key, &n))
		return true;
#endif
	(void)n;
	return false;
}


bool key_store_get_record(uint16_t record, key_entry_t *e, uint8_t *key)
{
#if CODEPLUG_MODE_RECORDS
	uint16_t n = 0;
	return record != 0 && traffic_entry(0, record, e, key, &n);
#else
	(void)record; (void)e; (void)key;
	return false;
#endif
}

uint16_t key_store_window(uint16_t first, key_entry_t *out, uint16_t max, uint16_t *total)
{
	uint16_t filled = 0, at = 0;
#if CONFIG_DMR_PRIVACY
	uint16_t np = 0;
	key_entry_t pe;
	(void)privacy_entry(0xFFFFu, &pe, NULL, &np);
	for (uint16_t i = first; i < np && filled < max; i++)
		if (privacy_entry(i, &out[filled], NULL, &at))
			filled++;
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
		codeplug_system_t s;
		if (len <= (int)CODEPLUG_TRAFFIC_HDR || !cached_system(&sc, v[0], &s))
			continue;
		uint16_t n = at++;
		if (n < first || filled >= max)
			continue;
		key_entry_t *e = &out[filled++];
		memset(e, 0, sizeof *e);
		e->cls = KCV_CLASS_TRAFFIC;
		e->type = s.type;
		e->system = v[0];
		e->alg = v[1];
		e->key_id = (uint16_t)(v[2] | v[3] << 8);
		e->flags = v[4];
		e->record = k;
		e->len = (uint16_t)(len - (int)CODEPLUG_TRAFFIC_HDR);
		memcpy(e->name, s.name, sizeof e->name);
		e->kcv_ok = e->len <= KEY_STORE_MAX_LEN &&
			kcv_traffic(s.type, e->alg, &v[CODEPLUG_TRAFFIC_HDR], e->len, e->kcv);
	}
	crypto_wipe(v, sizeof v);
#endif
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
	(void)traffic_entry(0xFFFFu, 0, &e, NULL, &n);
	total = (uint16_t)(total + n);
#endif
	(void)e;
	(void)n;
	return total;
}

int key_store_identity(uint8_t system, uint8_t *out, uint8_t cap)
{
#if CODEPLUG_MODE_RECORDS
	codeplug_system_t s;
	uint8_t block[CODEPLUG_SYS_HDR + IDENTITY_MAX];
	int len = codeplug_system(system, &s, block, IDENTITY_MAX);
	// An identity the block doesn't hold whole names nothing; one past what we read is past any payload's 8 too
	if (len < 0 || s.id_len > cap || s.id_len > IDENTITY_MAX || (int)s.id_len > len)
		return -1;
	memcpy(out, block, s.id_len);
	return s.id_len;
#else
	(void)system;
	(void)out;
	(void)cap;
	return -1;
#endif
}

#if CONFIG_DMR_PRIVACY
static uint8_t put_privacy(const key_put_t *p, uint8_t kcv[KCV_LEN])
{
	if (p->slot < 1 || p->slot > SETTINGS_PRIVACY_KEYS || p->key_len != 4u || p->key == NULL)
		return KEY_PUT_BAD;
	uint32_t value = (uint32_t)p->key[0] << 24 | (uint32_t)p->key[1] << 16 | (uint32_t)p->key[2] << 8 | p->key[3];
	// dmrPrivacyKeyIsValid sees 16 bits: a wider value would pass as its low half
	if (p->type == DMR_PRIVACY_OFF || p->type >= DMR_PRIVACY_NUM_TYPES || value > 0xFFFFu ||
	    !dmrPrivacyKeyIsValid((dmrPrivacyType_t)p->type, (uint16_t)value)) {
		crypto_wipe(&value, sizeof value);
		return KEY_PUT_BAD;
	}
	settings_t *st = settings_begin();
	st->privacy_type[p->slot - 1u] = p->type;
	st->privacy_key[p->slot - 1u] = value;
	// The radio core keeps the key in use in slot 1 and sets privacy_active to 1 when it saves (radio_sm.c), so a
	// record not yet saved since privacy went on (0) is using slot 1 as well
	uint8_t active = st->privacy_active != 0 ? st->privacy_active : 1u;
	settings_end(hal_millis());
	if (p->slot == active) {
		// Off stays off: the new key is loaded when privacy is turned on (ui_svc_privacy reads the slot)
		if (radio_dmr_privacy_on())
			radio_req_dmr_privacy(p->type, (uint16_t)value);
	}
	(void)kcv_privacy(p->type, value, kcv);
	crypto_wipe(&value, sizeof value);
	return KEY_PUT_STORED;
}
#endif

#if CODEPLUG_MODE_RECORDS
// The system p's key goes to (key-fill.md 8): its number, or -1 - KEY_PUT_NO_SYSTEM / KEY_PUT_AMBIGUOUS
static int put_system(const key_put_t *p, uint8_t *sys_type)
{
	codeplug_kv_cursor_t c;
	uint16_t k;
	int found = -1;
	// Keys only: codeplug_system reads the few systems present, not 255 checks of the whole area
	codeplug_kv_begin(&c, CODEPLUG_KEY_SYSTEM, (uint16_t)(CODEPLUG_KEY_SYSTEM + SYSTEMS - 1u));
	while (codeplug_kv_next(&c, &k, NULL, 0) >= 0) {
		codeplug_system_t s;
		uint8_t block[CODEPLUG_SYS_HDR + sizeof p->identity];
		uint8_t n = (uint8_t)(k - CODEPLUG_KEY_SYSTEM);
		int len = codeplug_system(n, &s, block, sizeof p->identity);
		if (len < 0 || s.type != p->type)
			continue;
		if (p->id_len != 0) {
			if (s.id_len != p->id_len || len < (int)p->id_len || memcmp(block, p->identity, p->id_len) != 0)
				continue;
		} else if (found >= 0) {
			return -1 - KEY_PUT_AMBIGUOUS;	// an empty identity names a type with one system only
		}
		*sys_type = s.type;
		found = n;
		if (p->id_len != 0)
			break;		// a system's identity is its own: the first match is the one
	}
	return found >= 0 ? found : -1 - KEY_PUT_NO_SYSTEM;
}

static uint8_t put_traffic(const key_put_t *p, uint8_t kcv[KCV_LEN])
{
	if (p->key_len < 1u || p->key_len > KEY_STORE_MAX_LEN || p->id_len > sizeof p->identity || p->key == NULL)
		return KEY_PUT_BAD;
	uint8_t sys_type = 0;
	int system = put_system(p, &sys_type);
	if (system < 0)
		return (uint8_t)(-1 - system);
	// Another firmware's codeplug is left as it is (codeplug-native.md 2)
	if (codeplug_foreign())
		return KEY_PUT_WRITE_FAILED;

	// The record to write: one of this system with the same algorithm and key ID, else the first key not in use
	uint8_t used[TRAFFIC_RECORDS / 8u];
	uint8_t h[CODEPLUG_TRAFFIC_HDR];
	codeplug_kv_cursor_t c;
	uint16_t k, at = 0;
	bool same = false;
	int len;
	memset(used, 0, sizeof used);
	codeplug_kv_begin(&c, CODEPLUG_KEY_TRAFFIC, (uint16_t)(CODEPLUG_KEY_TRAFFIC + TRAFFIC_RECORDS - 1u));
	while ((len = codeplug_kv_next(&c, &k, h, sizeof h)) >= 0) {
		uint16_t n = (uint16_t)(k - CODEPLUG_KEY_TRAFFIC);
		used[n / 8u] |= (uint8_t)(1u << (n % 8u));
		if (len >= (int)CODEPLUG_TRAFFIC_HDR && h[0] == (uint8_t)system && h[1] == p->alg &&
		    (uint16_t)(h[2] | h[3] << 8) == p->key_id) {
			at = k;
			same = true;
			break;
		}
	}
	if (!same) {
		uint16_t n = 0;
		while (n < TRAFFIC_RECORDS && (used[n / 8u] & (1u << (n % 8u))) != 0)
			n++;
		if (n == TRAFFIC_RECORDS)
			return KEY_PUT_WRITE_FAILED;	// the family is full
		at = (uint16_t)(CODEPLUG_KEY_TRAFFIC + n);
	}

	uint8_t v[CODEPLUG_TRAFFIC_HDR + KEY_STORE_MAX_LEN];
	v[0] = (uint8_t)system;
	v[1] = p->alg;
	v[2] = (uint8_t)p->key_id;
	v[3] = (uint8_t)(p->key_id >> 8);
	v[4] = p->flags;
	memcpy(&v[CODEPLUG_TRAFFIC_HDR], p->key, p->key_len);
	bool ok = codeplug_kv_set(at, v, (uint16_t)(CODEPLUG_TRAFFIC_HDR + p->key_len));
	// A check value this build can't compute (AES without AES) goes back as zeros: the key is stored all the same
	if (ok && !kcv_traffic(sys_type, p->alg, &v[CODEPLUG_TRAFFIC_HDR], p->key_len, kcv))
		memset(kcv, 0, KCV_LEN);
	crypto_wipe(v, sizeof v);
	return ok ? KEY_PUT_STORED : KEY_PUT_WRITE_FAILED;
}
#endif

uint8_t key_store_put(const key_put_t *p, uint8_t kcv[KCV_LEN])
{
	memset(kcv, 0, KCV_LEN);	// DONE carries zeros when nothing was stored (key-fill.md 4.4)
	switch (p->cls) {
	case KCV_CLASS_PRIVACY:
#if CONFIG_DMR_PRIVACY
		return put_privacy(p, kcv);
#else
		return KEY_PUT_UNSUPPORTED;
#endif
	case KCV_CLASS_TRAFFIC:
#if CODEPLUG_MODE_RECORDS
		return put_traffic(p, kcv);
#else
		return KEY_PUT_UNSUPPORTED;
#endif
	default:
		return KEY_PUT_UNSUPPORTED;
	}
}

#endif	// KEY_STORE
