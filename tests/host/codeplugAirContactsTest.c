// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The air mode contact table (docs/spec/air-contacts.md, core/codeplug/codeplug_air_contacts.c) on a simulated NOR
// flash: entries read by mode, call type and ID; the entries skipped (empty, another mode, a bad call type, an ID out of
// range, no name); the first of two duplicates; the last slot; another firmware's codeplug read as an empty table, and
// Start new erasing it
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_air_contacts.h"
#include "codeplug/codeplug_modes.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

#define FLASH_SIZE	0x200000u
#define EE		0x000000u
#define FL		0x020000u
#define TABLE		(EE + CODEPLUG_AIR_CONTACTS_ADDR)
#define P25		CODEPLUG_KIND_P25

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

// Slot n's bytes as the Codeplug Editor writes them (air-contacts.md 3)
static void put(uint16_t n, uint8_t mode, uint8_t flags, uint32_t id, const char *name)
{
	uint8_t *r = &sim_flash_data[TABLE + (n - 1u) * CODEPLUG_AIR_CONTACT_SIZE];
	r[0] = mode;
	r[1] = flags;
	r[2] = (uint8_t)id;
	r[3] = (uint8_t)(id >> 8);
	r[4] = (uint8_t)(id >> 16);
	memset(&r[5], 0xFF, 3);
	memset(&r[8], 0xFF, CODEPLUG_NAME_LEN);
	memcpy(&r[8], name, strlen(name));
}

static bool named(uint32_t id, bool group, const char *name)
{
	air_contact_t c;
	return codeplug_air_contact_find(P25, id, group, &c) && strcmp(c.name, name) == 0;
}

static bool unnamed(uint32_t id, bool group)
{
	air_contact_t c;
	return !codeplug_air_contact_find(P25, id, group, &c);
}

static void test_entries(void)
{
	air_contact_t c;

	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
	CHECK(!codeplug_foreign());
	CHECK(unnamed(1310, true) && !codeplug_air_contact(1, P25, &c));	// erased: an empty table

	put(1, P25, AIR_CONTACT_GROUP, 1310, "1 SB DPS");
	put(2, P25, AIR_CONTACT_UNIT, 1310, "Engine 7");	// the same number as a unit
	put(3, P25, AIR_CONTACT_ALL, 0, "Everyone");
	put(4, 0x04, AIR_CONTACT_GROUP, 2000, "NXDN grp");	// another mode: not a P25 name
	put(5, P25, 3, 2001, "Bad type");
	put(6, P25, AIR_CONTACT_GROUP, 0x10000, "Too wide");	// a P25 group is 16 bits
	put(7, P25, AIR_CONTACT_UNIT, 0xFFFFFF, "Bad unit");
	put(8, P25, AIR_CONTACT_GROUP, 2002, "");		// no name: names nothing
	put(9, P25, AIR_CONTACT_GROUP, 2003, "First");
	put(10, P25, AIR_CONTACT_GROUP, 2003, "Second");		// a duplicate: never used
	put(11, P25, 0xF0 | AIR_CONTACT_GROUP, 2004, "Bits kept");	// reserved bits don't stop it
	put(CODEPLUG_AIR_CONTACTS_MAX, P25, AIR_CONTACT_UNIT, 0xFFFFFE, "Last slot");
	memset(&sim_flash_data[TABLE + CODEPLUG_AIR_CONTACTS_MAX * CODEPLUG_AIR_CONTACT_SIZE], 0x02, 8);	// past the end

	CHECK(named(1310, true, "1 SB DPS") && named(1310, false, "Engine 7"));	// group and unit apart
	CHECK(named(0xFFFF, true, "Everyone"));		// the all call names talk group 0xFFFF
	CHECK(unnamed(2000, true) && unnamed(2001, true) && unnamed(2001, false));
	CHECK(unnamed(0x10000, true) && unnamed(0xFFFFFF, false) && unnamed(2002, true));
	CHECK(named(2003, true, "First") && named(2004, true, "Bits kept"));
	CHECK(named(0xFFFFFE, false, "Last slot"));
	CHECK(unnamed(77, true));				// no entry: no name

	// By slot: a valid entry of the mode asked, with its name ("" for one without)
	CHECK(codeplug_air_contact(1, P25, &c) && c.type == AIR_CONTACT_GROUP && c.id == 1310);
	CHECK(codeplug_air_contact(3, P25, &c) && c.type == AIR_CONTACT_ALL && c.id == 0xFFFF);
	CHECK(codeplug_air_contact(8, P25, &c) && c.name[0] == '\0');
	CHECK(!codeplug_air_contact(4, P25, &c) && !codeplug_air_contact(5, P25, &c) && !codeplug_air_contact(12, P25, &c));
	CHECK(!codeplug_air_contact(0, P25, &c) && !codeplug_air_contact(CODEPLUG_AIR_CONTACTS_MAX + 1u, P25, &c));
	CHECK(codeplug_air_contact(CODEPLUG_AIR_CONTACTS_MAX, P25, &c) && c.id == 0xFFFFFE);
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();
}

// Another firmware's codeplug (as codeplugEditTest's): pseudo-random bytes over both open-firmware images, and 16
// stock-style channels that make it foreign
static void foreign_image(void)
{
	uint32_t x = 99;
	for (uint32_t a = 0; a < 0xB0000u; a++) {
		x = x * 1103515245u + 12345u;
		if (a < 0x10000u || a >= FL) {
			sim_flash_data[a] = (uint8_t)(x >> 24);
		}
	}
	sim_flash_data[EE + 0x3780] = sim_flash_data[EE + 0x3781] = 0xFF;
	for (unsigned n = 0; n < 16; n++) {
		uint8_t *r = &sim_flash_data[EE + 0x3790 + 56u * n];
		memset(r, 0, 56);
		memcpy(r, "C\0h\0a\0n\0", 8);
		static const uint8_t f[4] = { 0x00, 0x00, 0x85, 0x43 };
		memcpy(r + 0x10, f, 4);
		memcpy(r + 0x14, f, 4);
		r[0x18] = 1;
	}
}

static void test_foreign(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	foreign_image();
	put(1, P25, AIR_CONTACT_GROUP, 1310, "Not ours");	// a valid entry by chance in the other firmware's bytes
	CHECK(storage_init());
	codeplug_accept(false);
	codeplug_init();
	CHECK(codeplug_foreign() && unnamed(1310, true));

	// Start new erases the table with the rest: nothing of the old bytes comes back as a name
	CHECK(codeplug_start_new() && !codeplug_foreign());
	for (uint32_t i = 0; i < CODEPLUG_AIR_CONTACTS_LEN; i++) {
		if (sim_flash_data[TABLE + i] != 0xFF) {
			CHECK(!"the air mode contact table erased by Start new");
			break;
		}
	}
	CHECK(unnamed(1310, true));
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();
}

int main(void)
{
	test_entries();
	test_foreign();
	printf("# codeplugAirContactsTest: %s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
