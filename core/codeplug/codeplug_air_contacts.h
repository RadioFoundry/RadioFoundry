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
#include "codeplug/codeplug_tag.h"	// CODEPLUG_SYSTEM_NONE

#define CODEPLUG_AIR_CONTACTS_ADDR	0xC000u	// in the EEPROM image: two STORAGE_SECTORs, to 0xDFFF
#define CODEPLUG_AIR_CONTACTS_LEN	0x2000u
#define CODEPLUG_AIR_CONTACT_SIZE	24u
#define CODEPLUG_AIR_CONTACTS_MAX	341u	// slots 1-341; the last 8 bytes are unused
#define AIR_CONTACT_SYSTEM	0x04u	// flags bit 2: bytes 5-7 qualify this row by System identity

// Call types (air-contacts.md 3): the target records' order, not the contact record's
enum { AIR_CONTACT_UNIT, AIR_CONTACT_GROUP, AIR_CONTACT_ALL };

typedef struct {
	uint8_t type;		// AIR_CONTACT_*
	uint32_t id;		// for all call, the mode's all-call address (P25: talk group 0xFFFF)
	char name[CODEPLUG_NAME_LEN + 1];
	bool scoped;
	uint8_t system;		// zero-based System, CODEPLUG_SYSTEM_NONE is an invalid scoped reference
	uint16_t system_check;	// the System owner's identity-only check
} air_contact_t;

#if CONFIG_AIR
// Slot index (1-based) when it is a valid entry of mode (3.1); false when empty, of another mode, invalid or unreadable
// system NONE reads only unscoped rows. An invalid scoped row returns false with out->scoped still set, so an
// explicit invalid target cannot become the legacy default. Other failures leave an empty output.
bool codeplug_air_contact(uint16_t index, uint8_t mode, uint8_t system, air_contact_t *out);
// The first valid entry of mode with this ID that has a name (3.2): group true finds a group or the all call, false a
// unit. Reads up to the whole table (8 KB): cache the answer
// Prefer an exact valid System scope; a legacy unscoped name is the fallback, never another System's name.
bool codeplug_air_contact_find(uint8_t mode, uint32_t id, bool group, uint8_t system, air_contact_t *out);
#endif

#endif
