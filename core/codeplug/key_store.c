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
// Traffic key entry n, in the area's order, counting only records whose system exists: as privacy_entry
static bool traffic_entry(uint16_t n, key_entry_t *e, uint8_t *key, uint16_t *count)
{
	uint8_t v[CODEPLUG_TRAFFIC_HDR + KEY_STORE_MAX_LEN];
	codeplug_kv_cursor_t c;
	uint16_t k, seen = 0;
	int len;
	bool found = false;
	codeplug_kv_begin(&c, CODEPLUG_KEY_TRAFFIC, (uint16_t)(CODEPLUG_KEY_TRAFFIC + TRAFFIC_RECORDS - 1u));
	while (!found && (len = codeplug_kv_next(&c, &k, v, sizeof v)) >= 0) {
		codeplug_system_t s;
		uint8_t block[CODEPLUG_SYS_HDR];
		if (len <= (int)CODEPLUG_TRAFFIC_HDR || codeplug_system(v[0], &s, block, 0) < 0)
			continue;
		if (seen++ != n)
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
	if (traffic_entry(i, e, key, &n))
		return true;
#endif
	(void)n;
	return false;
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
