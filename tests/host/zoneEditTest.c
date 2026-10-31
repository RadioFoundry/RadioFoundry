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
#include "settings/settings.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

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
	settings_init();
	codeplug_init();
}

// Power on again after a cut: the storage journal, the settings, the codeplug, then what the radio core does first
static void boot_again(void)
{
	sim_flash_power_on();
	CHECK(storage_init());
	settings_init();
	codeplug_init();
	zone_edit_settle();
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

	// A new empty zone (Zones > Create New Zone), then a channel added to it
	zone_op_t op;
	memset(&op, 0, sizeof op);
	op.kind = ZONE_OP_NEW;
	strcpy(op.name, "Empty");
	CHECK(zone_edit_apply(&op, &at) == ZONE_EDIT_OK && at == 3 && named(3, "Empty"));
	CHECK(codeplug_zone(3, &z) && z.count == 0);
	CHECK(zone_edit_insert(3, 0, 6) == ZONE_EDIT_OK && holds(3, (const uint16_t[]){ 6 }, 1));
	CHECK(zone_edit_new("", &at) == ZONE_EDIT_NO_NAME && codeplug_zone_count() == 4);

	// The same after a reboot
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_zone_count() == 4 && holds(1, (const uint16_t[]){ 21, 7, 20, 2, 3 }, 5));
	CHECK(named(3, "Empty") && holds(3, (const uint16_t[]){ 6 }, 1));
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
	uint16_t at = 0, out;
	uint32_t s0 = zone_edit_shifts();
	CHECK(zone_edit_apply(&op, &at) == ZONE_EDIT_OK && at == 1 && codeplug_zone_count() == 2);
	// A zone at or after the new one moves on; one before stays
	uint32_t s1 = zone_edit_shifts();
	CHECK(s1 == s0 + 1u && !zone_edit_busy());
	CHECK(zone_edit_map(s0, s1, 0, &out) == ZONE_MAP_KEPT && out == 0);
	CHECK(zone_edit_map(s0, s1, 1, &out) == ZONE_MAP_KEPT && out == 2);
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
	CHECK(zone_edit_shifts() == s1);	// edits inside a zone and a rename move no index
	op.kind = ZONE_OP_DELETE;
	op.zone = 0;
	CHECK(zone_edit_apply(&op, NULL) == ZONE_EDIT_OK && codeplug_zone_count() == 1 && named(0, "Renamed"));
	// After a delete the ones after it move back; the deleted index is the next zone now
	uint32_t s2 = zone_edit_shifts();
	CHECK(zone_edit_map(s1, s2, 0, &out) == ZONE_MAP_GONE && out == 0);
	CHECK(zone_edit_map(s1, s2, 3, &out) == ZONE_MAP_KEPT && out == 2);
	// Both entries at once: the copy (index 1) is index 0 now; the original zone 0 is gone
	CHECK(zone_edit_map(s0, s2, 0, &out) == ZONE_MAP_GONE && out == 0);
	// The last stored zone deleted: the radio shows the virtual one again, at the same index
	op.kind = ZONE_OP_DELETE;
	CHECK(zone_edit_apply(&op, NULL) == ZONE_EDIT_OK && !codeplug_zones_stored() && named(0, "All channels"));
	CHECK(zone_edit_map(s2, zone_edit_shifts(), 0, &out) == ZONE_MAP_GONE && out == 0);
	CHECK(zone_edit_apply(&op, NULL) == ZONE_EDIT_NO_ZONE);	// and that one can't be deleted
	op.kind = 0xEE;
	CHECK(zone_edit_apply(&op, NULL) == ZONE_EDIT_FAILED);
	CHECK(sim_flash_violations() == 0);
}

// The zone index log: mapping through several entries, a zone deleted then others added ahead of where it was, a
// reader too far behind
static void test_log(void)
{
	fresh();
	channel_t c = make("A", 145000000);
	CHECK(codeplug_set_channel(10, &c));
	zone_t z = zone_of("One", (const uint16_t[]){ 10 }, 1);
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);	// 0 All channels, 1 One
	z = zone_of("Two", (const uint16_t[]){ 10 }, 1);
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);	// 2 Two
	z = zone_of("Three", (const uint16_t[]){ 10 }, 1);
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);	// 3 Three
	uint32_t s0 = zone_edit_shifts();
	uint16_t out;
	CHECK(zone_edit_delete(1) == ZONE_EDIT_OK);		// One goes: Two 1, Three 2, slot 1 free
	z = zone_of("New", (const uint16_t[]){ 10 }, 1);
	uint16_t at = 99;
	CHECK(zone_edit_create(&z, &at) == ZONE_EDIT_OK && at == 1 && named(1, "New"));	// the free slot: ahead of Two
	uint32_t s1 = zone_edit_shifts();
	CHECK(s1 == s0 + 2u);
	CHECK(zone_edit_map(s0, s1, 0, &out) == ZONE_MAP_KEPT && out == 0);
	CHECK(zone_edit_map(s0, s1, 2, &out) == ZONE_MAP_KEPT && out == 2 && named(2, "Two"));
	CHECK(zone_edit_map(s0, s1, 3, &out) == ZONE_MAP_KEPT && out == 3 && named(3, "Three"));
	CHECK(zone_edit_map(s0, s1, 1, &out) == ZONE_MAP_GONE && out == 2);	// One's place went to Two, then New came
	CHECK(zone_edit_map(s1, s1, 3, &out) == ZONE_MAP_KEPT && out == 3);
	// The last zone deleted: its index is the count now (a reader clamps it)
	CHECK(zone_edit_delete(3) == ZONE_EDIT_OK);
	CHECK(zone_edit_map(s1, zone_edit_shifts(), 3, &out) == ZONE_MAP_GONE && out == 3 && codeplug_zone_count() == 3);
	// More entries than the log keeps
	uint32_t s2 = zone_edit_shifts();
	for (unsigned i = 0; i <= ZONE_SHIFT_LOG; i++) {
		CHECK(zone_edit_copy(0, NULL) == ZONE_EDIT_OK);
	}
	CHECK(zone_edit_map(s2, zone_edit_shifts(), 0, &out) == ZONE_MAP_LOST);
	CHECK(zone_edit_map(s2 + 1u, zone_edit_shifts(), 0, &out) == ZONE_MAP_KEPT);
	CHECK(!zone_edit_busy() && sim_flash_violations() == 0);
}

// A new zone is checked whole before anything is written (an import hands in any zone_t); Copy here's channel is
// taken back when its zone can't be written
static void test_refusals(void)
{
	fresh();
	channel_t c = make("A", 145000000);
	CHECK(codeplug_set_channel(10, &c));
	CHECK(codeplug_set_channel(11, &c));
	zone_t z = zone_of("One", (const uint16_t[]){ 10 }, 1);
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);
	uint16_t count = codeplug_zone_count();
	uint32_t s0 = zone_edit_shifts(), serial = codeplug_serial();
	z = zone_of("Bad", (const uint16_t[]){ 10, 99 }, 2);
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_NO_CHANNEL);
	z = zone_of("Twice", (const uint16_t[]){ 10, 11, 10 }, 3);
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_IN_ZONE);
	CHECK(codeplug_zone_count() == count && zone_edit_shifts() == s0 && codeplug_serial() == serial);
	// The zone records can't be written: the channel written for it is deleted again
	sim_flash_bad_range(0x8000u, 0x8000u);
	uint16_t n = codeplug_free_channel();
	CHECK(zone_edit_add_new(1, 0, n, &c) == ZONE_EDIT_FAILED);
	CHECK(!codeplug_channel(n, &c) && codeplug_free_channel() == n);
	sim_flash_bad_range(0, 0);
}

static int find_named(const char *name)
{
	zone_t z;
	for (uint16_t i = 0; i < codeplug_zone_count(); i++) {
		if (codeplug_zone(i, &z) && strcmp(z.name, name) == 0) {
			return i;
		}
	}
	return -1;
}

// A power cut through a zone add or delete that moves indexes: after the next power on the saved zone
// in use is the same zone (deleted: the one that took its index) and the roaming flags are on the same zones. The
// settings hold zone Three in use and Two, Three and Four roaming
enum { CUT_DELETE_AHEAD, CUT_DELETE_IN_USE, CUT_ADD_AHEAD, CUT_KINDS };

static void cut_setup(int kind)
{
	fresh();
	channel_t c = make("A", 145000000);
	CHECK(codeplug_set_channel(10, &c));
	static const char *const names[] = { "One", "Two", "Three", "Four" };
	for (unsigned i = 0; i < 4; i++) {
		zone_t z = zone_of(names[i], (const uint16_t[]){ 10 }, 1);
		CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);	// 0 All channels, 1 One .. 4 Four
	}
	if (kind == CUT_ADD_AHEAD) {
		CHECK(zone_edit_delete(1) == ZONE_EDIT_OK);	// slot 1 free: a new zone lands ahead of Two
	}
	settings_t *st = settings_begin();
	st->zone_index = (uint16_t)find_named("Three");
	st->zone_pos = 0;
	st->channel = 10;
	memset(st->roam_zones, 0, sizeof(st->roam_zones));
	for (unsigned i = 1; i < 4; i++) {
		int zi = find_named(names[i]);
		st->roam_zones[zi / 8] |= (uint8_t)(1u << (zi % 8));
	}
	settings_end(1);
	CHECK(settings_flush());
}

static void cut_op(int kind)
{
	if (kind == CUT_ADD_AHEAD) {
		zone_t z = zone_of("New", (const uint16_t[]){ 10 }, 1);
		zone_edit_create(&z, NULL);
	} else {
		zone_edit_delete(kind == CUT_DELETE_AHEAD ? 1 : 3);
	}
}

// The saved indexes against the zone list now. Returns false on a mismatch
static bool cut_consistent(int kind, uint32_t cut, uint32_t total)
{
	settings_t st;
	settings_read(&st);
	int three = find_named("Three");
	uint16_t n = codeplug_zone_count();
	uint16_t want = three >= 0 ? (uint16_t)three : (uint16_t)(3u < n ? 3u : n - 1u);
	bool ok = st.zone_index == want && st.zone_jrn_kind == SETTINGS_ZONE_JRN_NONE;
	for (uint16_t z = 0; z < SETTINGS_ROAM_ZONE_BYTES * 8u; z++) {
		bool flag = (st.roam_zones[z / 8u] & (1u << (z % 8u))) != 0;
		bool roaming = (int)z == find_named("Two") || (int)z == find_named("Three") || (int)z == find_named("Four");
		ok = ok && flag == roaming;
	}
	if (!ok) {
		printf("FAIL zone power cut: kind %d, cut after %u of %u units: zone_index %u (want %u), roam %02x\n", kind,
		       cut, total, st.zone_index, want, st.roam_zones[0]);
	}
	return ok;
}

static void test_power_cut(void)
{
	for (int kind = 0; kind < CUT_KINDS; kind++) {
		cut_setup(kind);
		uint8_t *snapshot = malloc(FLASH_SIZE);
		memcpy(snapshot, sim_flash_data, FLASH_SIZE);
		uint32_t u0 = sim_flash_units();
		cut_op(kind);
		uint32_t total = sim_flash_units() - u0;
		CHECK(total > 0);
		boot_again();
		CHECK(cut_consistent(kind, 0, total));		// no cut
		CHECK(find_named("Three") == (kind == CUT_DELETE_IN_USE ? -1 : kind == CUT_ADD_AHEAD ? 3 : 2));
		int done = 0, not_done = 0;
		// Every unit at the start and the end (the settings records), every 17th in the sector copies between
		for (uint32_t cut = 1; cut <= total; cut += cut < 600u || total - cut < 600u ? 1u : 17u) {
			memcpy(sim_flash_data, snapshot, FLASH_SIZE);
			boot_again();
			sim_flash_cut_after(cut);
			cut_op(kind);
			boot_again();
			if (!cut_consistent(kind, cut, total)) {
				failures++;
			}
			uint16_t n = codeplug_zone_count();
			if (kind == CUT_ADD_AHEAD ? n == 5 : n == 4) {
				done++;
			} else {
				not_done++;
			}
		}
		CHECK(done > 0 && not_done > 0);	// cuts on both sides of the zone write
		free(snapshot);
	}
	CHECK(sim_flash_violations() == 0);
}

int main(void)
{
	test_list();
	test_codeplug();
	test_full();
	test_builtin();
	test_apply_follow();
	test_log();
	test_refusals();
	test_power_cut();
	if (failures) {
		printf("zoneEditTest: %d failures\n", failures);
		return 1;
	}
	printf("zoneEditTest: all passed\n");
	return 0;
}
