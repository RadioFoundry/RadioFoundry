// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The MD-9600's data flash map under core/storage and core/settings, and its board record sector
// (boards/md9600/store_md9600.c), on the SPI NOR chip model of tests/host/spiNorTest.c: the areas don't overlap, the
// record survives a power cut at every operation of a change, and no test sends a command that could touch the
// factory calibration in the security registers.
#define SPI_NOR_TEST_LIB
#pragma GCC diagnostic ignored "-Wunused-function"	// the generic tests spiNorTest.c runs itself
#include "spiNorTest.c"
#include "store_md9600.h"

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return alen != 0 && blen != 0 && a < b + blen && b < a + alen;
}

static void test_md9600_map(void)
{
	const hal_flash_map_t *m = &md9600_flash_map;
	const uint32_t openfw_eeprom_len = 0x10000u;	// the 64 KB "EEPROM" image
	struct { uint32_t addr, len; } area[] = {
		{ m->settings_addr, m->settings_size }, { m->journal_addr, 2 * STORAGE_SECTOR },
		{ m->openfw_eeprom, openfw_eeprom_len }, { m->openfw_flash, m->openfw_end - m->openfw_flash },
		{ m->native_addr, m->native_size }, { m->staging_addr, m->staging_size },
		{ MD9600_STORE_REV_SECTOR, MD9600_STORE_SECTOR_SIZE },
	};
	const unsigned n = sizeof(area) / sizeof(area[0]);
	for (unsigned i = 0; i < n; i++) {
		CHECK(area[i].addr % STORAGE_SECTOR == 0 && area[i].len % STORAGE_SECTOR == 0);
		CHECK(area[i].addr + area[i].len <= MD9600_STORE_FULL_SIZE);
		for (unsigned j = i + 1; j < n; j++) {
			if (overlaps(area[i].addr, area[i].len, area[j].addr, area[j].len)) {
				printf("FAIL md9600 map: areas %u and %u overlap\n", i, j);
				failures++;
			}
		}
	}
	CHECK(m->settings_size >= 2 * STORAGE_SECTOR && m->calibration_size == 0);
}

static void md9600_create(void)
{
	chip_create(16u << 20, 0xEF, 0x40, 0x18);
	flash.map = &md9600_flash_map;
}

static void test_md9600_store(void)
{
	uint8_t sec_before[3][256], a[STORAGE_SECTOR];
	settings_t s;

	md9600_create();
	memcpy(sec_before, chip.security, sizeof sec_before);
	CHECK(boot() && storage_ready());
	CHECK(md9600_store_rev_load(&flash) == MD9600_REV_UNKNOWN);
	// Settings and a codeplug write under the board's map, then the board record beside them
	settings_begin()->squelch = 4;
	settings_end(1);
	CHECK(settings_flush());
	fill(a, 3);
	CHECK(storage_write(0x20FF0u, a, 64));
	CHECK(md9600_store_rev_save(&flash, MD9600_REV_V4) && md9600_store_rev_load(&flash) == MD9600_REV_V4);
	CHECK(md9600_store_rev_save(&flash, MD9600_REV_V5) && md9600_store_rev_load(&flash) == MD9600_REV_V5);
	uint32_t ops = chip.programs + chip.erases;
	CHECK(md9600_store_rev_save(&flash, MD9600_REV_V5) && chip.programs + chip.erases == ops);	// same: no write
	CHECK(!md9600_store_rev_save(&flash, MD9600_REV_UNKNOWN) && !md9600_store_rev_save(&flash, MD9600_REV_COUNT));
	// Appended: V4 in the first slot, V5 in the second, the third free
	CHECK(chip.mem[MD9600_STORE_REV_SECTOR + 5] == MD9600_REV_V4);
	CHECK(chip.mem[MD9600_STORE_REV_SECTOR + 13] == MD9600_REV_V5);
	CHECK(chip.mem[MD9600_STORE_REV_SECTOR + 16] == 0xFF);
	CHECK(boot());
	settings_read(&s);
	CHECK(s.squelch == 4 && memcmp(&chip.mem[0x20FF0u], a, 64) == 0);
	CHECK(md9600_store_rev_load(&flash) == MD9600_REV_V5);
	// The core never touched the record sector, and the record never touched the core's areas
	CHECK(storage_region() == STORAGE_REGION_OURS);
	// Another map or a smaller part: no record
	flash.map = &map16;
	CHECK(md9600_store_rev_load(&flash) == MD9600_REV_UNKNOWN && !md9600_store_rev_save(&flash, MD9600_REV_V4));
	chip_create(1u << 20, 0xEF, 0x40, 0x14);
	flash.map = &md9600_flash_map;
	boot();
	CHECK(md9600_store_rev_load(&flash) == MD9600_REV_UNKNOWN && !md9600_store_rev_save(&flash, MD9600_REV_V4));
	CHECK(memcmp(sec_before, chip.security, sizeof sec_before) == 0);
	flash.map = &map16;
}

// A power cut at every operation of a revision change, with the sector part used and with it full: after the next
// boot the old revision or the new one, never another; nothing outside the record sector changes
static void test_md9600_rev_cuts(void)
{
	uint8_t *snap = malloc(16u << 20);
	for (int full = 0; full < 2; full++) {
		uint32_t n_ops = 0;
		int old_seen = 0, new_seen = 0, lost = 0;
		for (uint32_t cut = 0; cut == 0 || cut <= n_ops + 1; cut++) {
			md9600_create();
			boot();
			CHECK(md9600_store_rev_save(&flash, MD9600_REV_V1V3));
			if (full) {
				// Fill every slot after the first with the same record: the next change must erase
				uint8_t rec[MD9600_REV_RECORD_SIZE];
				md9600_rev_record_encode(MD9600_REV_V1V3, rec);
				for (uint32_t off = MD9600_REV_RECORD_SIZE; off < MD9600_STORE_SECTOR_SIZE; off += sizeof rec) {
					f_write(MD9600_STORE_REV_SECTOR + off, rec, sizeof rec);
				}
			} else {
				CHECK(md9600_store_rev_save(&flash, MD9600_REV_V4));
				CHECK(md9600_store_rev_save(&flash, MD9600_REV_V1V3));
			}
			memcpy(snap, chip.mem, 16u << 20);
			uint32_t ops0 = chip.programs + chip.erases;
			chip.ops_left = cut;
			md9600_store_rev_save(&flash, MD9600_REV_V5);
			if (cut == 0) {
				n_ops = chip.programs + chip.erases - ops0;
			}
			boot();
			md9600_rev_t r = md9600_store_rev_load(&flash);
			if (r == MD9600_REV_V5) {
				new_seen++;
			} else if (r == MD9600_REV_V1V3) {
				old_seen++;
			} else if (r == MD9600_REV_UNKNOWN && full) {
				lost++;		// the erase of a full sector was cut: as a new radio (store_md9600.h)
			} else {
				printf("FAIL md9600 record: cut %u (%s sector) loads %d\n", cut, full ? "full" : "part used", r);
				failures++;
			}
			CHECK(memcmp(snap, chip.mem, MD9600_STORE_REV_SECTOR) == 0);
			CHECK(memcmp(snap + MD9600_STORE_REV_SECTOR + MD9600_STORE_SECTOR_SIZE,
				     chip.mem + MD9600_STORE_REV_SECTOR + MD9600_STORE_SECTOR_SIZE,
				     (16u << 20) - MD9600_STORE_REV_SECTOR - MD9600_STORE_SECTOR_SIZE) == 0);
			// A later save after the cut still works
			CHECK(md9600_store_rev_save(&flash, MD9600_REV_V4) && md9600_store_rev_load(&flash) == MD9600_REV_V4);
		}
		CHECK(new_seen > 0 && (old_seen > 0 || lost > 0));
		CHECK(full || lost == 0);
	}
	free(snap);
	flash.map = &map16;
	CHECK(chip.violations == 0 && chip.page_overruns == 0);
}

int main(void)
{
	test_probe();
	test_md9600_map();
	test_md9600_store();
	test_md9600_rev_cuts();
	test_commands_sent();
	free(chip.mem);
	printf("# md9600StoreTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
