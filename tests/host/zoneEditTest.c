// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone editing (core/codeplug/zone_edit.c) on the real codeplug over a simulated NOR flash: the zone_t helpers, a
// channel added, removed and moved, a new channel written and added in one step, zones created, copied, renamed and
// deleted, the virtual "All channels" zone kept when the first zone is made, where an added zone lands, the limits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/zone_edit.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

#define FLASH_SIZE	0x200000u
#define EE		0x000000u
#define FL		0x020000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
}

static channel_t make(const char *name, uint32_t hz)
{
	channel_t c;
	memset(&c, 0, sizeof(c));
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = c.tx_hz = hz;
	c.mode = RADIO_MODE_FM;
	c.bw = RADIO_BW_12K5;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	return c;
}

static zone_t zone_of(const char *name, const uint16_t *ch, uint16_t n)
{
	zone_t z;
	memset(&z, 0, sizeof(z));
	strncpy(z.name, name, CODEPLUG_NAME_LEN);
	for (uint16_t i = 0; i < n; i++)
		z.channels[z.count++] = ch[i];
	return z;
}

// The zone at index holds exactly these channels, in this order
static bool holds(uint16_t index, const uint16_t *ch, uint16_t n)
{
	zone_t z;
	if (!codeplug_zone(index, &z) || z.count != n)
		return false;
	for (uint16_t i = 0; i < n; i++)
		if (z.channels[i] != ch[i])
			return false;
	return true;
}

static bool named(uint16_t index, const char *name)
{
	zone_t z;
	return codeplug_zone(index, &z) && strcmp(z.name, name) == 0;
}

static void test_list(void)
{
	zone_t z = zone_of("L", (const uint16_t[]){ 5, 6, 7 }, 3);
	CHECK(zone_list_find(&z, 6) == 1 && zone_list_find(&z, 9) == -1);
	CHECK(zone_list_insert(&z, 0, 9, 80) == ZONE_EDIT_OK);			// first
	CHECK(zone_list_insert(&z, 4, 10, 80) == ZONE_EDIT_OK);		// appended
	CHECK(zone_list_insert(&z, 2, 11, 80) == ZONE_EDIT_OK);		// between
	CHECK(z.count == 6 && z.channels[0] == 9 && z.channels[1] == 5 && z.channels[2] == 11 && z.channels[3] == 6 &&
	      z.channels[5] == 10);
	CHECK(zone_list_insert(&z, 7, 12, 80) == ZONE_EDIT_NO_CHANNEL);	// past the end
	CHECK(zone_list_insert(&z, 0, 0, 80) == ZONE_EDIT_NO_CHANNEL);
	CHECK(zone_list_insert(&z, 0, 6, 80) == ZONE_EDIT_IN_ZONE);
	CHECK(zone_list_insert(&z, 0, 12, 6) == ZONE_EDIT_FULL);		// at its capacity
	CHECK(z.count == 6);
	CHECK(zone_list_move(&z, 0, -1) == ZONE_EDIT_EDGE && zone_list_move(&z, 5, 1) == ZONE_EDIT_EDGE);
	CHECK(zone_list_move(&z, 6, 1) == ZONE_EDIT_NO_CHANNEL);
	CHECK(zone_list_move(&z, 0, 1) == ZONE_EDIT_OK && z.channels[0] == 5 && z.channels[1] == 9);
	CHECK(zone_list_move(&z, 5, -1) == ZONE_EDIT_OK && z.channels[4] == 10 && z.channels[5] == 7);
	CHECK(zone_list_remove(&z, 6) == ZONE_EDIT_NO_CHANNEL);
	CHECK(zone_list_remove(&z, 0) == ZONE_EDIT_OK && z.count == 5 && z.channels[0] == 9 && z.channels[4] == 7);
	CHECK(zone_list_remove(&z, 4) == ZONE_EDIT_OK && z.count == 4 && z.channels[3] == 10);
	// A full zone_t
	zone_t f;
	memset(&f, 0, sizeof(f));
	for (uint16_t i = 0; i < CODEPLUG_ZONE_MAX; i++)
		f.channels[f.count++] = (uint16_t)(i + 1u);
	CHECK(zone_list_insert(&f, 0, 200, 999) == ZONE_EDIT_FULL);

	char name[CODEPLUG_NAME_LEN + 1];
	zone_copy_name(name, "Local");
	CHECK(strcmp(name, "Local 2") == 0);
	zone_copy_name(name, "Northeast Region");	// 16 characters: cut to 14
	CHECK(strcmp(name, "Northeast Regi 2") == 0 && strlen(name) == CODEPLUG_NAME_LEN);
	zone_copy_name(name, "");
	CHECK(strcmp(name, " 2") == 0);
}

// Channels 1-6 are the built-in set; 20 and 21 are written here
static void test_codeplug(void)
{
	fresh();
	channel_t c = make("Twenty", 145500000);
	CHECK(codeplug_set_channel(20, &c));
	c = make("Twenty-one", 146500000);
	CHECK(codeplug_set_channel(21, &c));
	CHECK(!codeplug_zones_stored() && codeplug_zone_count() == 1);

	// The first zone made keeps the virtual one: "All channels" is stored first, the new one after it
	zone_t z = zone_of("Local", (const uint16_t[]){ 20, 1, 3 }, 3);
	uint16_t at = 99;
	CHECK(zone_edit_create(&z, &at) == ZONE_EDIT_OK && at == 1);
	CHECK(codeplug_zones_stored() && codeplug_zone_count() == 2);
	CHECK(named(0, "All channels") && holds(0, (const uint16_t[]){ 1, 2, 3, 4, 5, 6, 20, 21 }, 8));
	CHECK(named(1, "Local") && holds(1, (const uint16_t[]){ 20, 1, 3 }, 3));

	// Add, move, remove
	uint32_t s0 = codeplug_serial();
	CHECK(zone_edit_insert(1, 1, 21) == ZONE_EDIT_OK);
	CHECK(codeplug_serial() != s0);
	CHECK(holds(1, (const uint16_t[]){ 20, 21, 1, 3 }, 4));
	CHECK(zone_edit_insert(1, 4, 2) == ZONE_EDIT_OK && holds(1, (const uint16_t[]){ 20, 21, 1, 3, 2 }, 5));
	CHECK(zone_edit_insert(1, 0, 21) == ZONE_EDIT_IN_ZONE);
	CHECK(zone_edit_insert(1, 0, 99) == ZONE_EDIT_NO_CHANNEL);		// not stored
	CHECK(zone_edit_insert(5, 0, 2) == ZONE_EDIT_NO_ZONE);
	CHECK(zone_edit_move(1, 0, 1) == ZONE_EDIT_OK && holds(1, (const uint16_t[]){ 21, 20, 1, 3, 2 }, 5));
	CHECK(zone_edit_move(1, 4, -1) == ZONE_EDIT_OK && holds(1, (const uint16_t[]){ 21, 20, 1, 2, 3 }, 5));
	CHECK(zone_edit_move(1, 4, 1) == ZONE_EDIT_EDGE && zone_edit_move(1, 0, -1) == ZONE_EDIT_EDGE);
	CHECK(zone_edit_remove(1, 2) == ZONE_EDIT_OK && holds(1, (const uint16_t[]){ 21, 20, 2, 3 }, 4));
	CHECK(zone_edit_remove(1, 4) == ZONE_EDIT_NO_CHANNEL);
	CHECK(codeplug_channel(1, &c));		// removed from the zone only: the channel stays

	// Rename
	CHECK(zone_edit_rename(1, "") == ZONE_EDIT_NO_NAME);
	CHECK(zone_edit_rename(1, "Hotspot and more!") == ZONE_EDIT_OK && named(1, "Hotspot and more"));	// cut to 16
	CHECK(zone_edit_rename(7, "X") == ZONE_EDIT_NO_ZONE);

	// Copy: the same channels, the name with " 2"
	CHECK(zone_edit_copy(1, &at) == ZONE_EDIT_OK && at == 2);
	CHECK(named(2, "Hotspot and mo 2") && holds(2, (const uint16_t[]){ 21, 20, 2, 3 }, 4));
	CHECK(zone_edit_copy(9, &at) == ZONE_EDIT_NO_ZONE);

	// A new channel written and added in one step, after position 0
	uint16_t free_n = codeplug_free_channel();
	CHECK(free_n == 7);
	c = make("Twenty-one 2", 146500000);
	CHECK(zone_edit_add_new(1, 1, free_n, &c) == ZONE_EDIT_OK);
	CHECK(holds(1, (const uint16_t[]){ 21, 7, 20, 2, 3 }, 5));
	channel_t back;
	CHECK(codeplug_channel(7, &back) && strcmp(back.name, "Twenty-one 2") == 0);
	CHECK(zone_edit_add_new(1, 0, 7, &c) == ZONE_EDIT_NO_FREE);	// a stored channel is never overwritten
	CHECK(zone_edit_add_new(1, 0, 0, &c) == ZONE_EDIT_NO_FREE);
	CHECK(zone_edit_add_new(1, 9, 8, &c) == ZONE_EDIT_NO_CHANNEL && !codeplug_channel(8, &back));

	// Delete, then a new zone takes the free record: it lands at index 0, not last
	CHECK(zone_edit_delete(0) == ZONE_EDIT_OK && codeplug_zone_count() == 2 && named(0, "Hotspot and more"));
	CHECK(zone_edit_delete(5) == ZONE_EDIT_NO_ZONE);
	z = zone_of("New", (const uint16_t[]){ 6 }, 1);
	CHECK(zone_edit_create(&z, &at) == ZONE_EDIT_OK && at == 0 && named(0, "New") && named(1, "Hotspot and more"));
	z.name[0] = 0;
	CHECK(zone_edit_create(&z, &at) == ZONE_EDIT_NO_NAME);

	// The same after a reboot
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_zone_count() == 3 && holds(1, (const uint16_t[]){ 21, 7, 20, 2, 3 }, 5));
	CHECK(codeplug_zone_capacity() == CODEPLUG_ZONE_MAX);
	CHECK(sim_flash_violations() == 0);
}

// A full zone refuses a new channel before writing it, so none is left behind in no zone
static void test_full(void)
{
	fresh();
	channel_t c = make("C", 145000000);
	zone_t z;
	memset(&z, 0, sizeof(z));
	strcpy(z.name, "Full");
	for (uint16_t n = 1; n <= CODEPLUG_ZONE_MAX; n++) {
		CHECK(codeplug_set_channel(n, &c));
		z.channels[z.count++] = n;
	}
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);
	uint16_t full = (uint16_t)(codeplug_zone_count() - 1u);
	CHECK(codeplug_zone(full, &z) && z.count == CODEPLUG_ZONE_MAX);
	CHECK(codeplug_set_channel(100, &c));
	CHECK(zone_edit_insert(full, 0, 100) == ZONE_EDIT_FULL);
	uint16_t n = codeplug_free_channel();
	CHECK(zone_edit_add_new(full, 0, n, &c) == ZONE_EDIT_FULL && !codeplug_channel(n, &c));
	// The zone limit
	uint16_t made = codeplug_zone_count();
	z = zone_of("Z", (const uint16_t[]){ 1 }, 1);
	while (made < CODEPLUG_ZONES_MAX && zone_edit_create(&z, NULL) == ZONE_EDIT_OK)
		made++;
	CHECK(made == CODEPLUG_ZONES_MAX && codeplug_zone_count() == CODEPLUG_ZONES_MAX);
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_ZONES_FULL);
	CHECK(zone_edit_copy(0, NULL) == ZONE_EDIT_ZONES_FULL);
	CHECK(sim_flash_violations() == 0);
}

// No zones stored, and the codeplug only the built-in set: the edits still work (they store it first)
static void test_builtin(void)
{
	fresh();
	CHECK(codeplug_source() == CODEPLUG_BUILTIN);
	CHECK(zone_edit_delete(0) == ZONE_EDIT_NO_ZONE);	// the virtual zone can't be deleted
	channel_t c = make("Copy", 146000000);
	uint16_t n = codeplug_free_channel();
	CHECK(zone_edit_add_new(0, 0, n, &c) == ZONE_EDIT_OK);
	CHECK(codeplug_source() == CODEPLUG_OPENFW && codeplug_zones_stored() && codeplug_zone_count() == 1);
	zone_t z;
	CHECK(codeplug_zone(0, &z) && z.channels[0] == n && z.count == 7);	// first, not twice
	CHECK(zone_edit_rename(0, "Mine") == ZONE_EDIT_OK && named(0, "Mine"));

	// Copy the virtual zone of a stored codeplug without zones
	fresh();
	CHECK(codeplug_set_channel(9, &c));
	uint16_t at = 99;
	CHECK(zone_edit_copy(0, &at) == ZONE_EDIT_OK && at == 1);
	CHECK(named(0, "All channels") && named(1, "All channels 2") && holds(1, (const uint16_t[]){ 1, 2, 3, 4, 5, 6, 9 }, 7));
	CHECK(sim_flash_violations() == 0);
}

static void test_apply_follow(void)
{
	fresh();
	channel_t c = make("A", 145000000);
	CHECK(codeplug_set_channel(10, &c));
	zone_op_t op;
	memset(&op, 0, sizeof(op));
	op.kind = ZONE_OP_COPY;
	op.zone = 0;
	uint16_t at = 0;
	CHECK(zone_edit_apply(&op, &at) == ZONE_EDIT_OK && at == 1 && codeplug_zone_count() == 2);
	// A zone at or after the new one moves on; one before stays
	CHECK(zone_edit_follow(&op, 1, 0) == 0 && zone_edit_follow(&op, 1, 1) == 2);
	op.kind = ZONE_OP_RENAME;
	strcpy(op.name, "Renamed");
	op.zone = 1;
	CHECK(zone_edit_apply(&op, &at) == ZONE_EDIT_OK && at == 1 && named(1, "Renamed"));
	op.kind = ZONE_OP_INSERT;
	op.zone = 1;
	op.pos = 0;
	op.channel = 10;
	CHECK(zone_edit_apply(&op, NULL) == ZONE_EDIT_IN_ZONE);	// the copy of All channels has it
	op.kind = ZONE_OP_REMOVE;
	CHECK(zone_edit_apply(&op, NULL) == ZONE_EDIT_OK);
	op.kind = ZONE_OP_MOVE;
	op.dir = 1;
	CHECK(zone_edit_apply(&op, NULL) == ZONE_EDIT_OK);
	op.kind = ZONE_OP_ADD_NEW;
	op.channel = codeplug_free_channel();
	op.ch = make("New", 146000000);
	CHECK(zone_edit_apply(&op, NULL) == ZONE_EDIT_OK);
	// 1-6 and 10, less the first, the next two swapped, the new one first
	CHECK(holds(1, (const uint16_t[]){ op.channel, 3, 2, 4, 5, 6, 10 }, 7));
	op.kind = ZONE_OP_DELETE;
	op.zone = 0;
	CHECK(zone_edit_apply(&op, NULL) == ZONE_EDIT_OK && codeplug_zone_count() == 1 && named(0, "Renamed"));
	// After a delete the ones after it move back; the deleted index is the next zone now
	CHECK(zone_edit_follow(&op, 0, 0) == 0 && zone_edit_follow(&op, 0, 3) == 2);
	op.kind = 0xEE;
	CHECK(zone_edit_apply(&op, NULL) == ZONE_EDIT_FAILED);
	CHECK(sim_flash_violations() == 0);
}

int main(void)
{
	test_list();
	test_codeplug();
	test_full();
	test_builtin();
	test_apply_follow();
	if (failures) {
		printf("zoneEditTest: %d failures\n", failures);
		return 1;
	}
	printf("zoneEditTest: all passed\n");
	return 0;
}
