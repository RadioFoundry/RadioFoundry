// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Named contact lists (docs/spec/contact-lists.md): which lists name a received call, from the keys the settings area's
// continuation holds, and the one entry point that names a call through a selection of lists. Every call takes
// storage_lock, so any task may call. What is read is cached until codeplug_kv_serial() moves
#ifndef RFOUNDRY_CODEPLUG_CONTACT_LISTS_H
#define RFOUNDRY_CODEPLUG_CONTACT_LISTS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_tag.h"	// CODEPLUG_SYSTEM_NONE

#if CONFIG_DMR

#define CONTACT_LIST_MAX	32u	// lists 0-31
#define CONTACT_LIST_NAME_LEN	16u	// bytes of a list name

// List bytes, for membership and selection (contact-lists.md 1)
#define CONTACT_LIST_NONE	0xFFu	// no list
#define CONTACT_LIST_IDDB	0xFEu	// the DMR ID database: a selection only, never a membership

// The mode of a call named by codeplug_name_lookup: DMR, or the air mode (a CODEPLUG_KIND_*) whose contact table holds
// the names
#define CODEPLUG_NAME_DMR	0u

#if CONFIG_CONTACT_LISTS
// The list the DMR contact in slot index (1-based) is in: 0-31, or CONTACT_LIST_NONE when it is in none, the slot is
// out of range, there is no membership key or membership is void (contact-lists.md 3.3)
uint8_t codeplug_contact_list_of(uint16_t index);
// The same for the air mode contact table's slot (1-based)
uint8_t codeplug_air_contact_list_of(uint16_t slot);

// System sys's selection: out[0] the primary list byte, out[1] the secondary. Each is 0-31, CONTACT_LIST_IDDB or
// CONTACT_LIST_NONE (also for a system that selects nothing). Not checked against the lists that exist
void codeplug_system_lists(uint8_t sys, uint8_t out[2]);

// The network of channel ch (1-based): the system number its channel network byte names when that is a system record of
// the Conventional type, else CODEPLUG_SYSTEM_NONE. A channel with a tag that names a system doesn't use it: the caller
// takes the tag's system first (contact-lists.md 5.1)
uint8_t codeplug_channel_system(uint16_t ch);

// List n's name (CONTACT_LIST_NAME_LEN bytes at most): false when the list doesn't exist (no name, an empty one, one
// over 16 bytes or with a control character). The radio edits no list name: the Codeplug Editor does
bool codeplug_list_name(uint8_t n, char out[CONTACT_LIST_NAME_LEN + 1]);

// A radio-side edit of contacts keeps membership and the table check consistent (contact-lists.md 4). Around the writes of
// the contact records: begin before the first, end after the last. A write that fails needs no end
typedef struct {
	uint8_t state;		// what membership was before the edit: nothing, live or void
	bool has_check;		// the table check key as begin found it (4 bytes), and its value
	uint32_t check;
} codeplug_contact_edit_t;
void codeplug_contact_lists_begin(codeplug_contact_edit_t *e);
// After the edit of n DMR contact slots (1-based), each a deleted or new contact that is in no list: their membership
// bytes become 0xFF, and the check is made again from the tables as they are now, in one rewrite. Void membership is
// left alone, and so is a codeplug with no membership (there is nothing to keep consistent). False when the keys could
// not be written, or when the check key is not what begin found (another writer got in between, which this check
// would vouch for): the contact edit stands, and membership is void until the Codeplug Editor assigns again
bool codeplug_contact_lists_end(const codeplug_contact_edit_t *e, const uint16_t *slots, unsigned n);

#endif

// Name a received call through the selection sel (primary, secondary; a CONTACT_LIST_* byte each), searched in order
// (contact-lists.md 5.2): a list number finds the first contact of the call's table that is a member of it with this ID
// and call type, CONTACT_LIST_IDDB asks the DMR ID database (a DMR private ID only), and a byte that is none or names
// a list that doesn't exist is skipped. mode is CODEPLUG_NAME_DMR or an air mode; group is a talk group (or all call)
// rather than a private/unit ID. The first name found goes into out (len bytes with the terminator, cut to fit) and
// the result is true; false (out empty) when nothing names the call or sel is NULL.
// Block reads and static buffers only: no large stack buffers. It scans the table (up to 24 KB of flash for DMR): cache
// the answer, per selection. With CONFIG_CONTACT_LISTS off, or in a codeplug that holds no contact list key at all (not
// migrated, contact-lists.md 6), sel is ignored and the answer is the first contact with the ID and call type, then the
// ID database, as before the lists
bool codeplug_name_lookup(const uint8_t sel[2], uint8_t mode, uint32_t id, bool group, char *out, size_t len);

#endif

#endif
