// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// core/storage (the sector journal) and core/settings (the record ring) on a simulated NOR flash, with power cuts
// at every point of a save and of a sector rewrite: after the next boot the old or the new content is there, whole,
// never a mix; wear goes round the ring; old and new record sizes load.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
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
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
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

// The native area on factory reset (storage_native_erase): blank stays blank, our data (the magic) is erased with
// the header sector last, anything else is left alone; a cut in the middle can be finished by running it again.
// And the deferred job the UI hands to the housekeeping task
static const hal_flash_map_t map_native = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x30000u,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
	.native_addr = 0x30000u, .native_size = 0x8000u,
};

static int job_runs;
static void job(void *ctx) { job_runs += *(int *)ctx; }

static void test_native_and_jobs(void)
{
	const uint32_t na = map_native.native_addr, ns = map_native.native_size;
	static const uint8_t magic[4] = { 'O', 'D', 'R', 'N' };
	uint8_t junk[16];
	memset(junk, 0x5A, sizeof(junk));

	sim_flash_create(FLASH_SIZE, &map_native);
	boot();
	CHECK(storage_native_erase() == STORAGE_NATIVE_BLANK);
	// Another firmware's bytes, no magic: untouched
	sim_flash.write(na + 0x5000u, junk, sizeof(junk));
	uint32_t e0 = sim_flash_units();
	CHECK(storage_native_erase() == STORAGE_NATIVE_FOREIGN);
	CHECK(sim_flash_units() == e0 && sim_flash_data[na + 0x5000u] == 0x5A);
	// Ours: everything erased, the header sector last
	sim_flash.write(na, magic, sizeof(magic));
	sim_flash.write(na + 0x2000u, junk, sizeof(junk));
	CHECK(storage_native_erase() == STORAGE_NATIVE_ERASED);
	for (uint32_t a = na; a < na + ns; a++) {
		if (sim_flash_data[a] != 0xFF) {
			CHECK(!"native area not blank");
			break;
		}
	}
	CHECK(sim_flash_erases(na + 0x7000u) == 0 && sim_flash_erases(na + 0x5000u) == 1 && sim_flash_erases(na) == 1);
	// A cut after the first erase: the magic is still there, a second run finishes
	sim_flash.write(na, magic, sizeof(magic));
	sim_flash.write(na + 0x6000u, junk, sizeof(junk));
	sim_flash.write(na + 0x1000u, junk, sizeof(junk));
	sim_flash_cut_after(1);
	CHECK(storage_native_erase() == STORAGE_NATIVE_FAILED);
	sim_flash_power_on();
	CHECK(memcmp(&sim_flash_data[na], magic, 4) == 0 && sim_flash_data[na + 0x6000u] == 0xFF);
	CHECK(storage_native_erase() == STORAGE_NATIVE_ERASED && sim_flash_data[na + 0x1000u] == 0xFF);
	// No native area in the map: refused
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(storage_native_erase() == STORAGE_NATIVE_FAILED);

	// Jobs: one at a time, run by the poll, then the next may come
	int one = 1;
	CHECK(!storage_job_busy() && storage_job_post(job, &one));
	CHECK(storage_job_busy() && !storage_job_post(job, &one));
	storage_job_poll();
	CHECK(job_runs == 1 && !storage_job_busy());
	storage_job_poll();
	CHECK(job_runs == 1);
	CHECK(!storage_job_post(NULL, NULL));
	CHECK(sim_flash_violations() == 0);
}


// --- records and the ring at their limits ---

// A record built from the format in docs/spec/settings-store.md 2, written straight into the simulated flash
static void put_record(const hal_flash_map_t *m, unsigned sector, unsigned slot, uint32_t seq, uint16_t layout,
		       uint16_t n, const void *payload, bool commit, bool crc_ok)
{
	uint8_t r[256];
	memset(r, 0xFF, sizeof(r));
	memcpy(r, "OSET", 4);
	memcpy(&r[4], &seq, 4);		// the host is little endian like the radios
	memcpy(&r[8], &layout, 2);
	memcpy(&r[10], &n, 2);
	if (n <= 236) {
		memcpy(&r[12], payload, n);
		uint32_t crc = storage_crc32(0, r, 12u + n) ^ (crc_ok ? 0 : 0x10u);
		memcpy(&r[12 + n], &crc, 4);
	}
	if (commit) {
		memset(&r[252], 0, 4);
	}
	memcpy(&sim_flash_data[m->settings_addr + sector * STORAGE_SECTOR + slot * 256u], r, sizeof(r));
}

// The payload is settings_t as in RAM, and layout 1 is frozen: fields are only appended (settings-store.md 3). A
// field that moved would make every stored record load into the wrong fields
static void test_settings_layout(void)
{
	static const struct { size_t got, want; } offs[] = {
		{ offsetof(settings_t, vfo), 0 },
		{ offsetof(settings_t, squelch), 1 },
		{ offsetof(settings_t, zone_index), 2 },
		{ offsetof(settings_t, channel), 4 },
		{ offsetof(settings_t, zone_pos), 6 },
		{ offsetof(settings_t, vfo_rx_hz), 8 },
		{ offsetof(settings_t, vfo_tx_hz), 12 },
		{ offsetof(settings_t, vfo_step_hz), 16 },
		{ offsetof(settings_t, vfo_mode), 20 },
		{ offsetof(settings_t, net_flags), 27 },
		{ offsetof(settings_t, vfo_rx_tone), 28 },
		{ offsetof(settings_t, brightness), 32 },
		{ offsetof(settings_t, usb_mode), 37 },
		{ offsetof(settings_t, reserved1), 38 },
		{ offsetof(settings_t, dmr_id), 40 },
		{ offsetof(settings_t, radio_name), 44 },
		{ offsetof(settings_t, privacy_key), 60 },
		{ offsetof(settings_t, privacy_type), 76 },
		{ offsetof(settings_t, privacy_active), 80 },
		{ offsetof(settings_t, snmp_rf_manager), 81 },
		{ offsetof(settings_t, rs_tot_s), 84 },
		{ offsetof(settings_t, rs_stored), 101 },
		{ offsetof(settings_t, rs_backlight_off_s), 108 },
		{ offsetof(settings_t, aprsis_range_km), 110 },
		{ offsetof(settings_t, ds_ars_server), 112 },
		{ offsetof(settings_t, snmp_flags), 119 },
		{ offsetof(settings_t, rs_reply_s), 120 },
		{ offsetof(settings_t, pos_send_s), 130 },
		{ offsetof(settings_t, em_dst), 132 },
		{ offsetof(settings_t, em_stored), 140 },
		{ offsetof(settings_t, cw_wpm), 141 },
		{ offsetof(settings_t, cw_tone_hz), 142 },
		{ offsetof(settings_t, roam_rssi_dbm), 144 },
		{ offsetof(settings_t, roam_check_s), 148 },
		{ offsetof(settings_t, roam_zones), 150 },
		{ offsetof(settings_t, theme), 160 },
		{ offsetof(settings_t, auto_off_min), 164 },
		{ offsetof(settings_t, aprsis_port), 166 },
		{ offsetof(settings_t, roam_beacon_s), 168 },
		{ offsetof(settings_t, rpt_beacon), 172 },
		{ offsetof(settings_t, reserved6), 173 },
		{ offsetof(settings_t, aprs_flags), 176 },
		{ offsetof(settings_t, aprs_freq_hz), 180 },
		{ offsetof(settings_t, aprs_lat), 184 },
		{ offsetof(settings_t, aprs_alt_m), 196 },
		{ offsetof(settings_t, aprs_symbol), 202 },
		{ offsetof(settings_t, aprs_stored), 209 },
		{ offsetof(settings_t, snmp_rf_gap_s), 210 },
		{ offsetof(settings_t, ds_ip_rule), 212 },
		{ offsetof(settings_t, aprsis_ip), 220 },
		{ offsetof(settings_t, net_link_ip), 224 },
		{ offsetof(settings_t, snmp_read_hash), 228 },
		{ offsetof(settings_t, snmp_write_hash), 232 },
	};
	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
		if (offs[i].got != offs[i].want) {
			printf("FAIL settings_t field %u at %zu, layout 1 has it at %zu\n", i, offs[i].got, offs[i].want);
			failures++;
		}
	}
	// A full 236-byte record as another build of the same layout writes it, byte by byte
	uint8_t pl[236];
	memset(pl, 0, sizeof(pl));
	pl[1] = 4;					// squelch
	pl[8] = 0xA0; pl[9] = 0x69; pl[10] = 0xBB; pl[11] = 0x08;	// vfo_rx_hz 146500000
	memcpy(&pl[44], "W1AW", 4);			// radio_name
	pl[232] = 0x78; pl[233] = 0x56; pl[234] = 0x34; pl[235] = 0x12;	// snmp_write_hash, the last field
	sim_flash_create(FLASH_SIZE, &map);
	put_record(&map, 1, 0, 9, SETTINGS_LAYOUT, sizeof(pl), pl, true, true);
	boot();
	settings_t s;
	settings_stats_t st;
	settings_read(&s);
	settings_get_stats(&st);
	CHECK(st.load == SETTINGS_LOADED && st.seq == 9 && st.sector == 1 && st.slot == 0);
	CHECK(s.squelch == 4 && s.vfo_rx_hz == 146500000u && strcmp(s.radio_name, "W1AW") == 0 &&
	      s.snmp_write_hash == 0x12345678u);
}

// Records that must not load: a bad CRC, no commit word, a length past the slot, a newer layout; the older valid
// record is in force, and the sequence numbers compare modulo 2^32
static void test_settings_bad_records(void)
{
	settings_t pl, s;
	settings_stats_t st;

	settings_defaults(&pl);
	for (int variant = 0; variant < 4; variant++) {
		sim_flash_create(FLASH_SIZE, &map);
		pl.squelch = 2;
		put_record(&map, 2, 0, 40, SETTINGS_LAYOUT, sizeof(pl), &pl, true, true);
		pl.squelch = 8;
		if (variant == 0) {
			put_record(&map, 2, 1, 41, SETTINGS_LAYOUT, sizeof(pl), &pl, true, false);	// CRC
		} else if (variant == 1) {
			put_record(&map, 2, 1, 41, SETTINGS_LAYOUT, sizeof(pl), &pl, false, true);	// no commit
		} else if (variant == 2) {
			put_record(&map, 2, 1, 41, SETTINGS_LAYOUT, 237, &pl, true, true);		// too long
			// (no room for its CRC in the slot: whatever is there can't make it valid)
			memset(&sim_flash_data[map.settings_addr + 2 * STORAGE_SECTOR + 256 + 12], 0, 236);
		} else {
			put_record(&map, 2, 1, 41, SETTINGS_LAYOUT + 1, sizeof(pl), &pl, true, true);	// newer layout
		}
		boot();
		settings_read(&s);
		settings_get_stats(&st);
		if (variant < 3) {
			CHECK(st.load == SETTINGS_LOADED && st.seq == 40 && s.squelch == 2 && st.skipped == 1);
		} else {
			// The newest record is a layout this firmware doesn't know: the defaults, and the next save
			// follows it (a newer firmware's records stay until the ring comes round)
			CHECK(st.load == SETTINGS_DEFAULTS && st.seq == 41 && s.squelch == SETTINGS_UNSET8);
			settings_begin()->squelch = 3;
			settings_end(1);
			CHECK(settings_flush());
			settings_get_stats(&st);
			CHECK(st.seq == 42 && st.sector == 2 && st.slot == 2);
		}
		CHECK(sim_flash_violations() == 0);
	}

	// Sequence wraparound: 0xFFFFFFFF then 0 then 1; the newest is 1, the next save 2
	sim_flash_create(FLASH_SIZE, &map);
	pl.squelch = 5;
	put_record(&map, 4, 15, 0xFFFFFFFFu, SETTINGS_LAYOUT, sizeof(pl), &pl, true, true);
	pl.squelch = 6;
	put_record(&map, 5, 0, 0, SETTINGS_LAYOUT, sizeof(pl), &pl, true, true);
	pl.squelch = 7;
	put_record(&map, 5, 1, 1, SETTINGS_LAYOUT, sizeof(pl), &pl, true, true);
	boot();
	settings_read(&s);
	settings_get_stats(&st);
	CHECK(st.seq == 1 && s.squelch == 7 && st.sector == 5 && st.slot == 1);
	settings_begin()->squelch = 1;
	settings_end(1);
	CHECK(settings_flush());
	boot();
	settings_read(&s);
	settings_get_stats(&st);
	CHECK(st.seq == 2 && s.squelch == 1);

	// A ring with no free slot and nothing valid (every slot torn): the save erases a sector and works
	sim_flash_create(FLASH_SIZE, &map);
	for (unsigned sec = 0; sec < map.settings_size / STORAGE_SECTOR; sec++) {
		for (unsigned sl = 0; sl < 16; sl++) {
			put_record(&map, sec, sl, sec * 16u + sl, SETTINGS_LAYOUT, sizeof(pl), &pl, false, true);
		}
	}
	boot();
	settings_get_stats(&st);
	CHECK(st.load == SETTINGS_DEFAULTS && st.skipped == 8 * 16);
	settings_begin()->squelch = 9;
	settings_end(1);
	CHECK(settings_flush());
	boot();
	settings_read(&s);
	CHECK(s.squelch == 9);
	CHECK(sim_flash_violations() == 0);
}

// A worn sector that no longer programs (its slots fail to read back): saves skip its slots, but never by erasing
// the sector that holds the newest valid record, which would leave nothing valid in the ring until the next write
// worked (a power cut there: the settings gone)
static const hal_flash_map_t map_two = {
	.settings_addr = 0x11000u, .settings_size = 0x2000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static void test_settings_worn_sector(void)
{
	settings_t s;
	settings_stats_t st;

	sim_flash_create(FLASH_SIZE, &map_two);
	boot();
	// Sector 0 full: the ownership mark and 15 records
	for (uint32_t i = 1; i <= 15; i++) {
		settings_begin()->vfo_rx_hz = 440000000u + i;
		settings_end(i);
		CHECK(settings_flush());
	}
	settings_get_stats(&st);
	CHECK(st.sector == 0 && st.slot == 15);
	sim_flash_bad_range(map_two.settings_addr + STORAGE_SECTOR, STORAGE_SECTOR);
	// Saves fail while sector 1 doesn't program; each one walks past 4 of its slots (the first erases sector 1).
	// From the second on, a power cut is armed at the first flash operation of the save: the one that would
	// erase sector 0 is cut. After that boot the last saved value must be there
	bool cut_seen = false;
	for (uint32_t i = 0; i < 8 && !cut_seen; i++) {
		settings_begin()->vfo_rx_hz = 145000000u + i;
		settings_end(100 + i);
		sim_flash_cut_after(i == 0 ? 0 : 1);
		CHECK(!settings_flush());
		cut_seen = sim_flash_is_cut();
		sim_flash_cut_after(0);
	}
	boot();
	settings_read(&s);
	if (s.vfo_rx_hz != 440000015u) {
		printf("FAIL worn sector: failed saves and a cut lose the settings (vfo_rx_hz %u, cut %d)\n", s.vfo_rx_hz,
		       cut_seen);
		failures++;
	}
	settings_get_stats(&st);
	CHECK(st.load == SETTINGS_LOADED);
	// The sector works again: saves go on
	sim_flash_bad_range(0, 0);
	settings_begin()->vfo_rx_hz = 150000000u;
	settings_end(1000);
	CHECK(settings_flush());
	boot();
	settings_read(&s);
	CHECK(s.vfo_rx_hz == 150000000u);
	CHECK(sim_flash_violations() == 0);
}


// A record that crosses a sector end (an open-firmware channel, zone or contact record can): a power cut at every unit
// of storage_write, and again inside the replay at the next boot, leaves the whole record old or new, never half of
// each; nothing else in the flash changes (the calibration copy included)
static void test_write_across_sectors(void)
{
	uint8_t old[200], neu[200], back[200];
	const uint32_t at = 0x5FA0u;		// 0x60 bytes before the sector end, 0x68 after
	uint8_t cal[0x200];

	for (unsigned i = 0; i < sizeof(old); i++) {
		old[i] = (uint8_t)(0xA0 ^ i);
		neu[i] = (uint8_t)(0x31 + 3u * i);
	}
	for (unsigned i = 0; i < sizeof(cal); i++) {
		cal[i] = (uint8_t)(i * 5u + 2u);
	}
	sim_flash_create(FLASH_SIZE, &map);
	memcpy(&sim_flash_data[map.calibration_addr], cal, sizeof(cal));
	boot();
	CHECK(storage_write(at, old, sizeof(old)));
	uint8_t *snap = malloc(FLASH_SIZE);
	memcpy(snap, sim_flash_data, FLASH_SIZE);
	uint32_t u0 = sim_flash_units();
	CHECK(storage_write(at, neu, sizeof(neu)));
	uint32_t total = sim_flash_units() - u0;
	CHECK(memcmp(&sim_flash_data[at], neu, sizeof(neu)) == 0);
	CHECK(storage_write(at, neu, sizeof(neu)));		// the same again: skipped, nothing programmed
	CHECK(sim_flash_units() - u0 == total);

	int o = 0, n = 0, bad = 0;
	for (uint32_t cut = 1; cut <= total + 1; cut += (cut < 300 || total - cut < 300) ? 1 : 37) {
		memcpy(sim_flash_data, snap, FLASH_SIZE);
		sim_flash_power_on();
		CHECK(storage_init());
		sim_flash_cut_after(cut);
		storage_write(at, neu, sizeof(neu));
		bool was_cut = sim_flash_is_cut();
		// The boot's replay cut too, at a point that moves with the first cut
		sim_flash_power_on();
		sim_flash_cut_after(was_cut ? 1u + cut % 4111u : 0);
		storage_init();
		sim_flash_power_on();
		CHECK(storage_init());
		memcpy(back, &sim_flash_data[at], sizeof(back));
		if (memcmp(back, neu, sizeof(back)) == 0) {
			n++;
		} else if (memcmp(back, old, sizeof(back)) == 0) {
			o++;
		} else {
			bad++;
			if (bad < 4) {
				printf("FAIL write across sectors: cut after %u of %u units leaves a mixed record\n", cut, total);
			}
		}
		// Outside the two sectors, the journal and the settings ring (the ownership mark), nothing changed
		CHECK(memcmp(&sim_flash_data[map.calibration_addr], cal, sizeof(cal)) == 0);
		CHECK(memcmp(sim_flash_data + 0x7000u, snap + 0x7000u, map.settings_addr - 0x7000u) == 0);
		CHECK(memcmp(sim_flash_data, snap, 0x5000u) == 0);
		// The journal is idle again
		CHECK(storage_write(at, old, sizeof(old)) && memcmp(&sim_flash_data[at], old, sizeof(old)) == 0);
	}
	failures += bad;
	CHECK(o > 0 && n > 0);
	printf("write across sectors: %d cut points, old %d new %d (write = %u units)\n", o + n + bad, o, n, total);

	// Longer than the journal takes in one transaction: sector by sector (the CPS-sized path)
	uint8_t *big = malloc(3 * STORAGE_SECTOR);
	memset(big, 0x3C, 3 * STORAGE_SECTOR);
	CHECK(storage_write(0x20800u, big, 3 * STORAGE_SECTOR));
	CHECK(sim_flash_data[0x207FF] == 0xFF && sim_flash_data[0x20800] == 0x3C && sim_flash_data[0x237FF] == 0x3C &&
	      sim_flash_data[0x23800] == 0xFF);
	// Edges: nothing to write, past the end, wrapping round
	CHECK(storage_write(0x3000u, big, 0));
	CHECK(!storage_write(FLASH_SIZE - 4u, big, 8) && !storage_write(0xFFFFFFF0u, big, 0x20));
	free(big);

	// A patch transaction pending in J0 at boot is ours (the foreign guard), even with the settings ring blank
	storage_stats_t ss;
	storage_get_stats(&ss);
	uint32_t replays = ss.replays;
	memcpy(sim_flash_data, snap, FLASH_SIZE);
	sim_flash_power_on();
	storage_init();
	sim_flash_cut_after(STORAGE_SECTOR + 300u);	// J0 programmed, the first sector staged, not yet rewritten
	storage_write(at, neu, sizeof(neu));
	sim_flash_power_on();
	memset(&sim_flash_data[map.settings_addr], 0xFF, map.settings_size);
	CHECK(storage_init() && storage_region() == STORAGE_REGION_OURS);
	CHECK(memcmp(&sim_flash_data[at], neu, sizeof(neu)) == 0);
	storage_get_stats(&ss);
	CHECK(ss.replays == replays + 1);
	free(snap);
	CHECK(sim_flash_violations() == 0);
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
	test_native_and_jobs();
	test_settings_layout();
	test_settings_bad_records();
	test_settings_worn_sector();
	test_write_across_sectors();
	sim_flash_free();
	printf("# %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
