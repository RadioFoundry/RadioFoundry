// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's stored keys as one list (docs/spec/key-check.md 4): the privacy key slots that hold a valid key
// (settings-store.md 3.1), then the traffic key records whose system exists (channel-modes.md 5.4), each with its key
// check value. A reader: Settings > Privacy > Stored keys shows the list. Built with CONFIG_KEY_CHECK when a privacy
// scheme or a mode with traffic keys is (KEY_STORE)
#ifndef RFOUNDRY_KEY_STORE_H
#define RFOUNDRY_KEY_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_modes.h"
#include "support/kcv.h"

#define KEY_STORE	(CONFIG_KEY_CHECK && (CONFIG_DMR_PRIVACY || CODEPLUG_MODE_RECORDS))
#define KEY_STORE_MAX_LEN	32u	// the longest key carried: AES-256. A longer traffic key is listed, not carried

typedef struct key_entry_s {
	uint8_t cls;		// KCV_CLASS_PRIVACY or KCV_CLASS_TRAFFIC
	uint8_t slot;		// privacy: the slot, 1..SETTINGS_PRIVACY_KEYS
	uint8_t type;		// privacy: dmrPrivacyType_t; traffic: the system's type
	uint8_t system;		// traffic: the system's number
	uint8_t alg;		// traffic: the algorithm id (the system type's numbering)
	uint8_t flags;		// traffic: the record's flags octet
	uint16_t key_id;	// traffic: the key ID
	uint16_t record;	// traffic: the record's area key, CODEPLUG_KEY_TRAFFIC + n
	uint16_t len;		// the key's octets (privacy: 4, its value big endian)
	bool kcv_ok;		// kcv holds the check value (false: its method isn't in this build)
	uint8_t kcv[KCV_LEN];
	char name[CODEPLUG_NAME_LEN + 1];	// traffic: the system's name; privacy: empty
} key_entry_t;

// A key to store (key fill, docs/spec/key-fill.md 8), as the payload carries it (4.3)
typedef struct {
	uint8_t cls;		// KCV_CLASS_PRIVACY or KCV_CLASS_TRAFFIC
	uint8_t type;		// privacy type, or system type
	uint8_t slot;		// privacy: 1..SETTINGS_PRIVACY_KEYS
	uint8_t alg;		// traffic
	uint16_t key_id;	// traffic
	uint8_t flags;		// traffic: the record's flags octet
	uint8_t id_len;		// traffic: the system's identity, 0..8 octets
	uint8_t identity[8];
	uint8_t key_len;	// privacy: 4 (the value, big endian); traffic: 1..KEY_STORE_MAX_LEN
	const uint8_t *key;
} key_put_t;
// key_store_put's results: key-fill.md 4.4's DONE results, the same numbers
enum { KEY_PUT_STORED = 0, KEY_PUT_NO_SYSTEM = 1, KEY_PUT_AMBIGUOUS = 2, KEY_PUT_UNSUPPORTED = 3, KEY_PUT_BAD = 4,
       KEY_PUT_WRITE_FAILED = 5 };

#if KEY_STORE
uint16_t key_store_count(void);
// The traffic key in record (key_entry_t.record), as key_store_get: one walk, not one per entry before it
bool key_store_get_record(uint16_t record, key_entry_t *e, uint8_t *key);
// Entries first .. first + max - 1 into out (as key_store_get without the key octets) and the count into *total, from
// one walk of the codeplug area: how many it filled. A list that draws several rows reads them this way, since every
// walk (and every system looked up) checks the whole area first
uint16_t key_store_window(uint16_t first, key_entry_t *out, uint16_t max, uint16_t *total);
// Entry i in *e; with key not NULL its octets too (at most KEY_STORE_MAX_LEN copied). False past the end, or when the
// list changed under the walk
bool key_store_get(uint16_t i, key_entry_t *e, uint8_t *key);
// Traffic entry e's system identity (channel-modes.md 7.1: the first id_len octets of its block) into out, at most cap
// octets: its length, -1 when the system is gone or its identity is longer than cap
int key_store_identity(uint8_t system, uint8_t *out, uint8_t cap);
// Store p (key-fill.md 8): a privacy slot through the settings, a traffic key into the receiver's matching system
// (replacing a record of that system, algorithm and key ID, else the first free one). Writes flash: the housekeeping
// task's (a storage job). KEY_PUT_*; with KEY_PUT_STORED the stored key's check value in kcv
uint8_t key_store_put(const key_put_t *p, uint8_t kcv[KCV_LEN]);
#else
static inline uint16_t key_store_count(void) { return 0; }
static inline uint16_t key_store_window(uint16_t first, key_entry_t *out, uint16_t max, uint16_t *total)
{
	(void)first; (void)out; (void)max;
	*total = 0;
	return 0;
}
static inline bool key_store_get(uint16_t i, key_entry_t *e, uint8_t *key) { (void)i; (void)e; (void)key; return false; }
#endif

#endif
