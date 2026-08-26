// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// core/storage (the sector journal) and core/settings (the record ring) on a simulated NOR flash, with power cuts
// at every point of a save and of a sector rewrite: after the next boot the old or the new content is there, whole,
// never a mix; wear goes round the ring; old and new record sizes load.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "settings/settings.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x40000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.legacy_eeprom = 0, .legacy_flash = 0x20000u, .legacy_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

const board_t board = { .name = "test", .flash = &sim_flash };

static void boot(void)
{
	sim_flash_power_on();
	CHECK(storage_init());
	settings_init();
}

// --- the journal ---

static void fill(uint8_t *b, uint8_t seed)
{
	for (unsigned i = 0; i < STORAGE_SECTOR; i++) {
		b[i] = (uint8_t)(seed + i * 7u + (i >> 8));
	}
}

static void test_journal_basic(void)
{
	uint8_t a[STORAGE_SECTOR];
	storage_stats_t st;

	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	fill(a, 1);
	CHECK(storage_sector_write(0x3000u, a));
	CHECK(memcmp(&sim_flash_data[0x3000], a, STORAGE_SECTOR) == 0);
	CHECK(storage_sector_write(0x3000u, a));	// same content: skipped
	storage_get_stats(&st);
	CHECK(st.sector_writes == 1 && st.sector_skips == 1);
	CHECK(!storage_sector_write(0x3001u, a));	// unaligned
	CHECK(!storage_sector_write(map.journal_addr, a));	// the journal itself
	CHECK(!storage_sector_write(FLASH_SIZE, a));
	// A range across two sectors, merged with what is there
	uint8_t s[100];
	memset(s, 0x42, sizeof(s));
	CHECK(storage_write(0x3FD0u, s, sizeof(s)));
	CHECK(sim_flash_data[0x3FCF] == a[0xFCF] && sim_flash_data[0x3FD0] == 0x42 && sim_flash_data[0x4033] == 0x42 &&
	      sim_flash_data[0x4034] == 0xFF);
	// The shared buffer: refused while taken
	uint8_t *b = storage_buffer_acquire();
	CHECK(b != NULL && storage_buffer_acquire() == NULL);
	CHECK(!storage_write(0x5000u, s, 4));
	storage_buffer_release();
	CHECK(storage_write(0x5000u, s, 4));
	CHECK(sim_flash_violations() == 0);
}

// A power cut at every unit of a rewrite: after the boot (journal replay) the target is the old or the new sector,
// and the new one whenever the journal header was complete
static void test_journal_cuts(void)
{
	uint8_t old[STORAGE_SECTOR], neu[STORAGE_SECTOR];
	const uint32_t target = 0x6000u;
	uint32_t total;

	fill(old, 3);
	fill(neu, 99);
	sim_flash_create(FLASH_SIZE, &map);
	storage_init();
	storage_sector_write(target, old);
	uint32_t u0 = sim_flash_units();
	storage_sector_write(target, neu);
	total = sim_flash_units() - u0;

	int old_seen = 0, new_seen = 0, bad = 0;
	for (uint32_t cut = 1; cut <= total + 1; cut += (cut < 40 || total - cut < 40 || cut % 97 == 0) ? 1 : 13) {
		sim_flash_create(FLASH_SIZE, &map);
		storage_init();
		storage_sector_write(target, old);
		sim_flash_cut_after(cut);
		bool ok = storage_sector_write(target, neu);
		bool was_cut = sim_flash_is_cut();
		CHECK(ok != was_cut || (ok && !was_cut));
		boot();
		if (memcmp(&sim_flash_data[target], neu, STORAGE_SECTOR) == 0) {
			new_seen++;
		} else if (memcmp(&sim_flash_data[target], old, STORAGE_SECTOR) == 0) {
			old_seen++;
		} else {
			bad++;
			printf("FAIL journal: cut after %u of %u units leaves a mixed sector\n", cut, total);
		}
		// The journal is idle again: a later rewrite works
		CHECK(storage_sector_write(target, old));
		CHECK(memcmp(&sim_flash_data[target], old, STORAGE_SECTOR) == 0);
	}
	CHECK(bad == 0 && old_seen > 0 && new_seen > 0);
	failures += bad != 0;
	printf("journal: %d cut points, old %d new %d (rewrite = %u units)\n", old_seen + new_seen + bad, old_seen,
	       new_seen, total);
}

// --- settings ---

static void test_settings_basic(void)
{
	settings_t s;
	settings_stats_t st;

	sim_flash_create(FLASH_SIZE, &map);
	boot();
	settings_get_stats(&st);
	CHECK(st.load == SETTINGS_DEFAULTS && st.seq == 0);
	settings_read(&s);
	CHECK(s.brightness == 100 && s.squelch == SETTINGS_UNSET8 && s.beeps == 1);

	settings_t *w = settings_begin();
	w->vfo_rx_hz = 146520000;
	w->squelch = 3;
	strcpy(w->radio_name, "N0CALL");
	settings_end(1000);
	settings_poll(1500);			// not rested yet
	settings_get_stats(&st);
	CHECK(st.dirty && st.saves == 0);
	w = settings_begin();
	w->squelch = 4;				// another change restarts the wait
	settings_end(2500);
	settings_poll(4000);
	settings_get_stats(&st);
	CHECK(st.dirty && st.saves == 0);
	settings_poll(4500);
	settings_get_stats(&st);
	CHECK(!st.dirty && st.saves == 1 && st.seq == 1);
	// No change, no save
	settings_begin();
	settings_end(5000);
	settings_poll(9000);
	settings_get_stats(&st);
	CHECK(st.saves == 1);

	boot();
	settings_get_stats(&st);
	settings_read(&s);
	CHECK(st.load == SETTINGS_LOADED && st.seq == 1);
	CHECK(s.vfo_rx_hz == 146520000 && s.squelch == 4 && strcmp(s.radio_name, "N0CALL") == 0);

	// A change undone before the save: nothing written
	w = settings_begin();
	w->squelch = 9;
	settings_end(10000);
	w = settings_begin();
	w->squelch = 4;
	settings_end(10100);
	settings_get_stats(&st);
	CHECK(!st.dirty);
	CHECK(sim_flash_violations() == 0);
}

// Many saves: every sector of the ring is used, erases spread evenly, the newest always loads
static void test_settings_wear(void)
{
	settings_stats_t st;
	settings_t s;
	const unsigned sectors = map.settings_size / STORAGE_SECTOR;

	sim_flash_create(FLASH_SIZE, &map);
	boot();
	for (uint32_t i = 1; i <= 1000; i++) {
		settings_t *w = settings_begin();
		w->vfo_rx_hz = 430000000u + i;
		settings_end(i * 10);
		CHECK(settings_flush());
	}
	settings_get_stats(&st);
	CHECK(st.saves == 1000 && st.seq == 1000 && st.failures == 0);
	uint32_t lo = 0xFFFFFFFFu, hi = 0;
	for (unsigned k = 0; k < sectors; k++) {
		uint32_t e = sim_flash_erases(map.settings_addr + k * STORAGE_SECTOR);
		lo = e < lo ? e : lo;
		hi = e > hi ? e : hi;
	}
	CHECK(hi - lo <= 1 && lo >= 6);		// 1000 records / 16 per sector = 62 sector uses over 8 sectors
	printf("settings wear: 1000 saves, %u..%u erases per sector over %u sectors\n", lo, hi, sectors);
	boot();
	settings_read(&s);
	CHECK(s.vfo_rx_hz == 430001000u);
	CHECK(sim_flash_violations() == 0);
}

// A power cut at every unit of a save, at a sector boundary and inside a sector: the next boot loads the old or the
// new record, never defaults, and the next save works
static void cut_run(unsigned saves_before)
{
	settings_t s;
	uint32_t total;

	sim_flash_create(FLASH_SIZE, &map);
	boot();
	for (unsigned i = 1; i <= saves_before; i++) {
		settings_begin()->vfo_rx_hz = 1000u + i;
		settings_end(i);
		settings_flush();
	}
	uint8_t *snapshot = malloc(FLASH_SIZE);
	memcpy(snapshot, sim_flash_data, FLASH_SIZE);
	uint32_t u0 = sim_flash_units();
	settings_begin()->vfo_rx_hz = 777777u;
	settings_end(9999);
	settings_flush();
	total = sim_flash_units() - u0;

	int old_seen = 0, new_seen = 0;
	for (uint32_t cut = 1; cut <= total; cut++) {
		memcpy(sim_flash_data, snapshot, FLASH_SIZE);
		boot();
		settings_begin()->vfo_rx_hz = 777777u;
		settings_end(9999);
		sim_flash_cut_after(cut);
		settings_flush();
		boot();
		settings_read(&s);
		if (s.vfo_rx_hz == 777777u) {
			new_seen++;
		} else if (s.vfo_rx_hz == 1000u + saves_before) {
			old_seen++;
		} else {
			printf("FAIL settings: cut after %u of %u units (after %u saves) loads %u\n", cut, total,
			       saves_before, s.vfo_rx_hz);
			failures++;
		}
		// And it keeps working afterwards
		settings_begin()->vfo_rx_hz = 555u;
		settings_end(20000);
		CHECK(settings_flush());
		boot();
		settings_read(&s);
		CHECK(s.vfo_rx_hz == 555u);
	}
	CHECK(old_seen > 0 && new_seen > 0);
	printf("settings cuts after %u saves: %u cut points, old %d new %d\n", saves_before, total, old_seen, new_seen);
	free(snapshot);
}

static void test_settings_cuts(void)
{
	// (slot 0 of the ring holds the ownership mark, so save n goes to slot n)
	cut_run(1);		// inside a sector
	cut_run(15);		// the save that erases the next sector
	cut_run(8 * 16 - 1);	// round the ring: the erase of the first sector again
}

// Records from an older firmware (fewer fields) and a newer one (more), and an incompatible layout
static void test_settings_versions(void)
{
	settings_stats_t st;
	settings_t s;

	for (int variant = 0; variant < 3; variant++) {
		sim_flash_create(FLASH_SIZE, &map);
		uint8_t r[256];
		memset(r, 0xFF, sizeof(r));
		uint16_t n = variant == 0 ? 8 : variant == 1 ? 200 : 8;
		uint16_t layout = variant == 2 ? SETTINGS_LAYOUT + 1 : SETTINGS_LAYOUT;
		memcpy(r, "OSET", 4);
		r[4] = 5; r[5] = r[6] = r[7] = 0;
		r[8] = (uint8_t)layout; r[9] = 0;
		r[10] = (uint8_t)n; r[11] = 0;
		memset(&r[12], 0, n);
		r[12] = 1;		// vfo = 1
		r[13] = 7;		// squelch 7
		uint32_t crc = storage_crc32(0, r, 12u + n);
		memcpy(&r[12 + n], &crc, 4);	// host is little endian like the radio
		memset(&r[252], 0, 4);
		memcpy(&sim_flash_data[map.settings_addr + 3 * STORAGE_SECTOR], r, sizeof(r));
		boot();
		settings_get_stats(&st);
		settings_read(&s);
		if (variant < 2) {
			CHECK(st.load == SETTINGS_UPGRADED && st.seq == 5 && s.vfo == 1 && s.squelch == 7);
			if (variant == 0) {
				CHECK(s.brightness == 100 && s.beeps == 1);	// fields it didn't know: defaults
			}
			// The next save follows it in the same sector
			settings_begin()->squelch = 2;
			settings_end(1);
			CHECK(settings_flush());
			settings_get_stats(&st);
			CHECK(st.seq == 6 && st.sector == 3 && st.slot == 1);
		} else {
			CHECK(st.load == SETTINGS_DEFAULTS && s.squelch == SETTINGS_UNSET8);
		}
	}
	// Garbage in a slot (a torn write) is skipped, counted, never reused
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	settings_begin()->squelch = 1;
	settings_end(1);
	settings_flush();
	// (slot 0: the ownership mark storage_init wrote on the blank ring, slot 1: the record)
	sim_flash_data[map.settings_addr + 2 * 256 + 7] = 0x12;	// junk in slot 2
	boot();
	settings_get_stats(&st);
	CHECK(st.load == SETTINGS_LOADED && st.skipped == 1 && st.slot == 1);
	settings_begin()->squelch = 2;
	settings_end(2);
	CHECK(settings_flush());
	settings_get_stats(&st);
	CHECK(st.slot == 3);
	// No flash map: RAM only
	sim_flash.map = NULL;
	CHECK(!storage_init());
	CHECK(settings_init() == SETTINGS_RAM);
	settings_begin()->squelch = 3;
	settings_end(3);
	CHECK(!settings_flush());
	settings_read(&s);
	CHECK(s.squelch == 3);
}

// --- the foreign region guard ---

static void junk(uint32_t addr, uint32_t len, uint8_t seed)
{
	for (uint32_t i = 0; i < len; i++) {
		sim_flash_data[addr + i] = (uint8_t)(seed + i * 13u + (i >> 7));
	}
}

// A boot that may find the region foreign (boot() checks it isn't)
static bool boot_any(void)
{
	sim_flash_power_on();
	bool ok = storage_init();
	settings_init();
	return ok;
}

static void test_foreign(void)
{
	uint8_t a[STORAGE_SECTOR];
	settings_stats_t st;
	storage_stats_t ss;
	settings_t s;
	const uint32_t ring = map.settings_addr, jr = map.journal_addr;

	// Another firmware's data in the ring and in J0: foreign, nothing erased or programmed there, defaults in RAM
	sim_flash_create(FLASH_SIZE, &map);
	junk(ring + 2 * STORAGE_SECTOR, 3000, 7);
	junk(jr, 100, 9);
	uint8_t *snap = malloc(FLASH_SIZE);
	memcpy(snap, sim_flash_data, FLASH_SIZE);
	uint32_t u0 = sim_flash_units();
	CHECK(!boot_any());
	CHECK(storage_region() == STORAGE_REGION_FOREIGN && !storage_ready());
	settings_get_stats(&st);
	CHECK(st.load == SETTINGS_FOREIGN);
	settings_begin()->squelch = 5;
	settings_end(100);
	settings_poll(5000);
	CHECK(!settings_flush());
	settings_read(&s);
	CHECK(s.squelch == 5);			// the radio runs on, from RAM
	fill(a, 5);
	CHECK(!storage_sector_write(0x3000u, a));	// no journal use: codeplug edits and CPS commits fail
	CHECK(!storage_write(0x3000u, a, 16));
	storage_get_stats(&ss);
	CHECK(ss.foreign_refusals == 2 && ss.foreign_slots > 0 && !ss.claim_pending);
	CHECK(sim_flash_units() == u0 && memcmp(snap, sim_flash_data, FLASH_SIZE) == 0);
	// The same at the next boot (nothing was "repaired")
	CHECK(!boot_any() && storage_region() == STORAGE_REGION_FOREIGN);
	CHECK(memcmp(snap, sim_flash_data, FLASH_SIZE) == 0);
	settings_begin()->squelch = 5;		// (the boot started from the defaults again)
	settings_end(200);

	// The debug link's read window: the ring, the journal, the calibration copy; nothing else
	CHECK(storage_debug_read_allowed(ring, map.settings_size));
	CHECK(storage_debug_read_allowed(jr, 2 * STORAGE_SECTOR));
	CHECK(storage_debug_read_allowed(map.calibration_addr, map.calibration_size));
	CHECK(!storage_debug_read_allowed(ring - 1, 2));
	CHECK(!storage_debug_read_allowed(jr + 2 * STORAGE_SECTOR - 1, 2));
	CHECK(!storage_debug_read_allowed(map.calibration_addr + 0x1F0, 0x20));
	CHECK(!storage_debug_read_allowed(0x3000u, 16) && !storage_debug_read_allowed(ring, 0));
	CHECK(!storage_debug_read_allowed(0xFFFFFFF0u, 0x20));

	// Claim: the housekeeping erases the ring and the journal, then saves what the radio runs with
	CHECK(storage_claim_request() && storage_claim_pending());
	settings_poll(6000);
	CHECK(storage_region() == STORAGE_REGION_OURS && storage_ready() && !storage_claim_pending());
	settings_get_stats(&st);
	CHECK(!st.dirty && st.saves == 1);
	CHECK(sim_flash_data[jr] == 0xFF && sim_flash_data[jr + STORAGE_SECTOR + 5] == 0xFF);
	CHECK(sim_flash_data[ring + 2 * STORAGE_SECTOR + 5] == 0xFF);
	CHECK(memcmp(snap + 0x20000, sim_flash_data + 0x20000, FLASH_SIZE - 0x20000) == 0);	// nothing else touched
	CHECK(storage_sector_write(0x3000u, a));
	CHECK(boot_any());
	settings_get_stats(&st);
	settings_read(&s);
	CHECK(st.load == SETTINGS_LOADED && s.squelch == 5);
	CHECK(!storage_claim_request());	// ours: nothing to claim
	CHECK(sim_flash_violations() == 0);

	// Junk beside a valid record of ours (a torn erase in our own ring): ours, the junk is skipped
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	settings_begin()->squelch = 6;
	settings_end(1);
	CHECK(settings_flush());
	junk(ring + 3 * STORAGE_SECTOR, 2000, 3);
	CHECK(boot_any() && storage_region() == STORAGE_REGION_OURS);
	settings_read(&s);
	CHECK(s.squelch == 6);

	// A sector copy in J1 with J0 never written, nothing else: foreign
	sim_flash_create(FLASH_SIZE, &map);
	junk(jr + STORAGE_SECTOR, STORAGE_SECTOR, 1);
	CHECK(!boot_any() && storage_region() == STORAGE_REGION_FOREIGN);
	// Our J0 header (any J1) beside junk in the ring: ours
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(storage_sector_write(0x3000u, a));
	memset(sim_flash_data + ring, 0xFF, map.settings_size);	// no settings record (no mark either)
	junk(ring + 5 * STORAGE_SECTOR, 500, 2);
	CHECK(boot_any() && storage_region() == STORAGE_REGION_OURS);

	// A blank region gets the ownership mark; a cut in the middle of a later journal rewrite (J0 erased, J1 full)
	// then never looks foreign
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(storage_sector_write(0x3000u, a));
	fill(a, 77);
	u0 = sim_flash_units();
	sim_flash_cut_after(STORAGE_SECTOR + 2u);	// J1 erased and programmed, J0 erased, header not yet
	CHECK(!storage_sector_write(0x3000u, a));
	CHECK(sim_flash_data[jr] == 0xFF && sim_flash_data[jr + STORAGE_SECTOR] != 0xFF);
	CHECK(boot_any() && storage_region() == STORAGE_REGION_OURS);
	// A cut inside the very first mark (a torn magic): still ours
	for (uint32_t cut = 1; cut <= 6; cut++) {
		sim_flash_create(FLASH_SIZE, &map);
		sim_flash_cut_after(cut);
		storage_init();
		CHECK(boot_any() && storage_region() == STORAGE_REGION_OURS);
	}
	free(snap);
}

int main(void)
{
	test_journal_basic();
	test_journal_cuts();
	test_settings_basic();
	test_settings_wear();
	test_settings_cuts();
	test_settings_versions();
	test_foreign();
	sim_flash_free();
	printf("# %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
