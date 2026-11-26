// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The air mode contact table (docs/spec/air-contacts.md): the names of P25 talk groups and units, apart from the DMR
// contacts, whose records carry no mode. Every call takes storage_lock, so any task may call
#ifndef RFOUNDRY_CODEPLUG_AIR_CONTACTS_H
#define RFOUNDRY_CODEPLUG_AIR_CONTACTS_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "codeplug/codeplug.h"	// CODEPLUG_NAME_LEN

#define CODEPLUG_AIR_CONTACTS_ADDR	0xC000u	// in the EEPROM image: two STORAGE_SECTORs, to 0xDFFF
#define CODEPLUG_AIR_CONTACTS_LEN	0x2000u
#define CODEPLUG_AIR_CONTACT_SIZE	24u
#define CODEPLUG_AIR_CONTACTS_MAX	341u	// slots 1-341; the last 8 bytes are unused
#define CODEPLUG_AIR_CONTACT_IDENT	5u	// the bytes of a slot the contact lists' table check covers: mode, type, ID

// Call types (air-contacts.md 3): the target records' order, not the contact record's
enum { AIR_CONTACT_UNIT, AIR_CONTACT_GROUP, AIR_CONTACT_ALL };

typedef struct {
	uint8_t type;		// AIR_CONTACT_*
	uint32_t id;		// for all call, the mode's all-call address (P25: talk group 0xFFFF)
	char name[CODEPLUG_NAME_LEN + 1];
} air_contact_t;

// *crc continued (storage_crc32) over the first CODEPLUG_AIR_CONTACT_IDENT bytes of every slot in order, empty ones too:
// the contact lists' table check (docs/spec/contact-lists.md 3.3). Available in every build. False, *crc unchanged, when
// the table can't be read
bool codeplug_air_contacts_check(uint32_t *crc);

#if CONFIG_AIR
// Slot index (1-based) when it is a valid entry of mode (3.1); false when empty, of another mode, invalid or unreadable
bool codeplug_air_contact(uint16_t index, uint8_t mode, air_contact_t *out);
// The first valid entry of mode with this ID that has a name (3.2): group true finds a group or the all call, false a
// unit. Reads up to the whole table (8 KB): cache the answer
bool codeplug_air_contact_find(uint8_t mode, uint32_t id, bool group, air_contact_t *out);
// The same among the entries pred accepts (NULL accepts all); it gets the slot (1-based) of each one that matches
typedef bool (*codeplug_air_pred_fn)(uint16_t slot, void *ctx);
bool codeplug_air_contact_find_if(uint8_t mode, uint32_t id, bool group, codeplug_air_pred_fn pred, void *pred_ctx,
				  air_contact_t *out);
#endif

#endif
