// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stored scan lists (docs/spec/scan-list.md): a named list of channel numbers from any zone, one record of the codeplug
// settings area (docs/spec/codeplug-settings.md) under key CODEPLUG_KEY_SCAN_LIST + slot. A change is the area's
// journaled sector rewrite (codeplug_kv_write), so a power cut leaves the old area or the new one. The records are read
// from flash when asked for: no RAM copy. Writes block on the flash; a task that must not wait (the UI) hands them to
// the housekeeping task (storage_job_post).
//
// A list is a zone_t (codeplug.h): the zone helpers in zone_edit.h that change one in RAM serve a scan list too. A list
// is named by its slot, the way a zone is by its index: deleting one leaves a free slot, and the lists after it keep
// their numbers. A channel number the codeplug no longer holds stays in the list, and the scan skips it.
#ifndef SCAN_LIST_H
#define SCAN_LIST_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

#define SCAN_LIST_SLOTS		16u			// lists the store holds; the key family reserves 256
#define SCAN_LIST_MAX		CODEPLUG_ZONE_MAX	// channels in a list
#define SCAN_LIST_HEAD		18u			// a record's fixed head: flags, count, the 16 byte name
#define SCAN_LIST_REC_MAX	(SCAN_LIST_HEAD + 2u * SCAN_LIST_MAX)

typedef zone_t scan_list_t;

// Whether lists can be stored: the codeplug settings area exists on this board and isn't a later firmware's
bool scan_list_usable(void);
// Slots the store has: SCAN_LIST_SLOTS, or 0 when it isn't usable
uint16_t scan_list_slots(void);
// Slot i's list; false when it is free or not a valid record
bool scan_list_read(uint16_t slot, scan_list_t *out);
// Lists stored
uint16_t scan_list_count(void);
// Store l in slot, replacing what is there. False for a slot outside the store, a list that isn't valid (no name, more
// than SCAN_LIST_MAX channels, channel number 0 or one twice), or when the area can't take it (full, not usable, the
// shared sector buffer in use) or the flash fails
bool scan_list_write(uint16_t slot, const scan_list_t *l);
// Store l in the first free slot: the slot, or -1 when full or when scan_list_write would refuse it
int scan_list_add(const scan_list_t *l);
// Free a slot; false when it holds no list or the write fails
bool scan_list_delete(uint16_t slot);

// Changes whenever the lists may read differently: every change written here, and every change of the settings area
// (a CPS or TFTP commit): a reader that keeps what it read knows to read again
uint32_t scan_list_generation(void);

// A list the store accepts
bool scan_list_valid(const scan_list_t *l);
// A record's bytes and back (little endian), for the store and the tests. Encode returns the length (at most
// SCAN_LIST_REC_MAX), 0 for a list that isn't valid; decode false for a record that isn't one
unsigned scan_list_encode(const scan_list_t *l, uint8_t out[SCAN_LIST_REC_MAX]);
bool scan_list_decode(const uint8_t *in, unsigned len, scan_list_t *out);

#endif
