// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 storage and calibration (docs/spec/hardware-gd77.md 6.2-6.4): the map per NOR size, the guard that keeps every
// write out of the first megabyte, the logical space split across the EEPROM and the NOR, and core/storage and
// core/settings running on a simulated NOR through the guard, with a power cut at every unit of a codeplug sector
// rewrite and of a settings save. Then the generic calibration loader (drivers/cal/cal_block) and the GD-77 layout
// on synthetic blocks: field offsets, the sanity check at each limit, the band rule, unreadable, glitchy and blank
// blocks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "settings/settings.h"
#include "sim/sim_flash.h"
#include "storage_gd77.h"
#include "cal_blocks_gd77.h"
#include "cal_gd77.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

// ---- The board's flash as flash_gd77.c builds it: the simulated part behind the guard ----

static hal_flash_t gd77;
static uint32_t refusals;
const board_t board = { .name = "test", .flash = &gd77 };

static bool g_write(uint32_t addr, const void *buf, uint32_t len)
{
	if (!gd77_storage_writable(addr, len, sim_flash.size)) {
		refusals++;
		return false;
	}
	return sim_flash.write(addr, buf, len);
}

static bool g_erase(uint32_t addr)
{
	if (!gd77_storage_writable(addr & ~(STORAGE_SECTOR - 1u), STORAGE_SECTOR, sim_flash.size)) {
		refusals++;
		return false;
	}
	return sim_flash.erase(addr);
}

// A part of size bytes, its first megabyte holding a pattern that stands for the radio's own data
static void part_create(uint32_t size)
{
	sim_flash_create(size, NULL);
	for (uint32_t i = 0; i < GD77_NOR_FACTORY_END && i < size; i++) {
		sim_flash_data[i] = (uint8_t)(i * 13u + (i >> 12));
	}
	gd77 = sim_flash;
	gd77.write = g_write;
	gd77.erase = g_erase;
	gd77.map = gd77_storage_map(size);
	gd77.size = gd77.map != NULL ? size : 0;
	refusals = 0;
}

static bool factory_intact(void)
{
	for (uint32_t i = 0; i < GD77_NOR_FACTORY_END; i++) {
		if (sim_flash_data[i] != (uint8_t)(i * 13u + (i >> 12))) {
			return false;
		}
	}
	return true;
}

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return alen != 0 && blen != 0 && a < b + blen && b < a + alen;
}

// ---- The map ----

static void test_map(void)
{
	CHECK(gd77_storage_map(0) == NULL);
	CHECK(gd77_storage_map(1u << 20) == NULL);		// 8 Mbit: no room outside the radio's own megabyte
	CHECK(gd77_storage_map((2u << 20) - 1u) == NULL);
	const hal_flash_map_t *m2 = gd77_storage_map(2u << 20);
	const hal_flash_map_t *m4 = gd77_storage_map(4u << 20);
	CHECK(m2 != NULL && m4 != NULL && gd77_storage_map(16u << 20) == m4 && gd77_storage_map(8u << 20) == m4);
	CHECK(gd77_storage_map(GD77_ODR_STAGING + GD77_ODR_STAGING_SIZE - 1u) == m2);
	CHECK(m2->native_size == 0 && m2->staging_size == 0 && m4->native_size != 0 && m4->staging_size != 0);

	const hal_flash_map_t *maps[] = { m2, m4 };
	const uint32_t sizes[] = { 2u << 20, 4u << 20 };
	for (int k = 0; k < 2; k++) {
		const hal_flash_map_t *m = maps[k];
		uint32_t size = sizes[k];
		// Every area in whole sectors, in the part, above the radio's megabyte
		const uint32_t areas[][2] = {
			{ m->openfw_eeprom, 0x10000u }, { m->settings_addr, m->settings_size },
			{ m->journal_addr, 2u * STORAGE_SECTOR }, { m->openfw_flash, m->openfw_end - m->openfw_flash },
			{ m->native_addr, m->native_size }, { m->staging_addr, m->staging_size },
		};
		for (int i = 0; i < 6; i++) {
			if (areas[i][1] == 0) {
				continue;
			}
			CHECK(areas[i][0] % STORAGE_SECTOR == 0 && areas[i][1] % STORAGE_SECTOR == 0);
			CHECK(areas[i][0] >= GD77_NOR_FACTORY_END && areas[i][0] + areas[i][1] <= size);
			CHECK(gd77_storage_writable(areas[i][0], areas[i][1], size));
			for (int j = i + 1; j < 6; j++) {
				CHECK(!overlaps(areas[i][0], areas[i][1], areas[j][0], areas[j][1]));
			}
		}
		// The layout tools rely on: the flash image 128 KB after the EEPROM image, the settings ring 2+ sectors
		CHECK(m->openfw_flash - m->openfw_eeprom == 0x20000u && m->settings_size >= 2u * STORAGE_SECTOR);
		CHECK(m->calibration_size == 0);
	}
	// The open-firmware flash image holds the codeplug (image offsets 0x7B000..0x8EE60)
	CHECK(m2->openfw_flash + 0x8EE60u <= m2->openfw_end);

	// The guard
	uint32_t s = 2u << 20;
	CHECK(!gd77_storage_writable(0, STORAGE_SECTOR, s));
	CHECK(!gd77_storage_writable(0x0FF000u, STORAGE_SECTOR, s));
	CHECK(!gd77_storage_writable(0x0FFFFFu, 2, s));		// straddles into the radio's megabyte
	CHECK(gd77_storage_writable(0x100000u, 1, s));
	CHECK(gd77_storage_writable(0x1FF000u, STORAGE_SECTOR, s));
	CHECK(!gd77_storage_writable(0x1FF001u, STORAGE_SECTOR, s));	// past the end
	CHECK(!gd77_storage_writable(0x200000u, 0, s));
	CHECK(!gd77_storage_writable(0x100000u, 0xFFFFFFFFu, s));
	CHECK(!gd77_storage_writable(0x100000u, 1, 1u << 20));
}

static void test_logical(void)
{
	gd77_logical_piece_t p;
	CHECK(gd77_logical_piece(0, 0x10, &p) && p.eeprom && p.addr == 0 && p.len == 0x10);
	CHECK(gd77_logical_piece(0x7C00, 0x70, &p) && p.eeprom && p.addr == 0x7C00);
	CHECK(gd77_logical_piece(0xB1AF, 1, &p) && p.eeprom && p.addr == 0xB1AF && p.len == 1);
	// Straddling: the EEPROM part first, then the NOR part at logical + 0x70000
	CHECK(gd77_logical_piece(0xB1A0, 0x20, &p) && p.eeprom && p.addr == 0xB1A0 && p.len == 0x10);
	CHECK(gd77_logical_piece(0xB1B0, 0x10, &p) && !p.eeprom && p.addr == 0x7B1B0u && p.len == 0x10);
	CHECK(gd77_logical_piece(0x1FFFF, 1, &p) && !p.eeprom && p.addr == 0x8FFFFu);
	CHECK(!gd77_logical_piece(0x1FFFF, 2, &p) && !gd77_logical_piece(0x20000, 1, &p) &&
	      !gd77_logical_piece(0, 0, &p) && !gd77_logical_piece(0x10, 0xFFFFFFFFu, &p));
	// The radio's own logical space is all in the region ODR never writes
	CHECK(GD77_LOGICAL_SIZE - 1u + GD77_LOGICAL_NOR_OFFSET < GD77_NOR_FACTORY_END);
	CHECK(GD77_REWRITE_SCRATCH + STORAGE_SECTOR <= GD77_NOR_FACTORY_END);
}

// ---- The core on the guarded part ----

static void boot(void)
{
	sim_flash_power_on();
	CHECK(storage_init());
	settings_init();
}

static void test_store(uint32_t size)
{
	part_create(size);
	const hal_flash_map_t *m = gd77.map;
	CHECK(storage_init() && storage_region() == STORAGE_REGION_OURS);
	CHECK(settings_init() == SETTINGS_DEFAULTS);
	// A setting survives a power cycle
	settings_t *st = settings_begin();
	st->brightness = 42;
	st->squelch = 3;
	settings_end(1000);
	CHECK(settings_flush());
	boot();
	settings_t now;
	settings_read(&now);
	CHECK(now.brightness == 42 && now.squelch == 3);
	// Codeplug writes into both open-firmware images, one across a sector end
	uint8_t rec[56];
	memset(rec, 0x5A, sizeof(rec));
	CHECK(storage_write(m->openfw_eeprom + 0x3790u, rec, sizeof(rec)));
	CHECK(storage_write(m->openfw_flash + 0x7B000u - 20u, rec, sizeof(rec)));
	CHECK(memcmp(&sim_flash_data[m->openfw_eeprom + 0x3790u], rec, sizeof(rec)) == 0);
	CHECK(memcmp(&sim_flash_data[m->openfw_flash + 0x7B000u - 20u], rec, sizeof(rec)) == 0);
	// Anything aimed at the radio's megabyte is refused by the board, and nothing there changes
	uint32_t r0 = refusals;
	CHECK(!storage_write(0x7B1B0u, rec, sizeof(rec)));
	uint8_t sector[STORAGE_SECTOR];
	memset(sector, 0, sizeof(sector));
	CHECK(!storage_sector_write(0x00D000u, sector));
	CHECK(refusals > r0);
	CHECK(factory_intact() && sim_flash_violations() == 0);
	if (m->staging_size != 0) {
		CHECK(storage_staging_size() == m->staging_size);
		CHECK(storage_native_erase() == STORAGE_NATIVE_BLANK);
	} else {
		CHECK(storage_staging_size() == 0);
		CHECK(storage_native_erase() == STORAGE_NATIVE_FAILED);
	}
	CHECK(factory_intact());
	sim_flash_free();
}

// A part too small: the core sees no flash, the settings run from RAM, nothing is touched
static void test_small_part(void)
{
	part_create(1u << 20);
	CHECK(gd77.size == 0 && gd77.map == NULL);
	CHECK(!storage_init() && settings_init() == SETTINGS_RAM);
	uint8_t b[4] = { 0 };
	CHECK(!storage_write(0x100u, b, sizeof(b)));
	CHECK(factory_intact() && sim_flash_units() == 0);
	sim_flash_free();
}

// Someone else's bytes where the settings ring goes (a first boot over unknown data): the core treats the region as
// foreign, runs on defaults in RAM and writes nothing
static void test_foreign(void)
{
	part_create(2u << 20);
	for (uint32_t i = 0; i < 64; i++) {
		sim_flash_data[GD77_ODR_SETTINGS + i] = (uint8_t)(0x30 + i);
	}
	uint32_t u0 = sim_flash_units();
	CHECK(!storage_init() && storage_region() == STORAGE_REGION_FOREIGN);
	CHECK(settings_init() == SETTINGS_FOREIGN);
	settings_t *st = settings_begin();
	st->brightness = 7;
	settings_end(10);
	settings_flush();
	CHECK(sim_flash_units() == u0 && factory_intact());
	sim_flash_free();
}

// A power cut at every unit of a codeplug sector rewrite: after the next boot the sector is the old or the new one,
// never a mix, and the radio's megabyte is untouched
static void test_rewrite_cuts(void)
{
	static uint8_t old[STORAGE_SECTOR], neu[STORAGE_SECTOR];
	const uint32_t size = 2u << 20;
	part_create(size);
	uint32_t target = gd77.map->openfw_flash + 0x7B000u;
	for (unsigned i = 0; i < STORAGE_SECTOR; i++) {
		old[i] = (uint8_t)(i * 3u + 1u);
		neu[i] = (uint8_t)(i * 5u + 2u);
	}
	CHECK(storage_init() && storage_sector_write(target, old));
	uint8_t *snap = malloc(size);
	memcpy(snap, sim_flash_data, size);
	uint32_t u0 = sim_flash_units();
	CHECK(storage_sector_write(target, neu));
	uint32_t total = sim_flash_units() - u0;
	int old_seen = 0, new_seen = 0;
	for (uint32_t cut = 1; cut <= total; cut += (cut < 40 || total - cut < 40 || cut % 89 == 0) ? 1 : 11) {
		memcpy(sim_flash_data, snap, size);
		sim_flash_power_on();
		CHECK(storage_init());
		sim_flash_cut_after(cut);
		(void)storage_sector_write(target, neu);
		sim_flash_cut_after(0);
		boot();
		if (memcmp(&sim_flash_data[target], neu, STORAGE_SECTOR) == 0) {
			new_seen++;
		} else if (memcmp(&sim_flash_data[target], old, STORAGE_SECTOR) == 0) {
			old_seen++;
		} else {
			printf("FAIL gd77 rewrite: cut after %u of %u units leaves a mixed sector\n", cut, total);
			failures++;
		}
		CHECK(factory_intact());
	}
	CHECK(old_seen > 0 && new_seen > 0);
	printf("gd77 rewrite: old %d new %d (rewrite = %u units)\n", old_seen, new_seen, total);
	free(snap);
	sim_flash_free();
}

// A power cut at every unit of a settings save: the old or the new value loads
static void test_save_cuts(void)
{
	const uint32_t size = 2u << 20;
	part_create(size);
	CHECK(storage_init());
	settings_init();
	settings_t *st = settings_begin();
	st->brightness = 10;
	settings_end(1);
	CHECK(settings_flush());
	uint8_t *snap = malloc(size);
	memcpy(snap, sim_flash_data, size);
	uint32_t u0 = sim_flash_units();
	st = settings_begin();
	st->brightness = 90;
	settings_end(2);
	CHECK(settings_flush());
	uint32_t total = sim_flash_units() - u0;
	for (uint32_t cut = 1; cut <= total; cut++) {
		memcpy(sim_flash_data, snap, size);
		boot();
		st = settings_begin();
		st->brightness = 90;
		settings_end(3);
		sim_flash_cut_after(cut);
		(void)settings_flush();
		sim_flash_cut_after(0);
		boot();
		settings_t now;
		settings_read(&now);
		CHECK(now.brightness == 10 || now.brightness == 90);
		CHECK(factory_intact());
	}
	free(snap);
	sim_flash_free();
}

// ---- Calibration ----

static uint8_t eeprom[0x10000];
static int read_calls, glitch_every, fail_reads;

static bool ee_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
	(void)ctx;
	read_calls++;
	if (fail_reads > 0) {
		fail_reads--;
		return false;
	}
	if (addr + len > sizeof(eeprom)) {
		return false;
	}
	memcpy(buf, &eeprom[addr], len);
	if (glitch_every != 0 && read_calls % glitch_every == 0) {
		((uint8_t *)buf)[len / 2] ^= 0x01;	// a flipped bit on the shared bus
	}
	return true;
}

// A block that passes: type 1 range 136.0-174.0 (VHF) or 400.0-480.0 (UHF), field bytes = offset pattern, field 0x50
// within its limits
static void make_block(uint8_t *b, uint16_t lo, uint16_t hi, uint8_t seed)
{
	for (unsigned i = 0; i < GD77_CAL_BLOCK; i++) {
		b[i] = (uint8_t)(seed + i);
	}
	b[0] = 0x50; b[1] = 0x05; b[2] = 0x40; b[3] = 0x06;	// type 0 range, not checked
	b[4] = (uint8_t)lo; b[5] = (uint8_t)(lo >> 8);
	b[6] = (uint8_t)hi; b[7] = (uint8_t)(hi >> 8);
	b[0x64] = 0x1F; b[0x65] = 0x3F; b[0x66] = 0x0F;
}

static void test_cal_layout(void)
{
	// Field offsets from the block start (the 8-byte header included), as hardware-gd77.md 6.4 lists them
	static const struct { uint8_t id, size; int off; } want[] = {
		{ 0x40, 3, 0x08 }, { 0x41, 48, 0x0B }, { 0x42, 4, 0x3B }, { 0x43, 20, 0x3F }, { 0x44, 2, 0x53 },
		{ 0x01, 8, 0x55 }, { 0x30, 1, 0x5D }, { 0x31, 1, 0x5E }, { 0x32, 1, 0x5F }, { 0x33, 2, 0x60 },
		{ 0x34, 2, 0x62 }, { 0x50, 8, 0x64 }, { 0x51, 3, 0x6C },
	};
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		uint8_t size = 0;
		CHECK(cal_field_offset(&gd77_cal_layout, want[i].id, &size) == want[i].off && size == want[i].size);
	}
	// The last field ends at 0x6F, inside the 0x70-byte block; ids without bytes aren't fields
	CHECK(cal_field_offset(&gd77_cal_layout, 0x51, NULL) + 3 == 0x6F);
	CHECK(cal_field_offset(&gd77_cal_layout, 0x02, NULL) < 0 && cal_field_offset(&gd77_cal_layout, 0xFF, NULL) < 0);
	// A table that doesn't fit its block is refused rather than read past the end
	static const cal_field_t big[] = { { 1, 200 } };
	const cal_layout_t bad = { .size = 0x70, .header = 8, .fields = big, .count = 1 };
	CHECK(cal_field_offset(&bad, 1, NULL) < 0);
}

static void test_cal_check(void)
{
	uint8_t b[GD77_CAL_BLOCK];
	make_block(b, 1360, 1740, 0);
	CHECK(gd77_cal_check(b));
	// Each limit of the type 1 range
	static const struct { uint16_t lo, hi; bool ok; } r[] = {
		{ 1359, 1740, false }, { 1360, 1740, true }, { 4000, 5200, true }, { 4000, 5201, false },
		{ 1360, 2600, true }, { 1360, 2601, false }, { 3999, 4800, false }, { 4000, 4800, true },
		{ 1740, 1740, false }, { 1740, 1360, false }, { 0x5005, 0x4006, false },
	};
	for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
		make_block(b, r[i].lo, r[i].hi, 0);
		CHECK(gd77_cal_check(b) == r[i].ok);
	}
	// Field 0x50's first three bytes
	make_block(b, 4000, 4800, 0);
	b[0x64] = 0x20;
	CHECK(!gd77_cal_check(b));
	make_block(b, 4000, 4800, 0);
	b[0x65] = 0x40;
	CHECK(!gd77_cal_check(b));
	make_block(b, 4000, 4800, 0);
	b[0x66] = 0x10;
	CHECK(!gd77_cal_check(b));
	// The range is little endian
	make_block(b, 4000, 4800, 0);
	uint8_t t = b[4]; b[4] = b[5]; b[5] = t;
	CHECK(!gd77_cal_check(b));
}

static void test_cal_load(void)
{
	uint8_t out[64];
	memset(eeprom, 0xFF, sizeof(eeprom));
	make_block(&eeprom[GD77_CAL_UHF_ADDR], 4000, 4800, 0x10);
	make_block(&eeprom[GD77_CAL_VHF_ADDR], 1360, 1740, 0x80);
	read_calls = glitch_every = fail_reads = 0;
	gd77_cal_load(ee_read, NULL);
	CHECK(gd77_cal_state(GD77_CAL_UHF) == CAL_BLOCK_OK && gd77_cal_state(GD77_CAL_VHF) == CAL_BLOCK_OK);
	// The band rule: 180 MHz and up is the UHF block
	CHECK(gd77_cal_band(179999999u) == GD77_CAL_VHF && gd77_cal_band(180000000u) == GD77_CAL_UHF);
	CHECK(gd77_cal_band(145500000u) == GD77_CAL_VHF && gd77_cal_band(446000000u) == GD77_CAL_UHF);
	CHECK(gd77_cal_ok(145500000u) && gd77_cal_ok(446000000u));
	// Fields come from the block of the frequency, raw
	CHECK(gd77_cal_field(446000000u, GD77_CAL_ID_41, out, sizeof(out)) == 48);
	CHECK(out[0] == (uint8_t)(0x10 + 0x0B) && out[47] == (uint8_t)(0x10 + 0x0B + 47));
	CHECK(gd77_cal_field(145500000u, GD77_CAL_ID_44, out, sizeof(out)) == 2 && out[0] == (uint8_t)(0x80 + 0x53));
	CHECK(gd77_cal_field(145500000u, GD77_CAL_ID_41, out, 4) == 4);	// truncated to the caller's room
	CHECK(gd77_cal_field(145500000u, 0x02, out, sizeof(out)) == 0);	// no bytes
	CHECK(gd77_cal_field(145500000u, 0x99, out, sizeof(out)) == 0);	// no such id
	uint16_t lo, hi;
	CHECK(gd77_cal_range(GD77_CAL_VHF, 1, &lo, &hi) && lo == 1360 && hi == 1740);
	CHECK(gd77_cal_range(GD77_CAL_UHF, 0, &lo, &hi) && lo == 0x0550 && hi == 0x0640);
	CHECK(!gd77_cal_range(GD77_CAL_UHF, 2, &lo, &hi) && !gd77_cal_range(GD77_CAL_BANDS, 0, &lo, &hi));
	CHECK(gd77_cal_raw(GD77_CAL_UHF) != NULL && memcmp(gd77_cal_raw(GD77_CAL_UHF), &eeprom[0x7C00], 0x70) == 0);
	CHECK(gd77_cal_raw(GD77_CAL_BANDS) == NULL && gd77_cal_state(GD77_CAL_BANDS) == CAL_BLOCK_NONE);

	// A VHF block that fails the check: rejected, the UHF one still used; the caller gets no bytes for VHF
	eeprom[GD77_CAL_VHF_ADDR + 0x64] = 0x77;
	gd77_cal_load(ee_read, NULL);
	CHECK(gd77_cal_state(GD77_CAL_VHF) == CAL_BLOCK_REJECTED && gd77_cal_state(GD77_CAL_UHF) == CAL_BLOCK_OK);
	CHECK(!gd77_cal_ok(145500000u) && gd77_cal_field(145500000u, GD77_CAL_ID_44, out, sizeof(out)) == 0);
	CHECK(gd77_cal_raw(GD77_CAL_VHF) == NULL);
	// Blank (never written) and all zero
	memset(&eeprom[GD77_CAL_VHF_ADDR], 0xFF, GD77_CAL_BLOCK);
	memset(&eeprom[GD77_CAL_UHF_ADDR], 0x00, GD77_CAL_BLOCK);
	gd77_cal_load(ee_read, NULL);
	CHECK(gd77_cal_state(GD77_CAL_VHF) == CAL_BLOCK_BLANK && gd77_cal_state(GD77_CAL_UHF) == CAL_BLOCK_BLANK);
	// A failed read, and two reads that disagree
	make_block(&eeprom[GD77_CAL_UHF_ADDR], 4000, 4800, 0x10);
	make_block(&eeprom[GD77_CAL_VHF_ADDR], 1360, 1740, 0x80);
	fail_reads = 1;
	read_calls = 0;
	glitch_every = 3;	// the second read of the second block (the first block stops at its failed read)
	gd77_cal_load(ee_read, NULL);
	CHECK(gd77_cal_state(GD77_CAL_UHF) == CAL_BLOCK_READ_FAILED && gd77_cal_state(GD77_CAL_VHF) == CAL_BLOCK_READ_FAILED);
	glitch_every = 0;
	gd77_cal_load(ee_read, NULL);
	CHECK(gd77_cal_state(GD77_CAL_UHF) == CAL_BLOCK_OK && gd77_cal_state(GD77_CAL_VHF) == CAL_BLOCK_OK);
	// The radio code's point from the loaded blocks (the conversion itself is gd77CalTest): the UHF block's
	// fill pattern puts its two-point bias (0x1918) above the limit, so no TX point, but its receive side is used
	gd77_cal_point_t pt;
	memset(&pt, 0x55, sizeof(pt));
	CHECK(!gd77_cal_at(446000000u, &pt) && !pt.valid && pt.rx_valid && pt.power_drive[0] == GD77_CAL_SAFE_DRIVE &&
	      pt.power_drive[1] == GD77_CAL_SAFE_DRIVE);
	// The generic band lookup outside every band
	static const cal_band_t one[] = { { 100, 200, 0 } };
	CHECK(cal_band_index(one, 1, 99) < 0 && cal_band_index(one, 1, 200) == 0 && cal_band_index(one, 1, 201) < 0);
}

int main(void)
{
	test_map();
	test_logical();
	test_store(2u << 20);
	test_store(4u << 20);
	test_small_part();
	test_foreign();
	test_rewrite_cuts();
	test_save_cuts();
	test_cal_layout();
	test_cal_check();
	test_cal_load();
	printf("# gd77StorageTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
