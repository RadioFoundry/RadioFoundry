// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stored scan lists (core/codeplug/scan_list.c, docs/spec/scan-list.md) as records of the codeplug settings area on a
// simulated NOR flash: records in and out unchanged, refused lists, replace, delete and the free slot it leaves, full,
// keys of other settings kept beside the lists, a later firmware's area, the shared buffer taken, and a power cut at
// every unit of a write: after the next boot the old list or the new one, never a mix
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/scan_list.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

#define FLASH_SIZE	0x200000u
#define EE		0x000000u
#define FL		0x020000u
#define AREA		(EE + CODEPLUG_KV_ADDR)

static hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

// A list named name with the channels first, first + 1, ... (n of them)
static scan_list_t make(const char *name, uint16_t first, uint16_t n)
{
	scan_list_t l;
	memset(&l, 0, sizeof l);
	snprintf(l.name, sizeof l.name, "%s", name);
	l.count = n;
	for (uint16_t i = 0; i < n; i++)
		l.channels[i] = (uint16_t)(first + i);
	return l;
}

// Field by field: the structs' padding holds whatever was on the stack
static bool same(const scan_list_t *a, const scan_list_t *b)
{
	return strcmp(a->name, b->name) == 0 && a->count == b->count &&
	       memcmp(a->channels, b->channels, a->count * sizeof a->channels[0]) == 0;
}

static void boot(void)
{
	sim_flash_power_on();
	CHECK(storage_init());
	codeplug_init();
}

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
}

static void test_codec(void)
{
	scan_list_t a = make("SATS", 1, 4), d;
	uint8_t b[SCAN_LIST_REC_MAX];
	unsigned n = scan_list_encode(&a, b);
	CHECK(n == SCAN_LIST_HEAD + 8u && scan_list_decode(b, n, &d) && same(&a, &d));
	// The layout the spec gives: flags, count, the name NUL padded, the channels little endian
	CHECK(b[0] == 0 && b[1] == 4 && memcmp(b + 2, "SATS", 5) == 0 && b[17] == 0 && b[18] == 1 && b[19] == 0 && b[24] == 4);
	// Fields a later layout appends are ignored
	uint8_t longer[SCAN_LIST_REC_MAX + 8];
	memcpy(longer, b, n);
	memset(longer + n, 0x5A, 8);
	CHECK(scan_list_decode(longer, n + 8u, &d) && same(&a, &d));
	// A record that is cut short, or names more channels than it holds, is not a list
	CHECK(!scan_list_decode(b, n - 1u, &d) && !scan_list_decode(b, SCAN_LIST_HEAD - 1u, &d) && !scan_list_decode(b, 0, &d));
	b[1] = SCAN_LIST_MAX + 1u;
	CHECK(!scan_list_decode(b, sizeof b, &d));
	// The widest list: 80 channels, a name that fills the field with no terminator in the record
	scan_list_t full = make("ABCDEFGHIJKLMNOP", 1000, SCAN_LIST_MAX);
	n = scan_list_encode(&full, b);
	CHECK(n == SCAN_LIST_REC_MAX && scan_list_decode(b, n, &d) && same(&full, &d) && strlen(d.name) == 16);
	scan_list_t none = make("EMPTY", 0, 0);
	n = scan_list_encode(&none, b);
	CHECK(n == SCAN_LIST_HEAD && scan_list_decode(b, n, &d) && d.count == 0);
	// Lists the store refuses: no name, a channel 0, a channel twice
	scan_list_t bad = make("", 1, 1);
	CHECK(scan_list_encode(&bad, b) == 0 && !scan_list_valid(&bad));
	bad = make("ZERO", 0, 3);
	CHECK(scan_list_encode(&bad, b) == 0);
	bad = make("TWICE", 5, 3);
	bad.channels[2] = 5;
	CHECK(scan_list_encode(&bad, b) == 0);
	bad = make("TOOMANY", 1, 3);
	bad.count = SCAN_LIST_MAX + 1u;
	CHECK(scan_list_encode(&bad, b) == 0);
}

static void test_store(void)
{
	scan_list_t r;
	fresh();
	CHECK(scan_list_usable() && scan_list_slots() == SCAN_LIST_SLOTS && scan_list_count() == 0);
	CHECK(!scan_list_read(0, &r) && !scan_list_delete(0) && !scan_list_read(SCAN_LIST_SLOTS, &r));

	scan_list_t a = make("SATS", 1, 4), b = make("REPEATERS", 20, 12), c = make("SIMPLEX", 100, 3);
	CHECK(scan_list_add(&a) == 0 && scan_list_add(&b) == 1 && scan_list_add(&c) == 2);
	CHECK(scan_list_count() == 3 && scan_list_read(1, &r) && same(&r, &b));
	// A write replaces what is in the slot, in place
	b.channels[b.count++] = 500;
	CHECK(scan_list_write(1, &b) && scan_list_count() == 3 && scan_list_read(1, &r) && same(&r, &b) && r.count == 13);
	// A write to a free slot fills it; delete frees a slot and the next add takes it
	scan_list_t d = make("LATER", 7, 2);
	CHECK(scan_list_write(10, &d) && scan_list_count() == 4 && scan_list_read(10, &r) && same(&r, &d));
	CHECK(scan_list_delete(1) && scan_list_count() == 3 && !scan_list_read(1, &r) && !scan_list_delete(1));
	CHECK(scan_list_read(0, &r) && same(&r, &a) && scan_list_read(2, &r) && same(&r, &c));	// the others keep their slots
	scan_list_t e = make("NEW", 50, 1);
	CHECK(scan_list_add(&e) == 1);
	// Refused lists and slots change nothing
	scan_list_t bad = make("", 1, 1);
	uint32_t gen = scan_list_generation();
	CHECK(!scan_list_write(3, &bad) && scan_list_add(&bad) == -1 && !scan_list_write(SCAN_LIST_SLOTS, &a));
	CHECK(scan_list_generation() == gen && scan_list_count() == 4);
	// A change moves the generation, and so does a change of the area that isn't ours
	CHECK(scan_list_write(3, &e) && scan_list_generation() != gen);
	gen = scan_list_generation();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 12345) && scan_list_generation() != gen);

	// Fill every slot with the widest list: one more doesn't fit, a replacement still does
	scan_list_t wide = make("WIDE", 1, SCAN_LIST_MAX);
	for (uint16_t i = 0; i < SCAN_LIST_SLOTS; i++)
		CHECK(scan_list_write(i, &wide));
	CHECK(scan_list_count() == SCAN_LIST_SLOTS && scan_list_add(&a) == -1);
	CHECK(scan_list_write(15, &a) && scan_list_read(15, &r) && same(&r, &a));
	CHECK(scan_list_write(2, &c) && scan_list_write(10, &d));
	CHECK(sim_flash_violations() == 0);

	// Survives a reboot
	boot();
	CHECK(scan_list_count() == SCAN_LIST_SLOTS && scan_list_read(2, &r) && same(&r, &c) && scan_list_read(10, &r) && same(&r, &d));

	// The shared sector buffer taken (a codeplug write): refused, nothing changed
	uint8_t *buf = storage_buffer_acquire();
	CHECK(buf != NULL && !scan_list_delete(2) && !scan_list_write(2, &a) && scan_list_read(2, &r) && same(&r, &c));
	storage_buffer_release();
	sim_flash_free();
}

// The lists share the area with the other keyed settings: neither disturbs the other, and a full area refuses a list
// without losing what is there
static void test_shared_area(void)
{
	scan_list_t r, a = make("SATS", 1, 4), b = make("REPEATERS", 20, 12);
	fresh();
	CHECK(codeplug_kv_set_u32(CODEPLUG_KEY_ARS_SERVER, 4711) && codeplug_kv_set_u32(CODEPLUG_KEY_IP_GATEWAY, 99));
	CHECK(scan_list_add(&a) == 0 && scan_list_add(&b) == 1);
	uint8_t v[4];
	CHECK(codeplug_kv_get(CODEPLUG_KEY_ARS_SERVER, v, sizeof v) == 4 && v[0] == (4711 & 0xFF) && v[1] == (4711 >> 8));
	CHECK(scan_list_delete(0) && codeplug_kv_get(CODEPLUG_KEY_IP_GATEWAY, v, sizeof v) == 4 && v[0] == 99);
	CHECK(scan_list_read(1, &r) && same(&r, &b));
	// Another setting's key between the lists' range and past it doesn't count as a list
	CHECK(codeplug_kv_set_u32((uint16_t)(CODEPLUG_KEY_SCAN_LIST + SCAN_LIST_SLOTS), 1) && scan_list_count() == 1);
	// A key of ours holding something that is not a record: the slot reads as free and add takes it
	static const uint8_t junk[3] = { 1, 2, 3 };
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SCAN_LIST + 0, junk, sizeof junk) && !scan_list_read(0, &r) && scan_list_count() == 1);
	CHECK(scan_list_add(&a) == 0 && scan_list_read(0, &r) && same(&r, &a));
	// A full area: another setting takes the room, a list that doesn't fit is refused, what was stored stays
	static uint8_t big[3900];
	memset(big, 0x77, sizeof big);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_TARGET, big, sizeof big));
	scan_list_t wide = make("WIDE", 1, SCAN_LIST_MAX);
	CHECK(scan_list_add(&wide) == -1 && !scan_list_write(5, &wide));
	CHECK(scan_list_count() == 2 && scan_list_read(0, &r) && same(&r, &a) && scan_list_read(1, &r) && same(&r, &b));
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();
}

static void test_unusable(void)
{
	scan_list_t a = make("SATS", 1, 4), r;
	// A later firmware's area is never written: the lists are not usable
	fresh();
	CHECK(scan_list_add(&a) == 0);
	uint8_t *s = &sim_flash_data[AREA];
	s[4] = 2;							// version 2: ours, but newer than this reader
	CHECK(!scan_list_usable() && scan_list_slots() == 0 && scan_list_count() == 0);
	CHECK(!scan_list_read(0, &r) && scan_list_add(&a) == -1 && !scan_list_write(0, &a) && !scan_list_delete(0));
	sim_flash_free();

	// A board with no flash map gets no lists
	sim_flash_create(FLASH_SIZE, NULL);
	CHECK(!scan_list_usable() && scan_list_slots() == 0 && scan_list_add(&a) == -1);
	sim_flash_free();
}

// A power cut at every unit of replacing a list: after the boot (the journal's replay) the store holds the old list
// or the new one, whole, and the others unchanged
static void test_power_cut(void)
{
	scan_list_t r, a = make("SATS", 1, 4), b = make("REPEATERS", 20, 12), a2 = make("SATS", 1, 9);
	fresh();
	CHECK(scan_list_add(&a) == 0 && scan_list_add(&b) == 1);
	uint32_t before = sim_flash_units();
	CHECK(scan_list_write(0, &a2));
	uint32_t units = sim_flash_units() - before;
	CHECK(scan_list_write(0, &a));
	unsigned olds = 0, news = 0;
	for (uint32_t cut = 1; cut <= units; cut++) {
		sim_flash_cut_after(cut);
		scan_list_write(0, &a2);
		sim_flash_cut_after(0);
		boot();
		CHECK(scan_list_count() == 2);
		CHECK(scan_list_read(1, &r) && same(&r, &b));
		CHECK(scan_list_read(0, &r));
		if (same(&r, &a))
			olds++;
		else if (same(&r, &a2))
			news++;
		else
			CHECK(!"a mix");
		CHECK(scan_list_write(0, &a));
	}
	printf("scanListTest: a write is %u units; cut at each: %u old, %u new\n", (unsigned)units, olds, news);
	CHECK(olds > 0 && news > 0);
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();
}

int main(void)
{
	test_codec();
	test_store();
	test_shared_area();
	test_unusable();
	test_power_cut();
	printf("scanListTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
