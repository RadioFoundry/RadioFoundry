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

// One row of a page (key_store_window): what a list row draws, 8 octets against key_entry_t's 34. A page that keeps
// rows for a screen most radios never open costs RAM on every radio (DM-1701 RAM is nearly full)
typedef struct key_row_s {
	uint8_t cls : 7;	// KCV_CLASS_PRIVACY or KCV_CLASS_TRAFFIC
	uint8_t kcv_ok : 1;	// kcv holds the check value
	uint8_t num;		// privacy: the slot; traffic: the system's number
	uint8_t type;		// privacy: dmrPrivacyType_t; traffic: the system's type
	uint8_t kcv[KCV_LEN];
	uint16_t key_id;	// traffic: the key ID
} key_row_t;

// The names of the first KEY_NAMES systems a page's rows use, the page's own copy: a few systems hold all the keys.
// A row whose system has no name here is drawn by the system's type. Names are CODEPLUG_NAME_LEN octets, not
// terminated when full
#define KEY_NAMES	2u
typedef struct key_names_s {
	uint8_t n;
	uint8_t system[KEY_NAMES];
	char name[KEY_NAMES][CODEPLUG_NAME_LEN];
} key_names_t;

#if KEY_STORE
uint16_t key_store_count(void);
// Rows first .. first + max - 1 into out (key_store_get's entries, cut to a row), the names of their first systems
// into *names, and the count into *total, from one walk of the codeplug area: how many it filled. A list that draws
// several rows reads them this way, since every walk (and every system looked up) checks the whole area first
uint16_t key_store_window(uint16_t first, key_row_t *out, uint16_t max, uint16_t *total, key_names_t *names);
// System's name from *names into out (terminated); false when the page kept no name for it
bool key_names_get(const key_names_t *names, uint8_t system, char out[CODEPLUG_NAME_LEN + 1]);
// Entry i in *e; with key not NULL its octets too (at most KEY_STORE_MAX_LEN copied). False past the end, or when the
// list changed under the walk
bool key_store_get(uint16_t i, key_entry_t *e, uint8_t *key);
#else
static inline uint16_t key_store_count(void) { return 0; }
static inline uint16_t key_store_window(uint16_t first, key_row_t *out, uint16_t max, uint16_t *total,
					 key_names_t *names)
{
	(void)first; (void)out; (void)max;
	names->n = 0;
	*total = 0;
	return 0;
}
static inline bool key_names_get(const key_names_t *names, uint8_t system, char out[CODEPLUG_NAME_LEN + 1])
{
	(void)names; (void)system; (void)out;
	return false;
}
static inline bool key_store_get(uint16_t i, key_entry_t *e, uint8_t *key) { (void)i; (void)e; (void)key; return false; }
#endif

#endif
