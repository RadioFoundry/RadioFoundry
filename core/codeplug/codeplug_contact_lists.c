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

// A name copied into out (len bytes with the terminator), cut to fit
static void copy_name(char *out, size_t len, const char *name)
{
	strncpy(out, name, len - 1u);
	out[len - 1u] = '\0';
}

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

// Whether the n bytes at v are a list name: 1-16 bytes, no control character
static bool name_ok(const uint8_t *v, int n)
{
	if (n < 1 || n > (int)CONTACT_LIST_NAME_LEN) {
		return false;
	}
	for (int i = 0; i < n; i++) {
		if (v[i] < 0x20u) {
			return false;
		}
	}
	return true;
}

// What was read of the contact list keys for this state of them, in one walk over their range. The one serial that
// moves for every change of the keys or the codeplug (codeplug_kv_serial) drops it all, as the contact tables' own edits
// do. Under storage_lock
static struct {
	uint8_t serial;
	bool valid;
	bool any;		// a key of 0x1E00-0x20FF is stored in any sector: the lists are in use (contact-lists.md 5.2)
	bool present;		// a membership key (DMR or air) is stored
	bool has_check;		// the check key is stored, 4 bytes
	uint32_t check;
	uint32_t names;		// bit n: list n exists (a valid name is stored)
	bool void_known;	// voided is worked out (once per serial, by the first thing that needs it)
	bool voided;		// membership is stored and the check key doesn't match the tables (3.3)
	bool sel_ok;		// the last system asked and its selection
	uint8_t sel_sys;
	uint8_t sel[2];
	uint16_t net_ch;	// the last channel asked (0: none) and its network
	uint8_t net_sys;
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

// Read the keys again if they or the codeplug changed: one walk of the range gives the lists that exist, whether any
// key is there, and what membership and its check are. Under storage_lock
static void refresh(void)
{
	uint8_t serial = codeplug_kv_serial();
	if (cache.valid && cache.serial == serial) {
		return;
	}
	memset(&cache, 0, sizeof(cache));
	cache.serial = serial;
	cache.valid = true;
	codeplug_kv_cursor_t cur;
	uint8_t v[CONTACT_LIST_NAME_LEN + 1u];	// a name and one byte more, to see one that is too long
	uint16_t key;
	int n;
	codeplug_kv_begin(&cur, CODEPLUG_KV_CONT_LO, CODEPLUG_KV_CONT_HI);
	while ((n = codeplug_kv_next(&cur, &key, v, sizeof(v))) >= 0) {
		cache.any = true;
		if (key >= CODEPLUG_KEY_LIST_NAME && key < CODEPLUG_KEY_LIST_NAME + CONTACT_LIST_MAX) {
			if (name_ok(v, n)) {
				cache.names |= 1u << (key - CODEPLUG_KEY_LIST_NAME);
			}
		} else if (key == CODEPLUG_KEY_LIST_DMR || key == CODEPLUG_KEY_LIST_AIR) {
			cache.present = true;
		} else if (key == CODEPLUG_KEY_LIST_CHECK && n == 4) {
			cache.has_check = true;
			cache.check = get32(v);
		}
	}
}

// Whether membership is live: stored, and the tables are what its check says (3.3). Void when the check key is absent,
// not 4 bytes or not the tables' check; tables that can't be read are void too (a name is never wrong). The tables are
// read once per state of the keys. Under storage_lock
static bool members_live(void)
{
	refresh();
	if (!cache.void_known) {
		uint32_t have;
		cache.voided = cache.present && (!cache.has_check || !tables_check(&have) || have != cache.check);
		cache.void_known = true;
	}
	return cache.present && !cache.voided;
}

// The membership byte of slot (1-based) in the table of key, of count slots: none when there is no live membership
static uint8_t member_of(uint16_t key, uint16_t slot, uint16_t count)
{
	uint8_t b = CONTACT_LIST_NONE;
	if (slot < 1u || slot > count) {
		return CONTACT_LIST_NONE;
	}
	storage_lock();
	if (members_live()) {
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
	out[0] = out[1] = CONTACT_LIST_NONE;
	if (sys == 0xFFu) {
		return;
	}
	storage_lock();
	refresh();
	if (cache.any && (!cache.sel_ok || cache.sel_sys != sys)) {
		uint8_t v[2] = { CONTACT_LIST_NONE, CONTACT_LIST_NONE };
		int n = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_SYS_LISTS + sys), v, sizeof(v));
		cache.sel[0] = n >= 1 ? select_byte(v[0]) : CONTACT_LIST_NONE;
		cache.sel[1] = n >= 2 ? select_byte(v[1]) : CONTACT_LIST_NONE;
		cache.sel_sys = sys;
		cache.sel_ok = true;
	}
	if (cache.any) {
		out[0] = cache.sel[0];
		out[1] = cache.sel[1];
	}
	storage_unlock();
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
	refresh();
	if (cache.any) {
		if (cache.net_ch != ch) {
			codeplug_kv_read(CODEPLUG_KEY_CH_NET, (uint16_t)(ch - 1u), &b, 1);
			cache.net_ch = ch;
			cache.net_sys = is_network(b) ? b : CODEPLUG_SYSTEM_NONE;
		}
		b = cache.net_sys;
	}
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
	if (!name_ok(v, len)) {
		return false;
	}
	memcpy(out, v, (size_t)len);
	out[len] = '\0';
	return true;
}

static bool read_check(uint32_t *crc)
{
	uint8_t v[5];
	if (codeplug_kv_get(CODEPLUG_KEY_LIST_CHECK, v, sizeof(v)) != 4) {
		return false;
	}
	*crc = get32(v);
	return true;
}

void codeplug_contact_lists_begin(codeplug_contact_edit_t *e)
{
	storage_lock();
	members_live();
	e->state = !cache.present ? EDIT_NONE : cache.voided ? EDIT_VOID : EDIT_LIVE;
	e->has_check = read_check(&e->check);
	storage_unlock();
}

bool codeplug_contact_lists_end(const codeplug_contact_edit_t *e, const uint16_t *slots, unsigned n)
{
	if (e->state != EDIT_LIVE) {
		return true;	// void: left as found; no membership: no contact is in a list and nothing is kept
	}
	uint32_t crc, now;
	bool ok = false;
	storage_lock();
	// The check key as begin found it, or someone else (another task, a CPS write) changed membership meanwhile: then
	// this check would vouch for a state it never saw, so nothing is written and membership stays void
	if (read_check(&now) == e->has_check && (!e->has_check || now == e->check) && tables_check(&crc)) {
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
				if (slots[i] < 1u || slots[i] > CODEPLUG_CONTACTS_MAX) {
					continue;
				}
				bytes[cnt][0] = (uint8_t)(slots[i] - 1u);
				bytes[cnt][1] = (uint8_t)((slots[i] - 1u) >> 8);
				bytes[cnt][2] = CONTACT_LIST_NONE;
				ops[cnt] = (codeplug_kv_op_t){ CODEPLUG_KEY_LIST_DMR, 1u | CODEPLUG_KV_TABLE, bytes[cnt] };
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
		copy_name(out, len, c.name);
		return true;
	}
#if CONFIG_AIR
	air_contact_t a;
	if (codeplug_air_contact_find_if(mode, id, group, air_in_list, &l, &a)) {
		copy_name(out, len, a.name);
		return true;
	}
#endif
	return false;
}

#endif

// A codeplug with no contact list key (or with the lists switched off): the contacts as before the lists, the first
// with the ID and call type, then for a private ID the ID database. A contact found stops the database, name or not
static bool legacy_lookup(uint8_t mode, uint32_t id, bool group, char *out, size_t len)
{
	out[0] = '\0';
	if (mode == CODEPLUG_NAME_DMR) {
		contact_t c;
		if (codeplug_contact_find(id, group, &c)) {
			copy_name(out, len, c.name);
			return out[0] != '\0';
		}
		return !group && dmr_iddb_lookup(id, out, len) && out[0] != '\0';
	}
#if CONFIG_AIR
	air_contact_t a;
	if (codeplug_air_contact_find(mode, id, group, &a)) {
		copy_name(out, len, a.name);
		return true;
	}
#endif
	return false;
}

bool codeplug_name_lookup(const uint8_t sel[2], uint8_t mode, uint32_t id, bool group, char *out, size_t len)
{
	if (len == 0) {
		return false;
	}
	out[0] = '\0';
#if CONFIG_CONTACT_LISTS
	storage_lock();
	refresh();
	bool legacy = !cache.any;
	uint32_t exist = cache.names;
	bool live = !legacy && members_live();
	storage_unlock();
	if (legacy) {
		return legacy_lookup(mode, id, group, out, len);	// not migrated: every contact names a call (5.2)
	}
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
		} else if (l < CONTACT_LIST_MAX && live && (exist & (1u << l)) != 0 && list_name(l, mode, id, group, out, len)) {
			return true;	// without live membership no contact is in a list: nothing to scan
		}
	}
	out[0] = '\0';
	return false;
#else
	(void)sel;
	return legacy_lookup(mode, id, group, out, len);
#endif
}

#endif
