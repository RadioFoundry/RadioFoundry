// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Named contact lists (codeplug_contact_lists.h, docs/spec/contact-lists.md): the keys of the settings area's
// continuation read and written, the void rule, and the lookup that names a call through a selection of lists
#include <string.h>
#include "codeplug/codeplug_contact_lists.h"
#include "codeplug/codeplug_air_contacts.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_modes.h"
#include "codeplug/codeplug_records.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/dmr_iddb.h"
#include "storage/storage.h"

#if CONFIG_DMR

#if CONFIG_CONTACT_LISTS

#define CH_MAX		1024u	// channels 1-1024, the channel network key's bytes
#define EDIT_NONE	0u	// codeplug_contact_edit_t.state
#define EDIT_LIVE	1u
#define EDIT_VOID	2u
#define EDIT_SLOTS	8u	// membership bytes set in one rewrite (ops and values on the stack: 8 x 11 bytes)

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A list byte as membership: a list number, else none (0xFE is never a membership byte, contact-lists.md 1)
static uint8_t member_byte(uint8_t b)
{
	return b < CONTACT_LIST_MAX ? b : CONTACT_LIST_NONE;
}

// A list byte as a selection: a list number or the ID database, else none
static uint8_t select_byte(uint8_t b)
{
	return b < CONTACT_LIST_MAX || b == CONTACT_LIST_IDDB ? b : CONTACT_LIST_NONE;
}

// What was read of the keys for this state of them. The one serial that moves for every change of the keys or the codeplug
// (codeplug_kv_serial) drops it, as the contact tables' own edits do. Under storage_lock
static struct {
	uint8_t serial;
	bool valid;
	bool present;		// a membership key (DMR or air) is stored
	bool voided;		// ... and the check key doesn't match the tables (3.3)
} cache;

// The CRC-32 of the contact tables as stored (3.3): the DMR slots, then the air slots
static bool tables_check(uint32_t *crc)
{
	uint32_t c = 0;
	if (!codeplug_contacts_check(&c) || !codeplug_air_contacts_check(&c)) {
		return false;
	}
	*crc = c;
	return true;
}

// Read the keys again if they or the codeplug changed. Void when a membership key is stored and the check key is absent,
// not 4 bytes, or not the tables' check; tables that can't be read are void too (a name is never wrong). Under
// storage_lock
static void refresh(void)
{
	uint8_t serial = codeplug_kv_serial();
	if (cache.valid && cache.serial == serial) {
		return;
	}
	cache.serial = serial;
	cache.present = codeplug_kv_get(CODEPLUG_KEY_LIST_DMR, NULL, 0) >= 0 || codeplug_kv_get(CODEPLUG_KEY_LIST_AIR, NULL, 0) >= 0;
	cache.voided = false;
	if (cache.present) {
		uint8_t v[5];
		uint32_t have;
		cache.voided = codeplug_kv_get(CODEPLUG_KEY_LIST_CHECK, v, sizeof(v)) != 4 || !tables_check(&have) || have != get32(v);
	}
	cache.valid = true;
}

// The membership byte of slot (1-based) in the table of key, of count slots: none when there is no live membership
static uint8_t member_of(uint16_t key, uint16_t slot, uint16_t count)
{
	uint8_t b = CONTACT_LIST_NONE;
	if (slot < 1u || slot > count) {
		return CONTACT_LIST_NONE;
	}
	storage_lock();
	refresh();
	if (cache.present && !cache.voided) {
		codeplug_kv_read(key, (uint16_t)(slot - 1u), &b, 1);	// a value that ends before the slot: its 0xFF
	}
	storage_unlock();
	return member_byte(b);
}

uint8_t codeplug_contact_list_of(uint16_t index)
{
	return member_of(CODEPLUG_KEY_LIST_DMR, index, CODEPLUG_CONTACTS_MAX);
}

uint8_t codeplug_air_contact_list_of(uint16_t slot)
{
	return member_of(CODEPLUG_KEY_LIST_AIR, slot, CODEPLUG_AIR_CONTACTS_MAX);
}

void codeplug_system_lists(uint8_t sys, uint8_t out[2])
{
	uint8_t v[2] = { CONTACT_LIST_NONE, CONTACT_LIST_NONE };
	out[0] = out[1] = CONTACT_LIST_NONE;
	if (sys == 0xFFu) {
		return;
	}
	int n = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_SYS_LISTS + sys), v, sizeof(v));
	if (n >= 1) {
		out[0] = select_byte(v[0]);
	}
	if (n >= 2) {
		out[1] = select_byte(v[1]);
	}
}

// Whether system sys is a record of the Conventional type (channel-modes.md 7.5)
static bool is_network(uint8_t sys)
{
	uint8_t type = 0;
	int n = codeplug_kv_read((uint16_t)(CODEPLUG_KEY_SYSTEM + sys), 0, &type, 1);
	return sys != 0xFFu && n >= (int)CODEPLUG_SYS_HDR && type == CODEPLUG_SYSTYPE_CONVENTIONAL;
}

uint8_t codeplug_channel_system(uint16_t ch)
{
	uint8_t b = CODEPLUG_SYSTEM_NONE;
	if (ch < 1u || ch > CH_MAX) {
		return CODEPLUG_SYSTEM_NONE;
	}
	storage_lock();
	codeplug_kv_read(CODEPLUG_KEY_CH_NET, (uint16_t)(ch - 1u), &b, 1);
	b = is_network(b) ? b : CODEPLUG_SYSTEM_NONE;
	storage_unlock();
	return b;
}

bool codeplug_list_name(uint8_t n, char out[CONTACT_LIST_NAME_LEN + 1])
{
	uint8_t v[CONTACT_LIST_NAME_LEN + 1];
	if (n >= CONTACT_LIST_MAX) {
		return false;
	}
	int len = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_LIST_NAME + n), v, sizeof(v));
	if (len < 1 || len > (int)CONTACT_LIST_NAME_LEN) {
		return false;
	}
	for (int i = 0; i < len; i++) {
		if (v[i] < 0x20u) {
			return false;
		}
	}
	memcpy(out, v, (size_t)len);
	out[len] = '\0';
	return true;
}

bool codeplug_list_set_name(uint8_t n, const char *name)
{
	size_t len = name != NULL ? strlen(name) : 0;
	if (n >= CONTACT_LIST_MAX || len > CONTACT_LIST_NAME_LEN) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if ((uint8_t)name[i] < 0x20u) {
			return false;
		}
	}
	uint16_t key = (uint16_t)(CODEPLUG_KEY_LIST_NAME + n);
	return len == 0 ? codeplug_kv_remove(key) : codeplug_kv_set(key, name, (uint16_t)len);
}

void codeplug_contact_lists_begin(codeplug_contact_edit_t *e)
{
	storage_lock();
	refresh();
	e->state = !cache.present ? EDIT_NONE : cache.voided ? EDIT_VOID : EDIT_LIVE;
	storage_unlock();
}

bool codeplug_contact_lists_end(const codeplug_contact_edit_t *e, bool air, const uint16_t *slots, unsigned n, uint8_t list)
{
	uint16_t key = air ? CODEPLUG_KEY_LIST_AIR : CODEPLUG_KEY_LIST_DMR, count = air ? CODEPLUG_AIR_CONTACTS_MAX : CODEPLUG_CONTACTS_MAX;
	bool named = list < CONTACT_LIST_MAX;
	if (list >= CONTACT_LIST_MAX && list != CONTACT_LIST_NONE && list != CONTACT_LIST_KEEP) {
		return false;
	}
	if (e->state == EDIT_VOID || (e->state == EDIT_NONE && !named)) {
		return true;	// void: left as found; no membership and none named: nothing to keep consistent
	}
	uint32_t crc;
	bool ok = false;
	storage_lock();
	if (tables_check(&crc)) {
		uint8_t check[4] = { (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24) };
		unsigned i = 0;
		ok = true;
		// The bytes and the check go in one rewrite of the sector. More slots than a batch holds go in more rewrites,
		// each with the check: a cut between two leaves the check of the tables, and membership as far as it got
		do {
			codeplug_kv_op_t ops[EDIT_SLOTS + 1u];
			uint8_t bytes[EDIT_SLOTS][3];
			uint8_t cnt = 0;
			for (; i < n && cnt < EDIT_SLOTS; i++) {
				if (list == CONTACT_LIST_KEEP || slots[i] < 1u || slots[i] > count) {
					continue;
				}
				bytes[cnt][0] = (uint8_t)(slots[i] - 1u);
				bytes[cnt][1] = (uint8_t)((slots[i] - 1u) >> 8);
				bytes[cnt][2] = list;
				ops[cnt] = (codeplug_kv_op_t){ key, 1u | CODEPLUG_KV_TABLE, bytes[cnt] };
				cnt++;
			}
			ops[cnt++] = (codeplug_kv_op_t){ CODEPLUG_KEY_LIST_CHECK, sizeof(check), check };
			ok = codeplug_kv_write(ops, cnt);
		} while (ok && i < n);
	}
	storage_unlock();
	return ok;
}

// Contacts that are members of list *ctx, for the filtered finds
static bool in_list(uint16_t index, void *ctx)
{
	return codeplug_contact_list_of(index) == *(const uint8_t *)ctx;
}

#if CONFIG_AIR
static bool air_in_list(uint16_t slot, void *ctx)
{
	return codeplug_air_contact_list_of(slot) == *(const uint8_t *)ctx;
}
#endif

// The first contact of list l with this ID and call type that has a name, in the call's table
static bool list_name(uint8_t l, uint8_t mode, uint32_t id, bool group, char *out, size_t len)
{
	if (mode == CODEPLUG_NAME_DMR) {
		contact_t c;
		if (codeplug_contact_find_index_if(id, group, in_list, &l, &c) == 0 || c.name[0] == '\0') {
			return false;
		}
		strncpy(out, c.name, len - 1u);
		out[len - 1u] = '\0';
		return true;
	}
#if CONFIG_AIR
	air_contact_t a;
	if (codeplug_air_contact_find_if(mode, id, group, air_in_list, &l, &a)) {
		strncpy(out, a.name, len - 1u);
		out[len - 1u] = '\0';
		return true;
	}
#endif
	return false;
}

bool codeplug_name_lookup(const uint8_t sel[2], uint8_t mode, uint32_t id, bool group, char *out, size_t len)
{
	char name[CONTACT_LIST_NAME_LEN + 1];
	if (len == 0) {
		return false;
	}
	out[0] = '\0';
	for (unsigned i = 0; sel != NULL && i < 2u; i++) {
		uint8_t l = sel[i];
		if (i == 1u && l == sel[0]) {
			break;	// the same list again finds nothing new
		}
		if (l == CONTACT_LIST_IDDB) {
			// Only a DMR private ID has an entry: a talk group, an all call and every air mode ID has none
			if (mode == CODEPLUG_NAME_DMR && !group && dmr_iddb_lookup(id, out, len) && out[0] != '\0') {
				return true;
			}
			out[0] = '\0';
		} else if (l < CONTACT_LIST_MAX && codeplug_list_name(l, name) && list_name(l, mode, id, group, out, len)) {
			return true;
		}
	}
	out[0] = '\0';
	return false;
}

#else

bool codeplug_name_lookup(const uint8_t sel[2], uint8_t mode, uint32_t id, bool group, char *out, size_t len)
{
	(void)sel;
	if (len == 0) {
		return false;
	}
	out[0] = '\0';
	if (mode == CODEPLUG_NAME_DMR) {
		contact_t c;
		if (codeplug_contact_find(id, group, &c)) {
			strncpy(out, c.name, len - 1u);
			out[len - 1u] = '\0';
			return out[0] != '\0';
		}
		return !group && dmr_iddb_lookup(id, out, len) && out[0] != '\0';
	}
#if CONFIG_AIR
	air_contact_t a;
	if (codeplug_air_contact_find(mode, id, group, &a)) {
		strncpy(out, a.name, len - 1u);
		out[len - 1u] = '\0';
		return true;
	}
#endif
	return false;
}

#endif

#endif
