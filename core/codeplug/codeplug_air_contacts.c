// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Reader of the air mode contact table (codeplug_air_contacts.h, docs/spec/air-contacts.md). Reads only: the Codeplug
// Editor writes the table as image bytes
#include <string.h>
#include "codeplug/codeplug_air_contacts.h"
#include "codeplug/codeplug_modes.h"
#include "codeplug/codeplug_records.h"
#include "board.h"
#include "storage/storage.h"

#if CONFIG_AIR

#define PER_READ	8u	// entries per flash read in a scan: 192 bytes of stack

// None while the open-firmware area holds another firmware's codeplug (codeplug_foreign), as the settings area: its
// bytes there aren't ours. Under storage_lock
static bool table_addr(uint32_t *addr)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || codeplug_foreign() || board.flash->size == 0 ||
	    m->openfw_eeprom + CODEPLUG_AIR_CONTACTS_ADDR + CODEPLUG_AIR_CONTACTS_LEN > board.flash->size) {
		return false;
	}
	*addr = m->openfw_eeprom + CODEPLUG_AIR_CONTACTS_ADDR;
	return true;
}

// The IDs a mode's call type takes (3); a mode this build doesn't know takes none, so its entries are skipped
static bool id_ok(uint8_t mode, uint8_t type, uint32_t id)
{
	switch (mode) {
	case CODEPLUG_KIND_P25:
		return type == AIR_CONTACT_ALL || (type == AIR_CONTACT_GROUP && id >= 1u && id <= 0xFFFFu) ||
		       (type == AIR_CONTACT_UNIT && id >= 1u && id <= 0xFFFFFEu);
	default:
		return false;
	}
}

static uint32_t all_call_id(uint8_t mode)
{
	return mode == CODEPLUG_KIND_P25 ? 0xFFFFu : 0u;
}

// An entry's bytes as a valid entry of mode (3.1)
static bool decode(const uint8_t *r, uint8_t mode, air_contact_t *out)
{
	out->scoped = r[0] == mode && (r[1] & AIR_CONTACT_SYSTEM) != 0;
	out->system = out->scoped ? r[5] : CODEPLUG_SYSTEM_NONE;
	out->system_check = out->scoped ? (uint16_t)(r[6] | (r[7] << 8)) : 0;
	uint8_t type = r[1] & 0x03u;
	uint32_t id = (uint32_t)r[2] | ((uint32_t)r[3] << 8) | ((uint32_t)r[4] << 16);
	if (r[0] != mode || type > AIR_CONTACT_ALL || !id_ok(mode, type, id)) {
		return false;
	}
	out->type = type;
	out->id = type == AIR_CONTACT_ALL ? all_call_id(mode) : id;
	unsigned n = 0;
	while (n < CODEPLUG_NAME_LEN && r[8 + n] != 0x00 && r[8 + n] != 0xFF) {
		out->name[n] = (char)r[8 + n];
		n++;
	}
	out->name[n] = '\0';
	return true;
}

static bool scope_ok(uint8_t mode, uint8_t system, const air_contact_t *c)
{
	uint16_t check;
	return !c->scoped || (mode == CODEPLUG_KIND_P25 && system != CODEPLUG_SYSTEM_NONE && c->system == system &&
		codeplug_system_identity_check(system, CODEPLUG_SYSTYPE_P25_TRUNK, &check) && check == c->system_check);
}

bool codeplug_air_contact(uint16_t index, uint8_t mode, uint8_t system, air_contact_t *out)
{
	uint8_t r[CODEPLUG_AIR_CONTACT_SIZE];
	uint32_t a;
	memset(out, 0, sizeof *out);
	if (index < 1u || index > CODEPLUG_AIR_CONTACTS_MAX) {
		return false;
	}
	storage_lock();
	bool ok = table_addr(&a) && board.flash->read(a + (index - 1u) * CODEPLUG_AIR_CONTACT_SIZE, r, sizeof(r));
	storage_unlock();
	return ok && decode(r, mode, out) && scope_ok(mode, system, out);
}

bool codeplug_air_contact_find(uint8_t mode, uint32_t id, bool group, uint8_t system, air_contact_t *out)
{
	uint8_t blk[PER_READ * CODEPLUG_AIR_CONTACT_SIZE];
	air_contact_t c;
	bool fallback = false;
	// The lock is taken per read, so the radio task's own reads wait for one block at most
	for (unsigned first = 0; first < CODEPLUG_AIR_CONTACTS_MAX; first += PER_READ) {
		unsigned n = CODEPLUG_AIR_CONTACTS_MAX - first < PER_READ ? CODEPLUG_AIR_CONTACTS_MAX - first : PER_READ;
		uint32_t a;
		storage_lock();
		bool ok = table_addr(&a) && board.flash->read(a + first * CODEPLUG_AIR_CONTACT_SIZE, blk,
							     n * CODEPLUG_AIR_CONTACT_SIZE);
		storage_unlock();
		if (!ok) {
			return false;
		}
		for (unsigned i = 0; i < n; i++) {
			if (!decode(&blk[i * CODEPLUG_AIR_CONTACT_SIZE], mode, &c) || c.id != id || c.name[0] == '\0' ||
			    (c.type != AIR_CONTACT_UNIT) != group || !scope_ok(mode, system, &c)) {
				continue;
			}
			if (c.scoped || system == CODEPLUG_SYSTEM_NONE) {
				*out = c;
				return true;
			}
			if (!fallback) {
				*out = c;
				fallback = true;
			}
		}
	}
	return fallback;
}

#endif
