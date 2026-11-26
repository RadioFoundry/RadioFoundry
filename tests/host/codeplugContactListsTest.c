// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Named contact lists (docs/spec/contact-lists.md, core/codeplug/codeplug_contact_lists.c) on a simulated NOR flash: the
// keys (list names, a system's selection, a channel's network), membership and the void rule, the name lookup in the
// order of a selection (list, then the ID database; once a list key exists no selection means no name, and a codeplug
// with none keeps the old lookup), the table check and the radio-side edit that keeps it, the Conventional system
// type, and with CONFIG_CONTACT_LISTS off the lookup as it was before the lists
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_air_contacts.h"
#include "codeplug/codeplug_contact_lists.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_modes.h"
#include "codeplug/codeplug_records.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

#define FLASH_SIZE	0x200000u
#define EE		0x000000u
#define FL		0x020000u
#define CONTACTS	(FL + 0x87620u)			// the DMR contact table (open-firmware-cps.md 7.3.5)
#define AIR_TABLE	(EE + CODEPLUG_AIR_CONTACTS_ADDR)
#define CONT		(EE + CODEPLUG_KV_CONT_ADDR)
#define P25		CODEPLUG_KIND_P25

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

// The ID database, as a stub: private IDs 5000 and 7777 have an entry; how often it was asked
static unsigned iddb_asked;
bool dmr_iddb_lookup(uint32_t id, char *out, size_t len)
{
	iddb_asked++;
	const char *text = id == 5000 ? "DB5000 Bob" : id == 7777 ? "DB7777 Carol" : NULL;
	if (text == NULL) {
		return false;
	}
	strncpy(out, text, len - 1);
	out[len - 1] = '\0';
	return true;
}

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
	iddb_asked = 0;
}

static void contact(uint16_t slot, const char *name, uint32_t id, contact_type_t type)
{
	contact_t c;
	memset(&c, 0, sizeof(c));
	strcpy(c.name, name);
	c.id = id;
	c.type = (uint8_t)type;
	CHECK(codeplug_set_contact(slot, &c));
}

#if CONFIG_CONTACT_LISTS || CONFIG_AIR
static void air_slot(uint16_t n, uint8_t flags, uint32_t id, const char *name)
{
	uint8_t *r = &sim_flash_data[AIR_TABLE + (n - 1u) * CODEPLUG_AIR_CONTACT_SIZE];
	r[0] = P25;
	r[1] = flags;
	r[2] = (uint8_t)id;
	r[3] = (uint8_t)(id >> 8);
	r[4] = (uint8_t)(id >> 16);
	memset(&r[5], 0xFF, 3);
	memset(&r[8], 0xFF, CODEPLUG_NAME_LEN);
	memcpy(&r[8], name, strlen(name));
}
#endif

#if CONFIG_CONTACT_LISTS
// The table check as docs/spec/contact-lists.md 3.3 gives it, from the flash bytes: 5 bytes at offset 0x10 of each of
// the 1024 DMR slots, then the first 5 bytes of each of the 341 air slots
static uint32_t table_check(void)
{
	uint32_t crc = 0;
	for (unsigned i = 0; i < CODEPLUG_CONTACTS_MAX; i++) {
		crc = storage_crc32(crc, &sim_flash_data[CONTACTS + i * 24u + 0x10u], 5);
	}
	for (unsigned i = 0; i < CODEPLUG_AIR_CONTACTS_MAX; i++) {
		crc = storage_crc32(crc, &sim_flash_data[AIR_TABLE + i * CODEPLUG_AIR_CONTACT_SIZE], 5);
	}
	return crc;
}

// List n's name key set (an empty name or NULL takes the list away): the editor's job, here for the tests
static void set_name(uint8_t n, const char *name)
{
	uint16_t key = (uint16_t)(CODEPLUG_KEY_LIST_NAME + n);
	CHECK(name == NULL || name[0] == '\0' ? codeplug_kv_remove(key) : codeplug_kv_set(key, name, (uint16_t)strlen(name)));
}

static void put_check(uint32_t crc)
{
	uint8_t v[4] = { (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24) };
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_CHECK, v, sizeof(v)));
}

static uint32_t stored_check(void)
{
	uint8_t v[5];
	return codeplug_kv_get(CODEPLUG_KEY_LIST_CHECK, v, sizeof(v)) == 4 ?
	       (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24) : 0;
}

#endif

// A name looked up: the text it gave, "" when none
static const char *look(const uint8_t sel[2], uint8_t mode, uint32_t id, bool group)
{
	static char out[40];
	if (!codeplug_name_lookup(sel, mode, id, group, out, sizeof(out))) {
		CHECK(out[0] == '\0');
	}
	return out;
}

#define SEL(a, b)	((const uint8_t[2]){ (a), (b) })
#define DMR		CODEPLUG_NAME_DMR

#if CONFIG_CONTACT_LISTS
// The contacts: TG 100 in two lists, private 5000 in two lists, one in no list, one in a list that has no name
static void build(void)
{
	fresh();
	contact(1, "Local100", 100, CONTACT_GROUP);
	contact(2, "County100", 100, CONTACT_GROUP);
	contact(3, "Alice", 5000, CONTACT_PRIVATE);
	contact(4, "Alicia", 5000, CONTACT_PRIVATE);
	contact(5, "Loose", 400, CONTACT_GROUP);
	contact(6, "Nameless", 300, CONTACT_GROUP);
	set_name(0, "Local");
	set_name(1, "County");
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_DMR, (const uint8_t[]){ 0, 1, 0, 1, 0xFF, 5 }, 6));
	put_check(table_check());
}

#endif

static void test_registry(void)
{
	CHECK(CODEPLUG_SYSTYPE_CONVENTIONAL == 6 && strcmp(codeplug_systype_name(6), "NET") == 0);
#if CONFIG_CONTACT_LISTS
	CHECK(codeplug_systype_built(6));
#else
	CHECK(!codeplug_systype_built(6));
#endif
	CHECK(!codeplug_systype_built(2));	// still reserved
}

#if CONFIG_CONTACT_LISTS

static void test_names(void)
{
	char n[CONTACT_LIST_NAME_LEN + 1];
	fresh();
	CHECK(!codeplug_list_name(0, n));
	set_name(0, "Local TGs");
	CHECK(codeplug_list_name(0, n) && strcmp(n, "Local TGs") == 0);
	set_name(31, "0123456789ABCDEF");
	CHECK(codeplug_list_name(31, n) && strcmp(n, "0123456789ABCDEF") == 0);
	CHECK(!codeplug_list_name(32, n));
	// What another tool stored: a name over 16 bytes, one with a control byte, an empty one: not lists
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME + 2, "0123456789ABCDEFG", 17));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME + 3, "a\x01", 2));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME + 4, "", 0));
	CHECK(!codeplug_list_name(2, n) && !codeplug_list_name(3, n) && !codeplug_list_name(4, n));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME + 5, "\xC3\xA9t\xC3\xA9", 5) && codeplug_list_name(5, n) && strlen(n) == 5);	// UTF-8
	// Taking the key away takes the list away
	set_name(0, "");
	CHECK(!codeplug_list_name(0, n));
	set_name(31, NULL);
	CHECK(!codeplug_list_name(31, n));
	// They live in the continuation, none in the first sector
	CHECK(codeplug_kv_check(&sim_flash_data[CONT]) == CODEPLUG_KV_VALID);
	CHECK(sim_flash_violations() == 0);
}

static void test_selection(void)
{
	uint8_t s[2];
	fresh();
	codeplug_system_lists(3, s);
	CHECK(s[0] == CONTACT_LIST_NONE && s[1] == CONTACT_LIST_NONE);			// no key: no selection
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_LISTS + 3, (const uint8_t[]){ 0, CONTACT_LIST_IDDB }, 2));
	codeplug_system_lists(3, s);
	CHECK(s[0] == 0 && s[1] == CONTACT_LIST_IDDB);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_LISTS + 4, (const uint8_t[]){ 2 }, 1));	// one byte: the primary only
	codeplug_system_lists(4, s);
	CHECK(s[0] == 2 && s[1] == CONTACT_LIST_NONE);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_LISTS + 5, (const uint8_t[]){ CONTACT_LIST_IDDB, 31, 7 }, 3));	// more: from the front
	codeplug_system_lists(5, s);
	CHECK(s[0] == CONTACT_LIST_IDDB && s[1] == 31);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_LISTS + 6, (const uint8_t[]){ 0x40, 0xFD }, 2));	// not list bytes: none
	codeplug_system_lists(6, s);
	CHECK(s[0] == CONTACT_LIST_NONE && s[1] == CONTACT_LIST_NONE);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_LISTS + 7, "", 0));			// empty: none
	codeplug_system_lists(7, s);
	CHECK(s[0] == CONTACT_LIST_NONE && s[1] == CONTACT_LIST_NONE);
	codeplug_system_lists(0xFF, s);
	CHECK(s[0] == CONTACT_LIST_NONE && s[1] == CONTACT_LIST_NONE);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_LISTS + 254, (const uint8_t[]){ 1, 2 }, 2));	// the last system number
	codeplug_system_lists(254, s);
	CHECK(s[0] == 1 && s[1] == 2);
	// The last answer is kept until the keys move: an edit of the same system is seen
	codeplug_system_lists(254, s);
	CHECK(s[0] == 1 && s[1] == 2);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_LISTS + 254, (const uint8_t[]){ 3, CONTACT_LIST_IDDB }, 2));
	codeplug_system_lists(254, s);
	CHECK(s[0] == 3 && s[1] == CONTACT_LIST_IDDB);
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_SYS_LISTS + 254));
	codeplug_system_lists(254, s);
	CHECK(s[0] == CONTACT_LIST_NONE && s[1] == CONTACT_LIST_NONE);
}

// A system record: type, then zeros to its 28-byte header
static void system_record(uint8_t n, uint8_t type, uint16_t len)
{
	uint8_t r[CODEPLUG_SYS_HDR];
	memset(r, 0, sizeof(r));
	r[0] = type;
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_SYSTEM + n), r, len));
}

static void test_channel_network(void)
{
	fresh();
	CHECK(codeplug_channel_system(5) == CODEPLUG_SYSTEM_NONE);			// no key
	system_record(8, CODEPLUG_SYSTYPE_CONVENTIONAL, CODEPLUG_SYS_HDR);
	system_record(10, CODEPLUG_SYSTYPE_DMR_TRUNK, CODEPLUG_SYS_HDR);			// a system, but not a network
	system_record(11, CODEPLUG_SYSTYPE_CONVENTIONAL, 10);				// a record too short to be one
	uint8_t t[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 8, 9, 10, 11 };			// channels 1-8: 5 -> 8, 6 -> 9 (none), 7 -> 10, 8 -> 11
	CHECK(codeplug_kv_set(CODEPLUG_KEY_CH_NET, t, sizeof(t)));
	CHECK(codeplug_channel_system(5) == 8);
	CHECK(codeplug_channel_system(6) == CODEPLUG_SYSTEM_NONE);			// names a system that doesn't exist
	CHECK(codeplug_channel_system(7) == CODEPLUG_SYSTEM_NONE);			// another type
	CHECK(codeplug_channel_system(8) == CODEPLUG_SYSTEM_NONE);			// too short
	CHECK(codeplug_channel_system(1) == CODEPLUG_SYSTEM_NONE);			// 0xFF: none
	CHECK(codeplug_channel_system(9) == CODEPLUG_SYSTEM_NONE);			// past the value: none
	CHECK(codeplug_channel_system(0) == CODEPLUG_SYSTEM_NONE && codeplug_channel_system(1025) == CODEPLUG_SYSTEM_NONE);
	// A network of number 254 (the highest) and the last channel
	system_record(254, CODEPLUG_SYSTYPE_CONVENTIONAL, CODEPLUG_SYS_HDR);
	codeplug_kv_op_t last = { CODEPLUG_KEY_CH_NET, 1 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 0xFF, 0x03, 254 } };
	CHECK(codeplug_kv_write(&last, 1) && codeplug_channel_system(1024) == 254);
	CHECK(codeplug_channel_system(5) == 8);
	// The last answer is kept until the keys move: a change of the channel's byte, or of its system's record, is seen
	CHECK(codeplug_channel_system(5) == 8);
	codeplug_kv_op_t clear = { CODEPLUG_KEY_CH_NET, 1 | CODEPLUG_KV_TABLE, (const uint8_t[]){ 4, 0, 0xFF } };
	CHECK(codeplug_kv_write(&clear, 1) && codeplug_channel_system(5) == CODEPLUG_SYSTEM_NONE);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_CH_NET, t, sizeof(t)) && codeplug_channel_system(5) == 8);
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_SYSTEM + 8) && codeplug_channel_system(5) == CODEPLUG_SYSTEM_NONE);
	system_record(8, CODEPLUG_SYSTYPE_CONVENTIONAL, CODEPLUG_SYS_HDR);
	CHECK(codeplug_channel_system(5) == 8);
	// The key that holds it is in the continuation
	CHECK(codeplug_kv_check(&sim_flash_data[CONT]) == CODEPLUG_KV_VALID);
}

// A channel's network goes with the channel: one written at the number later has none (contact-lists.md 3.2)
static void test_delete_channel(void)
{
	channel_t ch;
	memset(&ch, 0, sizeof(ch));
	strcpy(ch.name, "Net one");
	ch.rx_hz = ch.tx_hz = 451000000;
	ch.mode = RADIO_MODE_DMR;
	ch.bw = RADIO_BW_12K5;
	ch.power = CODEPLUG_POWER_DEFAULT;
	ch.color_code = 1;
	ch.timeslot = 1;
	ch.system = CODEPLUG_SYSTEM_NONE;
	fresh();
	CHECK(codeplug_set_channel(5, &ch) && codeplug_delete_channel(5));		// no networks stored at all: nothing to clear
	CHECK(sim_flash_data[CONT] == 0xFF);
	CHECK(codeplug_set_channel(5, &ch) && codeplug_set_channel(6, &ch));
	system_record(8, CODEPLUG_SYSTYPE_CONVENTIONAL, CODEPLUG_SYS_HDR);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_CH_NET, (const uint8_t[]){ 0xFF, 0xFF, 0xFF, 0xFF, 8, 8 }, 6));
	CHECK(codeplug_channel_system(5) == 8 && codeplug_channel_system(6) == 8);
	CHECK(codeplug_delete_channel(5));
	CHECK(codeplug_set_channel(5, &ch));
	CHECK(codeplug_channel_system(5) == CODEPLUG_SYSTEM_NONE);			// the new channel has no network
	CHECK(codeplug_channel_system(6) == 8);						// the next one keeps its own
	// The last network taken away leaves the key out (the codeplug is not migrated again by it)
	CHECK(codeplug_delete_channel(6) && codeplug_kv_get(CODEPLUG_KEY_CH_NET, NULL, 0) < 0);
	// A channel past the value (a short table) deletes without a write of the key
	CHECK(codeplug_kv_set(CODEPLUG_KEY_CH_NET, (const uint8_t[]){ 8 }, 1));
	CHECK(codeplug_set_channel(7, &ch) && codeplug_delete_channel(7) && codeplug_channel_system(1) == 8);
	CHECK(sim_flash_violations() == 0);
}

static void test_membership(void)
{
	build();
	CHECK(codeplug_contact_list_of(1) == 0 && codeplug_contact_list_of(2) == 1);
	CHECK(codeplug_contact_list_of(3) == 0 && codeplug_contact_list_of(4) == 1);
	CHECK(codeplug_contact_list_of(5) == CONTACT_LIST_NONE);				// 0xFF: in no list
	CHECK(codeplug_contact_list_of(6) == 5);					// a list that has no name is still the byte
	CHECK(codeplug_contact_list_of(7) == CONTACT_LIST_NONE);				// past the value: 0xFF
	CHECK(codeplug_contact_list_of(0) == CONTACT_LIST_NONE && codeplug_contact_list_of(1025) == CONTACT_LIST_NONE);
	// 0xFE is never a membership byte, nor is anything above 31
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_DMR, (const uint8_t[]){ 0, CONTACT_LIST_IDDB, 0x20, 0xFD }, 4));
	CHECK(codeplug_contact_list_of(1) == 0 && codeplug_contact_list_of(2) == CONTACT_LIST_NONE);
	CHECK(codeplug_contact_list_of(3) == CONTACT_LIST_NONE && codeplug_contact_list_of(4) == CONTACT_LIST_NONE);
	// A value longer than the table is read from the front
	static uint8_t long_value[1030];
	memset(long_value, 3, sizeof(long_value));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_DMR, long_value, sizeof(long_value)));
	CHECK(codeplug_contact_list_of(1024) == 3 && codeplug_contact_list_of(1025) == CONTACT_LIST_NONE);
	CHECK(sim_flash_violations() == 0);
}

// The void rule (3.3): a membership key with no check, a short one, a wrong one: no contact is in a list
static void test_void(void)
{
	uint8_t s[2];
	build();
	CHECK(codeplug_contact_list_of(1) == 0);
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_LIST_CHECK));
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE && codeplug_contact_list_of(2) == CONTACT_LIST_NONE);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_CHECK, "\x01\x02\x03", 3));		// not 4 bytes
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE);
	put_check(table_check() ^ 1u);							// the wrong check
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_CHECK, "\x01\x02\x03\x04\x05", 5));	// longer than 4
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE);
	put_check(table_check());							// right again: live
	CHECK(codeplug_contact_list_of(1) == 0 && codeplug_contact_list_of(2) == 1);
	// Names and the ID don't move the check; a contact's ID, call type or slot does
	contact_t c;
	CHECK(codeplug_contact(1, &c));
	strcpy(c.name, "Renamed100");
	CHECK(codeplug_set_contact(1, &c));
	CHECK(codeplug_contact_list_of(1) == 0);					// a rename doesn't void membership
	c.timeslot = 2;
	CHECK(codeplug_set_contact(1, &c) && codeplug_contact_list_of(1) == 0);		// nor does a timeslot
	c.id = 101;
	CHECK(codeplug_set_contact(1, &c));
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE && codeplug_contact_list_of(2) == CONTACT_LIST_NONE);	// a new ID does
	c.id = 100;
	CHECK(codeplug_set_contact(1, &c) && codeplug_contact_list_of(1) == 0);		// and back: live again
	CHECK(codeplug_delete_contact(5) && codeplug_contact_list_of(1) == CONTACT_LIST_NONE);	// a delete does
	// The air table is in the check as well, and read without any air mode
	build();
	CHECK(codeplug_contact_list_of(1) == 0);
	air_slot(3, AIR_CONTACT_GROUP, 1310, "A P25 TG");
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1));			// something that moves the serial: the keys read again
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE);
	// Nothing else is void: the selection, the lists and the channel network
	set_name(7, "Still");
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_LISTS + 2, (const uint8_t[]){ 1 }, 1));
	codeplug_system_lists(2, s);
	CHECK(s[0] == 1);
	char n[CONTACT_LIST_NAME_LEN + 1];
	CHECK(codeplug_list_name(7, n));
	// A codeplug with no membership key has nothing to void (the check key is not needed)
	fresh();
	contact(1, "Solo", 9, CONTACT_GROUP);
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_DMR, (const uint8_t[]){ 4 }, 1));	// a key with no check: void
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE);
	put_check(table_check());
	CHECK(codeplug_contact_list_of(1) == 4);
}

static void test_lookup(void)
{
	build();
	// A selection names a call through its lists, in order
	CHECK(strcmp(look(SEL(0, CONTACT_LIST_NONE), DMR, 100, true), "Local100") == 0);
	CHECK(strcmp(look(SEL(1, CONTACT_LIST_NONE), DMR, 100, true), "County100") == 0);
	CHECK(strcmp(look(SEL(1, 0), DMR, 100, true), "County100") == 0);
	CHECK(strcmp(look(SEL(0, 1), DMR, 100, true), "Local100") == 0);
	CHECK(strcmp(look(SEL(0, 1), DMR, 5000, false), "Alice") == 0);
	CHECK(strcmp(look(SEL(1, 0), DMR, 5000, false), "Alicia") == 0);
	CHECK(iddb_asked == 0);								// the database is asked only when selected
	// A call type is a call type: TG 100 isn't private 100, and an ID in no list names nothing
	CHECK(look(SEL(0, 1), DMR, 100, false)[0] == '\0');
	CHECK(look(SEL(0, 1), DMR, 400, true)[0] == '\0');				// "Loose" is in no list
	CHECK(look(SEL(0, 1), DMR, 9999, true)[0] == '\0');				// not a contact
	// No selection, a missing one, a list that doesn't exist (list 5 has no name) or isn't there: no name
	CHECK(look(SEL(CONTACT_LIST_NONE, CONTACT_LIST_NONE), DMR, 100, true)[0] == '\0');
	CHECK(look(NULL, DMR, 100, true)[0] == '\0');
	CHECK(look(SEL(5, CONTACT_LIST_NONE), DMR, 300, true)[0] == '\0');
	CHECK(look(SEL(9, 31), DMR, 100, true)[0] == '\0');
	CHECK(look(SEL(0x40, 0xFD), DMR, 100, true)[0] == '\0');				// not list bytes
	CHECK(!codeplug_name_lookup(NULL, DMR, 100, true, (char[4]){ 0 }, 4));
	CHECK(!codeplug_name_lookup(SEL(0, 1), DMR, 100, true, NULL, 0));			// no room: no name
	// The ID database, from its place in the selection: before a contact list it wins, after it it is the fallback
	CHECK(strcmp(look(SEL(CONTACT_LIST_IDDB, 0), DMR, 5000, false), "DB5000 Bob") == 0);
	CHECK(strcmp(look(SEL(0, CONTACT_LIST_IDDB), DMR, 5000, false), "Alice") == 0);
	CHECK(strcmp(look(SEL(0, CONTACT_LIST_IDDB), DMR, 7777, false), "DB7777 Carol") == 0);
	CHECK(strcmp(look(SEL(CONTACT_LIST_IDDB, CONTACT_LIST_NONE), DMR, 7777, false), "DB7777 Carol") == 0);
	CHECK(look(SEL(CONTACT_LIST_IDDB, CONTACT_LIST_NONE), DMR, 4242, false)[0] == '\0');	// not in the database
	// ... for a private ID only: a group, and the all call, have no entry and the walk goes on
	iddb_asked = 0;
	CHECK(look(SEL(CONTACT_LIST_IDDB, CONTACT_LIST_NONE), DMR, 7777, true)[0] == '\0');
	CHECK(strcmp(look(SEL(CONTACT_LIST_IDDB, 0), DMR, 100, true), "Local100") == 0);
	CHECK(iddb_asked == 0);
	CHECK(strcmp(look(SEL(CONTACT_LIST_IDDB, CONTACT_LIST_IDDB), DMR, 7777, false), "DB7777 Carol") == 0 && iddb_asked == 1);
	// The answer is cut to the room given
	char small[4];
	CHECK(codeplug_name_lookup(SEL(0, 1), DMR, 100, true, small, sizeof(small)) && strcmp(small, "Loc") == 0);
	// Void membership: no contact names a call, the ID database still does
	put_check(table_check() ^ 1u);
	CHECK(look(SEL(0, 1), DMR, 100, true)[0] == '\0' && look(SEL(0, 1), DMR, 5000, false)[0] == '\0');
	CHECK(strcmp(look(SEL(0, CONTACT_LIST_IDDB), DMR, 5000, false), "DB5000 Bob") == 0);
	// The unfiltered find sees every contact, in a list or not (the radio's browser and Find)
	contact_t c;
	CHECK(codeplug_contact_find(400, true, &c) && strcmp(c.name, "Loose") == 0);
	CHECK(codeplug_contact_find(100, true, &c) && strcmp(c.name, "Local100") == 0);	// the first
	CHECK(codeplug_contact_find_index_if(100, true, NULL, NULL, &c) == 1);
	CHECK(sim_flash_violations() == 0);
}

#if CONFIG_AIR
static void test_lookup_air(void)
{
	build();
	air_slot(1, AIR_CONTACT_GROUP, 1310, "Fire TG");
	air_slot(2, AIR_CONTACT_GROUP, 1310, "Police TG");	// the same group, another list
	air_slot(3, AIR_CONTACT_UNIT, 1310, "Engine 7");
	air_slot(4, AIR_CONTACT_GROUP, 2000, "Unlisted");
	air_slot(5, AIR_CONTACT_ALL, 0, "Everyone");
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_AIR, (const uint8_t[]){ 2, 3, 2, 0xFF, 2 }, 5));
	set_name(2, "Fire");
	set_name(3, "PD");
	put_check(table_check());
	CHECK(codeplug_air_contact_list_of(1) == 2 && codeplug_air_contact_list_of(2) == 3 && codeplug_air_contact_list_of(4) == CONTACT_LIST_NONE);
	CHECK(codeplug_air_contact_list_of(0) == CONTACT_LIST_NONE && codeplug_air_contact_list_of(CODEPLUG_AIR_CONTACTS_MAX + 1u) == CONTACT_LIST_NONE);
	CHECK(strcmp(look(SEL(2, 3), P25, 1310, true), "Fire TG") == 0);
	CHECK(strcmp(look(SEL(3, 2), P25, 1310, true), "Police TG") == 0);
	CHECK(strcmp(look(SEL(2, CONTACT_LIST_NONE), P25, 1310, false), "Engine 7") == 0);	// a unit apart from the group
	CHECK(look(SEL(3, CONTACT_LIST_NONE), P25, 1310, false)[0] == '\0');			// Engine 7 isn't in list 3
	CHECK(strcmp(look(SEL(2, CONTACT_LIST_NONE), P25, 0xFFFF, true), "Everyone") == 0);	// the all call: talk group 0xFFFF
	CHECK(look(SEL(2, 3), P25, 2000, true)[0] == '\0');					// in no list
	CHECK(look(SEL(CONTACT_LIST_NONE, CONTACT_LIST_NONE), P25, 1310, true)[0] == '\0');
	CHECK(look(SEL(2, 3), 0x04, 1310, true)[0] == '\0');					// another mode: no table
	// A P25 call isn't named by the DMR contacts, and never by the ID database
	CHECK(look(SEL(0, 1), P25, 100, true)[0] == '\0');
	iddb_asked = 0;
	CHECK(look(SEL(CONTACT_LIST_IDDB, CONTACT_LIST_NONE), P25, 5000, false)[0] == '\0' && iddb_asked == 0);
	// The air table is in the check: changing a slot voids both tables' membership
	air_slot(4, AIR_CONTACT_GROUP, 2001, "Moved");
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 1));
	CHECK(codeplug_air_contact_list_of(1) == CONTACT_LIST_NONE && codeplug_contact_list_of(1) == CONTACT_LIST_NONE);
	CHECK(look(SEL(2, 3), P25, 1310, true)[0] == '\0');
	CHECK(sim_flash_violations() == 0);
}
#endif

// Membership bytes of slots first..first+n-1 set to list l, as the Codeplug Editor would have stored them (the tables
// don't change, so the check stays right)
static void members(uint16_t first, unsigned n, uint8_t l)
{
	uint8_t v[2 + 64];
	CHECK(n <= 64);
	v[0] = (uint8_t)(first - 1u);
	v[1] = (uint8_t)((first - 1u) >> 8);
	memset(&v[2], l, n);
	codeplug_kv_op_t op = { CODEPLUG_KEY_LIST_DMR, (uint16_t)(n | CODEPLUG_KV_TABLE), v };
	CHECK(codeplug_kv_write(&op, 1));
}

static void test_update(void)
{
	codeplug_contact_edit_t e;
	uint16_t slot;
	build();
	// A new contact in a live codeplug: in no list, the check follows the table, the others unchanged
	codeplug_contact_lists_begin(&e);
	contact(8, "New800", 800, CONTACT_GROUP);
	slot = 8;
	CHECK(codeplug_contact_lists_end(&e, &slot, 1));
	CHECK(stored_check() == table_check());
	CHECK(codeplug_contact_list_of(8) == CONTACT_LIST_NONE && codeplug_contact_list_of(1) == 0 && codeplug_contact_list_of(2) == 1);
	CHECK(strcmp(look(SEL(0, 1), DMR, 100, true), "Local100") == 0);		// not void
	// A new contact in a slot that had a stale byte: it is in no list
	members(9, 1, 1);
	codeplug_contact_lists_begin(&e);
	contact(9, "Fresh900", 900, CONTACT_GROUP);
	slot = 9;
	CHECK(codeplug_contact_lists_end(&e, &slot, 1));
	CHECK(codeplug_contact_list_of(9) == CONTACT_LIST_NONE && look(SEL(1, CONTACT_LIST_NONE), DMR, 900, true)[0] == '\0');
	CHECK(stored_check() == table_check());
	// An edit that keeps the slot's list (a rename, a timeslot) needs no end: the check doesn't cover them
	contact_t c;
	CHECK(codeplug_contact(2, &c));
	strcpy(c.name, "County100b");
	CHECK(codeplug_set_contact(2, &c));
	CHECK(codeplug_contact_list_of(2) == 1 && strcmp(look(SEL(1, CONTACT_LIST_NONE), DMR, 100, true), "County100b") == 0);
	// A delete: the slot's byte is 0xFF and its successor in the table is not given the list
	codeplug_contact_lists_begin(&e);
	CHECK(codeplug_delete_contact(1));
	slot = 1;
	CHECK(codeplug_contact_lists_end(&e, &slot, 1));
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE && codeplug_contact_list_of(2) == 1);
	CHECK(look(SEL(0, CONTACT_LIST_NONE), DMR, 100, true)[0] == '\0');		// the Local100 contact is gone
	CHECK(stored_check() == table_check());
	// Several slots in one call, more than a batch holds: all become 0xFF
	uint16_t many[20];
	members(20, 20, 2);
	CHECK(codeplug_contact_list_of(20) == 2 && codeplug_contact_list_of(39) == 2);
	codeplug_contact_lists_begin(&e);
	for (unsigned i = 0; i < 20; i++) {
		many[i] = (uint16_t)(20 + i);
		char name[8];
		snprintf(name, sizeof(name), "M%u", i);
		contact(many[i], name, 2000 + i, CONTACT_GROUP);
	}
	CHECK(codeplug_contact_lists_end(&e, many, 20));
	CHECK(codeplug_contact_list_of(20) == CONTACT_LIST_NONE && codeplug_contact_list_of(39) == CONTACT_LIST_NONE);
	CHECK(codeplug_contact_list_of(2) == 1 && stored_check() == table_check());
	// A slot out of range is left out
	uint16_t odd[3] = { 0, 1025, 41 };
	members(41, 1, 3);
	CHECK(codeplug_contact_list_of(41) == 3);
	codeplug_contact_lists_begin(&e);
	CHECK(codeplug_contact_lists_end(&e, odd, 3) && codeplug_contact_list_of(41) == CONTACT_LIST_NONE);
	// The one check covers the air table too: an air slot filled before an edit is in the check the edit writes
	air_slot(3, AIR_CONTACT_GROUP, 1310, "A P25 TG");
	codeplug_contact_lists_begin(&e);
	contact(50, "Another", 5050, CONTACT_GROUP);
	slot = 50;
	CHECK(codeplug_contact_lists_end(&e, &slot, 1));
	CHECK(stored_check() == table_check());
	CHECK(codeplug_contact_list_of(2) == 1 && codeplug_air_contact_list_of(3) == CONTACT_LIST_NONE);
	CHECK(sim_flash_violations() == 0);
}

// The check key as begin found it: another writer that changed it since (a CPS write, a second task) leaves the end
// writing nothing, so membership stays void and the new check is never vouched for
static void test_update_race(void)
{
	codeplug_contact_edit_t e;
	uint16_t slot = 8;
	build();
	codeplug_contact_lists_begin(&e);
	contact(8, "Racing", 801, CONTACT_GROUP);
	put_check(table_check() ^ 0x55u);						// somebody else's write between
	uint32_t other = stored_check();
	uint32_t w = sim_flash_units();
	CHECK(!codeplug_contact_lists_end(&e, &slot, 1));
	CHECK(stored_check() == other && sim_flash_units() == w);
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE);				// void
	// The check key gone meanwhile, or made after begin saw none: the same
	build();
	codeplug_contact_lists_begin(&e);
	contact(8, "Racing", 801, CONTACT_GROUP);
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_LIST_CHECK));
	CHECK(!codeplug_contact_lists_end(&e, &slot, 1) && codeplug_kv_get(CODEPLUG_KEY_LIST_CHECK, NULL, 0) < 0);
	// A check written again with the value begin saw (an editor save that changed nothing) is not a race
	build();
	codeplug_contact_lists_begin(&e);
	put_check(stored_check());
	contact(8, "Quiet", 802, CONTACT_GROUP);
	CHECK(codeplug_contact_lists_end(&e, &slot, 1) && stored_check() == table_check());
	CHECK(sim_flash_violations() == 0);
}

// A codeplug whose membership is void is left as found by a radio-side edit, and one with none gets nothing
static void test_update_states(void)
{
	codeplug_contact_edit_t e;
	uint16_t slot = 10;
	uint8_t before[8], after[8];
	build();
	put_check(table_check() ^ 1u);							// void
	int n = codeplug_kv_get(CODEPLUG_KEY_LIST_DMR, before, sizeof(before));
	uint32_t check = stored_check();
	codeplug_contact_lists_begin(&e);
	contact(10, "While void", 1000, CONTACT_GROUP);
	CHECK(codeplug_contact_lists_end(&e, &slot, 1));
	CHECK(codeplug_kv_get(CODEPLUG_KEY_LIST_DMR, after, sizeof(after)) == n && memcmp(before, after, sizeof(before)) == 0);
	CHECK(stored_check() == check);
	CHECK(codeplug_contact_list_of(1) == CONTACT_LIST_NONE);				// still void
	// No membership at all: an edit writes nothing and makes no continuation
	fresh();
	codeplug_contact_lists_begin(&e);
	contact(1, "Plain", 55, CONTACT_GROUP);
	slot = 1;
	CHECK(codeplug_contact_lists_end(&e, &slot, 1));
	CHECK(sim_flash_data[CONT] == 0xFF && sim_flash_data[EE + CODEPLUG_KV_ADDR] == 0xFF);
}

// With the area unwritable (a later version's first sector) the contact edit stands and the area is left as it was
static void test_update_unwritable(void)
{
	codeplug_contact_edit_t e;
	uint16_t slot = 11;
	contact_t c;
	build();
	sim_flash_data[EE + CODEPLUG_KV_ADDR + 4] = CODEPLUG_KV_VERSION + 1;
	codeplug_reload();
	codeplug_contact_lists_begin(&e);
	contact(11, "Edited", 301, CONTACT_GROUP);
	CHECK(codeplug_contact_lists_end(&e, &slot, 1));				// no keys are read, so nothing to keep
	CHECK(codeplug_contact(11, &c) && c.id == 301);					// the contact is edited
	CHECK(sim_flash_data[EE + CODEPLUG_KV_ADDR + 4] == CODEPLUG_KV_VERSION + 1);	// and the area as it was
}

// A codeplug with no contact list key is not migrated: the lookup is the old one (first contact, then the ID database)
// whatever the selection says. Any key of the range, in any sector, ends that; taking the last one away brings it back
static void test_legacy(void)
{
	fresh();
	contact(1, "Local100", 100, CONTACT_GROUP);
	contact(2, "County100", 100, CONTACT_GROUP);
	contact(3, "Alice", 5000, CONTACT_PRIVATE);
	CHECK(strcmp(look(SEL(CONTACT_LIST_NONE, CONTACT_LIST_NONE), DMR, 100, true), "Local100") == 0);	// no selection: still named
	CHECK(strcmp(look(NULL, DMR, 100, true), "Local100") == 0);
	CHECK(strcmp(look(SEL(1, 2), DMR, 5000, false), "Alice") == 0);				// the selection is ignored
	CHECK(strcmp(look(NULL, DMR, 7777, false), "DB7777 Carol") == 0);			// no contact: the ID database
	iddb_asked = 0;
	CHECK(look(NULL, DMR, 7777, true)[0] == '\0' && iddb_asked == 0);			// not for a group
	CHECK(strcmp(look(NULL, DMR, 5000, false), "Alice") == 0 && iddb_asked == 0);	// the contact wins over the database
	CHECK(look(NULL, DMR, 4242, false)[0] == '\0');
	// Nothing in the lists' range is read as a selection or a network either
	uint8_t s[2];
	codeplug_system_lists(3, s);
	CHECK(s[0] == CONTACT_LIST_NONE && s[1] == CONTACT_LIST_NONE);
	CHECK(codeplug_channel_system(5) == CODEPLUG_SYSTEM_NONE);
#if CONFIG_AIR
	air_slot(1, AIR_CONTACT_GROUP, 1310, "Fire TG");
	CHECK(strcmp(look(NULL, P25, 1310, true), "Fire TG") == 0 && look(NULL, P25, 1311, true)[0] == '\0');
#endif
	// Any key of the range ends it: a list name, a selection, a network, a membership, the check alone
	static const uint16_t keys[] = { CODEPLUG_KEY_LIST_NAME + 20, CODEPLUG_KEY_SYS_LISTS + 9, CODEPLUG_KEY_CH_NET,
					 CODEPLUG_KEY_LIST_DMR, CODEPLUG_KEY_LIST_AIR, CODEPLUG_KEY_LIST_CHECK };
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		CHECK(codeplug_kv_set(keys[i], (const uint8_t[]){ 3, 3, 3, 3 }, 4));
		CHECK(look(SEL(CONTACT_LIST_NONE, CONTACT_LIST_NONE), DMR, 100, true)[0] == '\0');	// strict: no selection, no name
		CHECK(look(NULL, DMR, 5000, false)[0] == '\0');
		CHECK(strcmp(look(SEL(CONTACT_LIST_IDDB, CONTACT_LIST_NONE), DMR, 7777, false), "DB7777 Carol") == 0);
		CHECK(codeplug_kv_remove(keys[i]));
		CHECK(strcmp(look(NULL, DMR, 100, true), "Local100") == 0);			// the last one gone: legacy again
	}
	// A key that is empty or invalid is still a key
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_NAME + 1, "", 0));
	CHECK(look(NULL, DMR, 100, true)[0] == '\0');
	CHECK(sim_flash_violations() == 0);
}

#else

// Switch off: the first contact with the ID and call type, then the ID database, as before the lists; any selection
static void test_fallback(void)
{
	fresh();
	contact(1, "Local100", 100, CONTACT_GROUP);
	contact(2, "County100", 100, CONTACT_GROUP);
	contact(3, "Alice", 5000, CONTACT_PRIVATE);
	CHECK(strcmp(look(SEL(1, CONTACT_LIST_NONE), DMR, 100, true), "Local100") == 0);	// the selection is ignored
	CHECK(strcmp(look(NULL, DMR, 100, true), "Local100") == 0);
	CHECK(strcmp(look(SEL(CONTACT_LIST_NONE, CONTACT_LIST_NONE), DMR, 5000, false), "Alice") == 0);
	CHECK(strcmp(look(NULL, DMR, 7777, false), "DB7777 Carol") == 0);			// no contact: the ID database
	iddb_asked = 0;
	CHECK(look(NULL, DMR, 7777, true)[0] == '\0' && iddb_asked == 0);			// not for a group
	CHECK(look(NULL, DMR, 4242, false)[0] == '\0');
	CHECK(strcmp(look(NULL, DMR, 5000, false), "Alice") == 0 && iddb_asked == 0);	// the contact wins over the database
#if CONFIG_AIR
	air_slot(1, AIR_CONTACT_GROUP, 1310, "Fire TG");
	CHECK(strcmp(look(NULL, P25, 1310, true), "Fire TG") == 0 && look(NULL, P25, 1311, true)[0] == '\0');
#endif
	// The keys are left alone: a write of one still works (the editor edits them)
	CHECK(codeplug_kv_set(CODEPLUG_KEY_LIST_DMR, (const uint8_t[]){ 1 }, 1));
}

#endif

int main(void)
{
	test_registry();
#if CONFIG_CONTACT_LISTS
	test_names();
	test_selection();
	test_channel_network();
	test_delete_channel();
	test_membership();
	test_void();
	test_lookup();
#if CONFIG_AIR
	test_lookup_air();
#endif
	test_update();
	test_update_race();
	test_update_states();
	test_update_unwritable();
	test_legacy();
#else
	test_fallback();
#endif
	sim_flash_free();
	if (failures) {
		printf("codeplugContactListsTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("codeplugContactListsTest: all passed\n");
	return 0;
}
