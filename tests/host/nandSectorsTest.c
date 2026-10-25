// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// drivers/flash/nand_sectors on a simulated NAND region, and core/storage + core/settings on top of it.
//
// The simulated NAND: 2 KB pages with 64 spare bytes, erase sets a block to 0xFF, a page takes one program and only
// in page order within its block (anything else is counted as a violation), program only clears bits, an internal
// data move copies a page through the buffer. Factory bad blocks carry the mark and refuse program and erase; worn
// blocks report program or erase failures; a page can read back wrong once. A power cut after N operations tears the
// operation in progress in one of several ways (a page half programmed with ECC failing, a page whose spare area
// made it but not all of its data, a page that looks blank, an erase that left some pages) and fails everything after
// it until the power comes back. After each cut the region is mounted again: every sector must hold its old or its new
// content, whole, and no other sector may change.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flash/nand_sectors/nand_sectors.h"
#include "storage/storage.h"
#include "settings/settings.h"
#include "cps/cps_openfw.h"
#include "codeplug/codeplug.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

// --- The simulated NAND ---

#define PAGE		2048u
#define SPARE		64u
#define RAW		(PAGE + SPARE)
#define MAX_BLOCKS	24u
#define MAX_PPB		64u

typedef enum { TEAR_ECC, TEAR_DATA, TEAR_BLANK, TEAR_MODES } tear_t;

static struct {
	uint16_t ppb, blocks;
	uint8_t data[MAX_BLOCKS * MAX_PPB][RAW];
	bool ecc_fail[MAX_BLOCKS * MAX_PPB];		// reads of the page fail ECC
	uint16_t next[MAX_BLOCKS];			// the next page that may be programmed in each block
	bool factory_bad[MAX_BLOCKS];
	bool worn_program[MAX_BLOCKS], worn_erase[MAX_BLOCKS];
	uint32_t erases[MAX_BLOCKS];
	uint32_t flip_page;				// the next program of this page reads back with a bit flipped
	uint32_t violations;				// programs out of order or twice, ops on factory-bad blocks
	uint32_t ops;					// programs + erases so far
	uint32_t programs;				// the page programs among them
	uint32_t cut_at;				// 0: no cut armed; else the op number that is torn
	tear_t tear;
	bool off;					// power cut: everything fails
	uint32_t reads;
} nand;

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

static void nand_create(uint16_t blocks, uint16_t ppb)
{
	memset(&nand, 0, sizeof(nand));
	nand.blocks = blocks;
	nand.ppb = ppb;
	memset(nand.data, 0xFF, sizeof(nand.data));
	nand.flip_page = NAND_SECTORS_NO_PAGE;
}

static void factory_bad(uint16_t b)
{
	nand.factory_bad[b] = true;
	for (uint32_t p = 0; p < nand.ppb; p++) {
		memset(nand.data[b * nand.ppb + p], 0x00, RAW);
		nand.ecc_fail[b * nand.ppb + p] = true;
	}
}

// One more operation: false when the power is (or just went) off. *torn: this is the operation being cut
static bool op_begin(bool *torn)
{
	*torn = false;
	if (nand.off) {
		return false;
	}
	nand.ops++;
	if (nand.cut_at != 0 && nand.ops == nand.cut_at) {
		nand.off = true;
		*torn = true;
	}
	return true;
}

static bool sim_read(void *ctx, uint32_t page, uint16_t column, void *buf, uint32_t len)
{
	(void)ctx;
	if (nand.off || page >= (uint32_t)nand.blocks * nand.ppb || column + len > RAW || nand.ecc_fail[page]) {
		return false;
	}
	nand.reads++;
	memcpy(buf, &nand.data[page][column], len);
	return true;
}

static nand_result_t sim_program(void *ctx, uint32_t page, uint32_t src, const nand_chunk_t *chunks, unsigned n)
{
	(void)ctx;
	static uint8_t buf[RAW];
	bool torn;
	uint16_t b = (uint16_t)(page / nand.ppb);
	if (page >= (uint32_t)nand.blocks * nand.ppb) {
		return NAND_ERROR;
	}
	if (src != NAND_SECTORS_NO_PAGE) {
		if (nand.off || nand.ecc_fail[src]) {
			return NAND_ERROR;
		}
		memcpy(buf, nand.data[src], RAW);
	} else {
		memset(buf, 0xFF, RAW);
	}
	for (unsigned i = 0; i < n; i++) {
		CHECK(chunks[i].column + chunks[i].len <= RAW);
		memcpy(&buf[chunks[i].column], chunks[i].data, chunks[i].len);
	}
	if (!op_begin(&torn)) {
		return NAND_ERROR;
	}
	nand.programs++;
	if (nand.factory_bad[b]) {
		nand.violations++;
		return NAND_FAILED;
	}
	if (page % nand.ppb != nand.next[b]) {
		nand.violations++;		// twice, or not in page order
	}
	nand.next[b] = (uint16_t)(page % nand.ppb + 1u);
	uint8_t *d = nand.data[page];
	if (torn) {
		switch (nand.tear) {
		case TEAR_ECC:		// half programmed: ECC can't make sense of it
			for (uint32_t i = 0; i < RAW; i++) {
				d[i] &= (uint8_t)(buf[i] | (rnd() & 0xFFu));
			}
			nand.ecc_fail[page] = true;
			break;
		case TEAR_DATA:		// the spare area made it (and reads clean), part of the data didn't
			for (uint32_t i = 0; i < RAW; i++) {
				d[i] &= i < PAGE / 2 || i >= PAGE ? buf[i] : 0xFF;
			}
			break;
		default:		// cut at once: nothing changed
			break;
		}
		return NAND_ERROR;
	}
	if (nand.worn_program[b]) {
		for (uint32_t i = 0; i < RAW; i++) {
			d[i] &= (uint8_t)(buf[i] | 0x0F);	// something programmed, wrongly
		}
		return NAND_FAILED;
	}
	for (uint32_t i = 0; i < RAW; i++) {
		d[i] &= buf[i];
	}
	if (page == nand.flip_page) {
		d[100] ^= 0x01;
		nand.flip_page = NAND_SECTORS_NO_PAGE;
	}
	return NAND_OK;
}

static nand_result_t sim_erase(void *ctx, uint16_t b)
{
	(void)ctx;
	bool torn;
	if (b >= nand.blocks || !op_begin(&torn)) {
		return NAND_ERROR;
	}
	if (nand.factory_bad[b]) {
		nand.violations++;
		return NAND_FAILED;
	}
	nand.erases[b]++;
	for (uint32_t p = 0; p < nand.ppb; p++) {
		uint32_t g = b * nand.ppb + p;
		// A torn erase: every other page erased, the others left as they were, and one page half erased
		if (torn && (p % 2) == 1) {
			continue;
		}
		if (torn && p == 2) {
			for (uint32_t i = 0; i < RAW; i++) {
				nand.data[g][i] |= (uint8_t)rnd();
			}
			nand.ecc_fail[g] = true;
			continue;
		}
		memset(nand.data[g], 0xFF, RAW);
		nand.ecc_fail[g] = false;
	}
	if (torn) {
		nand.next[b] = nand.ppb;	// no page of a torn erase may be programmed again before an erase
		return NAND_ERROR;
	}
	nand.next[b] = 0;
	return nand.worn_erase[b] ? NAND_FAILED : NAND_OK;
}

static bool sim_block_bad(void *ctx, uint16_t b, bool *bad)
{
	(void)ctx;
	if (nand.off || b >= nand.blocks) {
		return false;
	}
	*bad = nand.data[b * nand.ppb][PAGE] != 0xFF;
	return true;
}

static void sim_mark_bad(void *ctx, uint16_t b)
{
	(void)ctx;
	bool torn;
	if (!op_begin(&torn)) {
		return;
	}
	memset(nand.data[b * nand.ppb], 0x00, RAW);
	nand.ecc_fail[b * nand.ppb] = true;
}

// The board feeds its watchdog here (nand_dev_t.busy)
static unsigned busy_calls;

static void sim_busy(void *ctx)
{
	(void)ctx;
	busy_calls++;
}

static nand_dev_t dev = {
	.ctx = NULL, .spare_size = SPARE,
	.read = sim_read, .program = sim_program, .erase = sim_erase, .block_bad = sim_block_bad, .mark_bad = sim_mark_bad,
	.busy = sim_busy,
};

// --- The layer instance and a model of what it should hold ---

#define MAX_SECTORS	64u

static uint16_t map[MAX_SECTORS];
static uint8_t live[MAX_BLOCKS], flags[MAX_BLOCKS];
static uint32_t order[MAX_BLOCKS];
static nand_sectors_t ns;
static uint8_t model[MAX_SECTORS * NAND_SECTORS_SECTOR];

static bool mount(uint16_t blocks, uint16_t ppb, uint16_t sectors)
{
	dev.blocks = blocks;
	dev.pages_per_block = ppb;
	memset(&ns, 0, sizeof(ns));
	ns.dev = &dev;
	ns.sectors = sectors;
	ns.map = map;
	ns.live = live;
	ns.flags = flags;
	ns.order = order;
	return nand_sectors_mount(&ns);
}

static bool remount(void)
{
	nand.off = false;
	nand.cut_at = 0;
	return mount(dev.blocks, dev.pages_per_block, ns.sectors);
}

static void fill(uint8_t *b, uint32_t len, uint32_t seed)
{
	for (uint32_t i = 0; i < len; i++) {
		b[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 9));
	}
}

static bool matches_model(void)
{
	static uint8_t buf[NAND_SECTORS_SECTOR];
	for (uint32_t s = 0; s < ns.sectors; s++) {
		if (!nand_sectors_read(&ns, s * NAND_SECTORS_SECTOR, buf, sizeof(buf)) ||
		    memcmp(buf, &model[s * NAND_SECTORS_SECTOR], sizeof(buf)) != 0) {
			printf("  sector %u differs\n", (unsigned)s);
			return false;
		}
	}
	return true;
}

// A write through the layer and into the model (NOR: bits only cleared)
static bool write_both(uint32_t addr, const uint8_t *data, uint32_t len)
{
	bool ok = nand_sectors_write(&ns, addr, data, len);
	if (ok) {
		for (uint32_t i = 0; i < len; i++) {
			model[addr + i] &= data[i];
		}
	}
	return ok;
}

static bool erase_both(uint32_t addr)
{
	bool ok = nand_sectors_erase(&ns, addr);
	if (ok) {
		memset(&model[addr], 0xFF, NAND_SECTORS_SECTOR);
	}
	return ok;
}

// A whole-sector rewrite as core/storage does it: erase, then program
static bool rewrite_both(uint32_t s, uint32_t seed)
{
	static uint8_t b[NAND_SECTORS_SECTOR];
	fill(b, sizeof(b), seed);
	return erase_both(s * NAND_SECTORS_SECTOR) && write_both(s * NAND_SECTORS_SECTOR, b, sizeof(b));
}

static void fresh(uint16_t blocks, uint16_t ppb, uint16_t sectors)
{
	nand_create(blocks, ppb);
	memset(model, 0xFF, sizeof(model));
	CHECK(mount(blocks, ppb, sectors));
}

// --- Tests ---

static void test_basic(void)
{
	uint8_t b[300], r[300];
	nand_sectors_stats_t st;

	fresh(8, 64, 16);
	CHECK(nand_sectors_size(&ns) == 16 * NAND_SECTORS_SECTOR);
	CHECK(matches_model());			// blank: 0xFF everywhere
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.blocks_free == 8 && st.blocks_bad == 0 && st.sectors_used == 0);

	// Small writes into one erased sector, one after the other (a settings slot, then its commit word)
	fill(b, sizeof(b), 1);
	CHECK(write_both(0x1000 + 10, b, 242));
	static const uint8_t zero[4] = { 0, 0, 0, 0 };
	CHECK(write_both(0x1000 + 252, zero, 4));
	CHECK(write_both(0x1000 + 2040, b, 20));	// across the two pages of the sector
	CHECK(write_both(0x1FF0, b, 32));		// across two sectors
	CHECK(matches_model());
	CHECK(nand_sectors_read(&ns, 0x1000 + 10, r, 242) && memcmp(r, b, 242) == 0);

	// The same bytes again: nothing written
	nand_sectors_get_stats(&ns, &st);
	uint32_t writes = st.writes;
	CHECK(write_both(0x1000 + 10, b, 242));
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.writes == writes);
	// Clearing more bits of programmed bytes is a program; setting one back isn't
	uint8_t x = 0x00;
	CHECK(write_both(0x1000 + 10, &x, 1));
	x = 0xFF;
	CHECK(!nand_sectors_write(&ns, 0x1000 + 10, &x, 1));
	CHECK(matches_model());

	// Erase, and an erase of an erased sector costs nothing
	CHECK(erase_both(0x1000));
	nand_sectors_get_stats(&ns, &st);
	writes = st.writes;
	CHECK(erase_both(0x1000));
	CHECK(erase_both(0x5000));		// never written
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.writes == writes);
	CHECK(!nand_sectors_erase(&ns, 0x1001));
	CHECK(!nand_sectors_erase(&ns, 16 * NAND_SECTORS_SECTOR));
	CHECK(matches_model());

	// Bounds
	CHECK(!nand_sectors_read(&ns, 16 * NAND_SECTORS_SECTOR - 4, r, 5));
	CHECK(!nand_sectors_write(&ns, 16 * NAND_SECTORS_SECTOR - 4, b, 5));
	CHECK(nand_sectors_read(&ns, 16 * NAND_SECTORS_SECTOR - 4, r, 4));

	// It all comes back after a remount; nothing programmed out of order
	CHECK(remount() && matches_model());
	CHECK(nand.violations == 0);
}

// Rewrites far beyond the region's size: garbage collection, wear spread over every block, the map right after
// remounts at random points, a full device
// The long loops call the busy hook (a board feeds its watchdog there, docs/spec/watchdog.md 4): the mount at least once
// per block and once per sector in use, garbage collection on each pass. Without the hook nothing changes
static void test_busy_hook(void)
{
	fresh(8, 16, 12);
	for (uint32_t s = 0; s < 12; s++) {
		CHECK(rewrite_both(s, s + 1));
	}
	busy_calls = 0;
	CHECK(remount() && matches_model());
	CHECK(busy_calls >= 8 + 12);
	// Rewrites until garbage collection has had to run: the hook is called during the writes too
	busy_calls = 0;
	for (uint32_t i = 0; i < 200; i++) {
		CHECK(rewrite_both(i % 12, 1000 + i));
	}
	nand_sectors_stats_t st;
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.gc_blocks > 0 && busy_calls > 0);
	dev.busy = NULL;
	CHECK(remount() && matches_model());
	dev.busy = sim_busy;
}

static void test_gc_and_wear(void)
{
	nand_sectors_stats_t st;
	fresh(10, 16, 24);			// 10 blocks of 8 records; 24 sectors = 48 pages = 3 blocks
	for (uint32_t s = 0; s < 24; s++) {
		CHECK(rewrite_both(s, s));
	}
	for (uint32_t i = 0; i < 3000; i++) {
		uint32_t s = rnd() % 4;		// a few hot sectors, the rest static
		if (rnd() % 3 == 0) {
			uint8_t b[16];
			fill(b, sizeof(b), i);
			uint32_t at = s * NAND_SECTORS_SECTOR + rnd() % (NAND_SECTORS_SECTOR - 16);
			// Erased bytes only, as the core writes: an erase first when they aren't
			bool erased = true;
			for (unsigned k = 0; k < sizeof(b); k++) {
				erased = erased && model[at + k] == 0xFF;
			}
			if (!erased) {
				CHECK(erase_both(s * NAND_SECTORS_SECTOR));
			}
			CHECK(write_both(at, b, sizeof(b)));
		} else {
			CHECK(rewrite_both(s, i));
		}
		if (i % 500 == 499 && i < 2999) {
			CHECK(remount() && matches_model());
		}
	}
	CHECK(matches_model());
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.leveled > 0 && st.erases > 0);
	uint32_t lo = 0xFFFFFFFFu, hi = 0;
	for (uint32_t b = 0; b < 10; b++) {
		lo = nand.erases[b] < lo ? nand.erases[b] : lo;
		hi = nand.erases[b] > hi ? nand.erases[b] : hi;
	}
	printf("  gc: %u blocks emptied, %u records moved, %u leveled, erases per block %u..%u\n",
	       (unsigned)st.gc_blocks, (unsigned)st.gc_copies, (unsigned)st.leveled, (unsigned)lo, (unsigned)hi);
	CHECK(lo > 0 && hi <= 2 * lo + 4);	// every block takes its share
	CHECK(nand.violations == 0);

	// Every sector full and rewritten: the region holds the device plus the reserve
	for (uint32_t i = 0; i < 400; i++) {
		CHECK(rewrite_both(rnd() % 24, i + 7000));
	}
	CHECK(remount() && matches_model());
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.sectors_used == 24 && st.torn == 0);
	CHECK(nand.violations == 0);
}

static void test_bad_blocks(void)
{
	nand_sectors_stats_t st;

	// Factory bad blocks are never programmed or erased
	nand_create(8, 16);
	memset(model, 0xFF, sizeof(model));
	factory_bad(0);
	factory_bad(5);
	CHECK(mount(8, 16, 12));
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.blocks_bad == 2 && st.blocks_free == 6);
	for (uint32_t i = 0; i < 300; i++) {
		CHECK(rewrite_both(rnd() % 12, i));
	}
	CHECK(remount() && matches_model());
	CHECK(nand.violations == 0 && nand.erases[0] == 0 && nand.erases[5] == 0);

	// Blocks that start failing programs (the one being filled among them): what they hold moves out, then they are
	// retired for good
	CHECK(rewrite_both(0, 999));		// a block is open after the remount
	CHECK(ns.open < 8);
	nand.worn_program[ns.open % 8] = true;
	nand.worn_program[3] = true;
	for (uint32_t i = 0; i < 300; i++) {
		CHECK(rewrite_both(rnd() % 12, i + 1000));
	}
	CHECK(matches_model());
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.retired >= 1);
	CHECK(remount() && matches_model());
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.blocks_bad >= 3);

	// A block that won't erase is retired when it should be reused
	nand_create(8, 16);
	memset(model, 0xFF, sizeof(model));
	CHECK(mount(8, 16, 12));
	nand.worn_erase[2] = true;
	for (uint32_t i = 0; i < 400; i++) {
		CHECK(rewrite_both(rnd() % 12, i + 2000));
	}
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.retired >= 1);
	CHECK(remount() && matches_model());
	CHECK(nand.data[2 * 16][PAGE] == 0x00);	// marked

	// A page that reads back wrong is written again elsewhere; the block is kept
	nand_create(8, 16);
	memset(model, 0xFF, sizeof(model));
	CHECK(mount(8, 16, 12));
	CHECK(rewrite_both(1, 1));
	nand.flip_page = (uint32_t)ns.open * 16 + ns.next;
	CHECK(rewrite_both(2, 2));
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.verify_fails == 1 && st.retired == 0);
	CHECK(remount() && matches_model());
	CHECK(nand.violations == 0);
}

// --- Foreign data in the region ---

// A page someone else wrote, with the chip's ECC on: reads back clean. how: 0 data and spare garbage, 1 main area only
// (spare area erased), 2 only a stock-style block mark in spare bytes 4-5
static void foreign_page(uint32_t page, unsigned how)
{
	uint8_t *d = nand.data[page];
	memset(d, 0xFF, RAW);
	if (how == 0) {
		for (uint32_t i = 0; i < RAW; i++) {
			d[i] = (uint8_t)rnd();
		}
		d[PAGE] = 0xFF;		// not the factory bad-block mark: that block would be left alone anyway
	} else if (how == 1) {
		fill(d, PAGE, page);
	} else {
		d[PAGE + 4] = 0x00;
		d[PAGE + 5] = 0x00;
	}
	nand.ecc_fail[page] = false;
	nand.next[page / nand.ppb] = (uint16_t)(page % nand.ppb + 1u);
}

// Mount must refuse and leave the chip exactly as it was
static void expect_refused(uint16_t blocks, uint16_t ppb, uint16_t sectors, uint16_t block, const char *what)
{
	static uint8_t before[MAX_BLOCKS * MAX_PPB][RAW];
	nand_sectors_stats_t st;
	uint8_t buf[16];
	memcpy(before, nand.data, sizeof(before));
	uint32_t ops = nand.ops;
	bool ok = mount(blocks, ppb, sectors);
	nand_sectors_get_stats(&ns, &st);
	if (ok || st.status != NAND_SECTORS_FOREIGN || st.foreign_block != block) {
		printf("  foreign data not refused: %s\n", what);
	}
	CHECK(!ok && st.status == NAND_SECTORS_FOREIGN && st.foreign_block == block && st.foreign >= 1);
	CHECK(nand.ops == ops && memcmp(before, nand.data, sizeof(before)) == 0);	// nothing programmed or erased
	CHECK(!nand_sectors_write(&ns, 0, buf, sizeof(buf)) && !nand_sectors_read(&ns, 0, buf, sizeof(buf)) &&
	      !nand_sectors_erase(&ns, 0));
	CHECK(nand.ops == ops);
}

static void test_foreign(void)
{
	nand_sectors_stats_t st;

	// A blank region mounts
	fresh(8, 16, 12);
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.status == NAND_SECTORS_MOUNTED && st.foreign == 0);

	// So does a region of our own blocks, after many writes (and garbage collection)
	for (uint32_t i = 0; i < 200; i++) {
		CHECK(rewrite_both(rnd() % 12, i));
	}
	CHECK(remount() && matches_model());
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.status == NAND_SECTORS_MOUNTED && st.foreign == 0 && st.blocks_used > 0);

	// Someone else's data on a first page, three kinds, refused with nothing touched
	for (unsigned how = 0; how < 3; how++) {
		nand_create(8, 16);
		foreign_page(3 * 16, how);
		expect_refused(8, 16, 12, 3, "first page");
	}
	// ... on a last page only
	nand_create(8, 16);
	foreign_page(5 * 16 + 15, 0);
	expect_refused(8, 16, 12, 5, "last page");
	// ... a block full of it
	nand_create(8, 16);
	for (uint32_t p = 0; p < 16; p++) {
		foreign_page(6 * 16 + p, 0);
	}
	expect_refused(8, 16, 12, 6, "whole block");

	// Mixed: our records in some blocks, foreign data in a free one. Refused, and our data still there once the
	// foreign block is cleared by hand
	fresh(8, 16, 12);
	for (uint32_t i = 0; i < 30; i++) {
		CHECK(rewrite_both(i % 12, i + 500));
	}
	uint16_t victim = 0xFFFFu;
	for (uint16_t b = 0; b < 8 && victim == 0xFFFFu; b++) {
		if (live[b] == 0 && b != ns.open && nand.data[b * 16][PAGE + 8] == 0xFF) {
			victim = b;
		}
	}
	CHECK(victim != 0xFFFFu);
	foreign_page(victim * 16u, 1);
	expect_refused(8, 16, 12, victim, "mixed region");
	memset(nand.data[victim * 16u], 0xFF, RAW);
	nand.next[victim] = 0;
	CHECK(remount() && matches_model());

	// Ours wins over a foreign-looking page elsewhere in the block: a valid record on page 0, or on page 1 (page 0
	// one of ours that read back wrong)
	fresh(8, 16, 12);
	CHECK(rewrite_both(0, 1));
	uint16_t b0 = ns.open;
	nand.data[b0 * 16u + 15u][0] = 0x00;	// the block's last page programmed by something else
	nand.next[b0] = 16;
	CHECK(remount() && matches_model());
	fresh(8, 16, 12);
	CHECK(rewrite_both(0, 1));
	b0 = ns.open;
	nand.data[b0 * 16u][PAGE + 8] ^= 0x01;	// page 0's spare area no longer checks: ECC-clean garbage
	CHECK(remount());
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.status == NAND_SECTORS_MOUNTED && st.foreign == 0);

	// A first page a power cut left unreadable, the rest blank: what we leave, not foreign. The block is reused
	nand_create(8, 16);
	memset(model, 0xFF, sizeof(model));
	memset(nand.data[2 * 16], 0xA5, RAW);
	nand.data[2 * 16][PAGE] = 0xFF;
	nand.ecc_fail[2 * 16] = true;
	nand.next[2] = 1;
	CHECK(mount(8, 16, 12));
	for (uint32_t i = 0; i < 200; i++) {
		CHECK(rewrite_both(rnd() % 12, i + 700));
	}
	CHECK(nand.erases[2] > 0 && remount() && matches_model());

	// Foreign data the mount didn't see (a read error then): checked again before the block is erased, skipped, and
	// the layer goes on in the other blocks
	fresh(8, 16, 12);
	CHECK(rewrite_both(0, 1));
	uint16_t late = (uint16_t)((ns.open + 3u) % 8u);
	foreign_page(late * 16u, 0);
	uint8_t keep[RAW];
	memcpy(keep, nand.data[late * 16u], RAW);
	for (uint32_t i = 0; i < 200; i++) {
		CHECK(rewrite_both(rnd() % 12, i + 900));
	}
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.foreign == 1 && st.foreign_block == late && nand.erases[late] == 0);
	CHECK(memcmp(keep, nand.data[late * 16u], RAW) == 0 && matches_model());
	CHECK(nand.violations == 0);

	// A first page the chip fails to program (left with a garbled spare area): the block is retired at once, so a
	// power cut right after doesn't leave it looking foreign to the next mount
	fresh(8, 16, 12);
	CHECK(rewrite_both(0, 1));
	uint16_t keep_open = ns.open;
	for (uint16_t b = 0; b < 8; b++) {
		nand.worn_program[b] = b != keep_open;
	}
	bool failed_write = false;
	for (uint32_t i = 0; i < 40 && !failed_write; i++) {
		failed_write = !rewrite_both(1 + i % 11, i + 1100);
	}
	CHECK(failed_write);
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.retired >= 1);
	memset(nand.worn_program, 0, sizeof(nand.worn_program));
	CHECK(remount() && matches_model());		// the power cut: nothing else ran in between
	nand_sectors_get_stats(&ns, &st);
	CHECK(st.status == NAND_SECTORS_MOUNTED && st.foreign == 0);
}

typedef bool (*step_fn)(void);

// The layer's RAM state, to run a step again from the same point
static struct {
	nand_sectors_t ns;
	uint16_t map[MAX_SECTORS];
	uint8_t live[MAX_BLOCKS], flags[MAX_BLOCKS];
	uint32_t order[MAX_BLOCKS];
} saved;

static void layer_save(void)
{
	saved.ns = ns;
	memcpy(saved.map, map, sizeof(map));
	memcpy(saved.live, live, sizeof(live));
	memcpy(saved.flags, flags, sizeof(flags));
	memcpy(saved.order, order, sizeof(order));
}

static void layer_restore(void)
{
	ns = saved.ns;
	memcpy(map, saved.map, sizeof(map));
	memcpy(live, saved.live, sizeof(live));
	memcpy(flags, saved.flags, sizeof(flags));
	memcpy(order, saved.order, sizeof(order));
}

// Each step is one call into the layer; its prep runs before the cut is armed
static uint32_t step_sector;
static bool prep_none(void)
{
	return true;
}

static bool prep_erased(void)
{
	return erase_both(step_sector * NAND_SECTORS_SECTOR);
}

static bool prep_half(void)
{
	uint8_t b[NAND_SECTORS_SECTOR / 2];
	fill(b, sizeof(b), 55);
	return prep_erased() && write_both(step_sector * NAND_SECTORS_SECTOR, b, sizeof(b));
}

// A whole sector programmed into an erased one (core/storage's rewrite after its erase)
static bool step_fill(void)
{
	static uint8_t b[NAND_SECTORS_SECTOR];
	fill(b, sizeof(b), 999);
	return write_both(step_sector * NAND_SECTORS_SECTOR, b, sizeof(b));
}

// A record into an erased slot of a sector that holds data (a settings save)
static bool step_small(void)
{
	uint8_t b[64];
	fill(b, sizeof(b), 77);
	return write_both(step_sector * NAND_SECTORS_SECTOR + 3000, b, sizeof(b));
}

static bool step_erase(void)
{
	return erase_both(step_sector * NAND_SECTORS_SECTOR);
}

// Run step with a power cut at its n-th operation, for every n until it completes without one, in every tear mode.
// Each time: mount again, the sector it touched is old or new (whole), every other sector unchanged; then more
// writes and another remount, so a torn page left behind can't come back later
static bool cut_everywhere(const char *name, step_fn prep, step_fn step, uint16_t blocks, uint16_t ppb,
			   uint16_t sectors, uint32_t setup_writes)
{
	bool gc = false;
	uint32_t torn = 0;
	static uint8_t before[MAX_SECTORS * NAND_SECTORS_SECTOR], after[MAX_SECTORS * NAND_SECTORS_SECTOR];
	static uint8_t snap_data[sizeof(nand)];
	static uint8_t snap_model[sizeof(model)];
	uint32_t cuts = 0;

	for (unsigned mode = 0; mode < TEAR_MODES; mode++) {
		fresh(blocks, ppb, sectors);
		for (uint32_t s = 0; s < sectors; s++) {
			CHECK(rewrite_both(s, s + 100));
		}
		for (uint32_t i = 0; i < setup_writes; i++) {
			CHECK(rewrite_both(rnd() % sectors, i + 300));
		}
		CHECK(remount());
		CHECK(prep());
		// The expected outcome without a cut
		nand_sectors_stats_t st;
		nand_sectors_get_stats(&ns, &st);
		uint32_t gc0 = st.gc_blocks;
		memcpy(snap_data, &nand, sizeof(nand));
		memcpy(snap_model, model, sizeof(model));
		layer_save();
		memcpy(before, model, sizeof(model));
		uint32_t ops0 = nand.ops;
		CHECK(step());
		uint32_t total = nand.ops - ops0;
		nand_sectors_get_stats(&ns, &st);
		gc = gc || st.gc_blocks != gc0;
		memcpy(after, model, sizeof(model));
		for (uint32_t n = 1; n <= total; n++) {
			memcpy(&nand, snap_data, sizeof(nand));
			memcpy(model, snap_model, sizeof(model));
			layer_restore();	// the layer as it was before the step (no remount in between)
			nand.tear = (tear_t)mode;
			nand.cut_at = nand.ops + n;
			step();
			nand.cut_at = 0;
			CHECK(remount());
			cuts++;
			// A torn page found is dealt with once: the next mount finds none
			nand_sectors_get_stats(&ns, &st);
			torn += st.torn;
			if (st.torn > 0) {
				CHECK(remount());
				nand_sectors_get_stats(&ns, &st);
				CHECK(st.torn == 0);
			}
			// Old or new for the stepped sector, the rest untouched
			static uint8_t buf[NAND_SECTORS_SECTOR];
			for (uint32_t s = 0; s < sectors; s++) {
				uint32_t a = s * NAND_SECTORS_SECTOR;
				CHECK(nand_sectors_read(&ns, a, buf, sizeof(buf)));
				bool old = memcmp(buf, &before[a], sizeof(buf)) == 0;
				bool new = memcmp(buf, &after[a], sizeof(buf)) == 0;
				if (!(old || new) || (s != step_sector && !old)) {
					printf("  %s: cut at %u of %u (tear %u): sector %u is %s\n", name, (unsigned)n,
					       (unsigned)total, mode, (unsigned)s, old || new ? "changed" : "mixed");
					failures++;
				}
				memcpy(&model[a], buf, sizeof(buf));	// what is there now is the model from here on
			}
			// Life goes on: more rewrites, a remount, still consistent
			for (uint32_t i = 0; i < 30; i++) {
				CHECK(rewrite_both(rnd() % sectors, i + 5000));
			}
			CHECK(remount() && matches_model());
			CHECK(nand.violations == 0);
		}
	}
	if (cuts > 0) {
		printf("  %s: %u cuts, %u torn pages found%s\n", name, (unsigned)cuts, (unsigned)torn,
		       gc ? " (with garbage collection)" : "");
	}
	return gc;
}

static void test_power_cuts(void)
{
	// A sector filled, a small write, an erase, in a quiet region
	step_sector = 3;
	cut_everywhere("fill", prep_erased, step_fill, 8, 16, 10, 0);
	cut_everywhere("small write", prep_half, step_small, 8, 16, 10, 0);
	cut_everywhere("erase", prep_none, step_erase, 8, 16, 10, 0);
	// The same in a region so full that steps run garbage collection first (copies, then the victim's erase)
	// Several random histories: one of them once left the mount's torn-record fix-up taking the last free block, and
	// the next write found no block to collect into
	bool gc = false;
	static const uint32_t seeds[] = { 12345u, 1u };
	for (unsigned k = 0; k < sizeof(seeds) / sizeof(seeds[0]); k++) {
		rng = seeds[k];
		for (uint32_t w = 30; w < 40; w++) {
			gc = cut_everywhere("fill, full region", prep_erased, step_fill, 6, 8, 12, w) || gc;
			gc = cut_everywhere("erase, full region", prep_none, step_erase, 6, 8, 12, w) || gc;
		}
	}
	CHECK(gc);
}

// --- core/storage and core/settings on the layer, through a hal_flash_t ---

static bool hf_init(void) { return true; }
static bool hf_read(uint32_t a, void *b, uint32_t n) { return nand_sectors_read(&ns, a, b, n); }
static bool hf_write(uint32_t a, const void *b, uint32_t n) { return nand_sectors_write(&ns, a, b, n); }
static bool hf_erase(uint32_t a) { return nand_sectors_erase(&ns, a); }

// The DB25-D's logical map (boards/db25d/flash_db25d.c), cut down to what fits the simulated region
static const hal_flash_map_t hf_map = {
	.settings_addr = 0x011000u, .settings_size = 0x8000u, .journal_addr = 0x019000u,
	.openfw_eeprom = 0, .openfw_flash = 0x020000u, .openfw_end = 0x40000u,
};
static hal_flash_t hf = {
	.init = hf_init, .size = 0x40000u, .erase_size = 4096, .read = hf_read, .write = hf_write, .erase = hf_erase,
	.map = &hf_map,
};
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 2, .flash = &hf };

static void boot(void)
{
	CHECK(remount());
	CHECK(storage_init());
	settings_init();
}

static void test_settings(void)
{
	settings_stats_t st;

	nand_create(20, 64);
	CHECK(mount(20, 64, 0x40000u / NAND_SECTORS_SECTOR));
	CHECK(storage_init());
	CHECK(settings_init() == SETTINGS_DEFAULTS);
	settings_t *s = settings_begin();
	s->squelch = 3;
	s->brightness = 40;
	settings_end(0);
	CHECK(settings_flush());

	// A reboot: the settings are loaded
	boot();
	settings_get_stats(&st);
	CHECK(st.load == SETTINGS_LOADED);
	settings_t r;
	settings_read(&r);
	CHECK(r.squelch == 3 && r.brightness == 40);

	// Many saves: round the ring, through garbage collection, every one survives a reboot
	for (uint32_t i = 0; i < 200; i++) {
		s = settings_begin();
		s->brightness = (uint8_t)(10 + i % 90);
		settings_end(i);
		CHECK(settings_flush());
		if (i % 37 == 0) {
			boot();
			settings_read(&r);
			CHECK(r.brightness == (uint8_t)(10 + i % 90));
		}
	}
	boot();
	settings_read(&r);
	CHECK(r.brightness == (uint8_t)(10 + 199 % 90) && r.squelch == 3);

	// A journaled sector rewrite (a codeplug sector) on top
	static uint8_t sec[STORAGE_SECTOR], back[STORAGE_SECTOR];
	fill(sec, sizeof(sec), 42);
	CHECK(storage_sector_write(0x030000u, sec));
	boot();
	CHECK(hf_read(0x030000u, back, sizeof(back)) && memcmp(back, sec, sizeof(sec)) == 0);

	// A power cut at every operation of a save: after the reboot the old or the new value, never the defaults
	uint32_t cuts = 0;
	for (uint32_t n = 1;; n++) {
		boot();
		settings_read(&r);
		uint8_t old = r.brightness, want = (uint8_t)(old == 77 ? 78 : 77);
		s = settings_begin();
		s->brightness = want;
		settings_end(0);
		nand.tear = (tear_t)(n % TEAR_MODES);
		nand.cut_at = nand.ops + n;
		bool done = settings_flush() && !nand.off;
		nand.cut_at = 0;
		boot();
		settings_get_stats(&st);
		settings_read(&r);
		CHECK(st.load == SETTINGS_LOADED && r.squelch == 3);
		CHECK(r.brightness == old || r.brightness == want);
		CHECK(!done || r.brightness == want);
		cuts++;
		if (done) {
			break;
		}
	}
	printf("  settings: %u cuts\n", (unsigned)cuts);
	CHECK(nand.violations == 0);
}

// --- The open-firmware CPS on the layer: the DB25-D's codeplug write (docs/spec/hardware-db25d.md 6.6, 9) ---
//
// The logical map of boards/db25d/flash_db25d.c up to the end of the open-firmware flash image (no native codeplug or
// staging: the CPS never writes them), on a bigger layer than the tests above use. hal_flash_t.replace is the DB25-D's
// (nand_sectors_replace): one record per sector, no journal.

#define BIG_SECTORS	0x120u			// 0x120000 / 4096
// 13 blocks of 64 pages: the 271 image sectors take 542 pages (8.5 blocks), so garbage collection runs all the time
#define BIG_BLOCKS	13u
static uint16_t big_map[BIG_SECTORS];
static uint8_t big_live[MAX_BLOCKS], big_flags[MAX_BLOCKS];
static uint32_t big_order[MAX_BLOCKS];

static const hal_flash_map_t big_flash_map = {
	.settings_addr = 0x011000u, .settings_size = 0x008000u, .journal_addr = 0x019000u,
	.openfw_eeprom = 0x000000u, .openfw_flash = 0x020000u, .openfw_end = 0x120000u,
	.native_addr = 0x120000u, .native_size = 0,
};

static bool hf_replace(uint32_t a, const void *b) { return nand_sectors_replace(&ns, a, b); }

static bool mount_big(void)
{
	dev.blocks = BIG_BLOCKS;
	dev.pages_per_block = MAX_PPB;
	memset(&ns, 0, sizeof(ns));
	ns.dev = &dev;
	ns.sectors = BIG_SECTORS;
	ns.map = big_map;
	ns.live = big_live;
	ns.flags = big_flags;
	ns.order = big_order;
	return nand_sectors_mount(&ns);
}

static uint8_t cps_out[4096];
static size_t cps_out_len;
static uint32_t cps_now = 1000;
static int cps_reboots;

static void cps_write_out(const uint8_t *b, size_t n)
{
	if (cps_out_len + n <= sizeof(cps_out)) {
		memcpy(cps_out + cps_out_len, b, n);
	}
	cps_out_len += n;
}
static void cps_reboot_req(bool save) { (void)save; cps_reboots++; }
static const cps_io_t cps_io = { .write = cps_write_out, .reboot = cps_reboot_req };

// What the radio does at power on, after the layer is mounted
static bool boot_big(void)
{
	nand.off = false;
	nand.cut_at = 0;
	bool ok = mount_big() && storage_init();
	settings_init();
	codeplug_init();
	cps_init(&cps_io);
	return ok;
}

static size_t cps_req(const uint8_t *b, size_t n)
{
	cps_out_len = 0;
	cps_feed(b, n, cps_now);
	for (int i = 0; i < CPS_STALL_MS / 5 + 3 && cps_out_len == 0; i++) {
		cps_now += 5;
		cps_feed(NULL, 0, cps_now);
	}
	cps_now += 1;
	return cps_out_len;
}

static bool cps_ctl(uint8_t sub, uint8_t arg)
{
	uint8_t r[3] = { 'C', sub, arg };
	cps_req(r, sub == 6 ? 3 : 2);
	return cps_out_len == 1 && cps_out[0] == '-';
}

// R area 1: n bytes of the flash at addr
static bool cps_read(uint32_t addr, uint8_t *dst, uint16_t n)
{
	uint8_t r[8] = { 'R', 1, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
			 (uint8_t)(n >> 8), (uint8_t)n };
	cps_req(r, sizeof(r));
	if (cps_out_len != 3u + n || cps_out[0] != 'R') {
		return false;
	}
	memcpy(dst, cps_out + 3, n);
	return true;
}

// X 1 (prepare), X 2 (data, in 1 KB requests), X 3 (commit) for the sector at addr; false at the first '-'
static bool cps_write_sector(uint32_t addr, const uint8_t *data)
{
	uint8_t r[8 + 1024];
	uint32_t sec = addr / STORAGE_SECTOR;
	r[0] = 'X'; r[1] = 1; r[2] = (uint8_t)(sec >> 16); r[3] = (uint8_t)(sec >> 8); r[4] = (uint8_t)sec;
	cps_req(r, 5);
	if (cps_out_len != 2 || cps_out[0] != 'X') {
		return false;
	}
	for (uint32_t off = 0; off < STORAGE_SECTOR; off += 1024) {
		uint32_t a = addr + off;
		r[1] = 2;
		r[2] = (uint8_t)(a >> 24); r[3] = (uint8_t)(a >> 16); r[4] = (uint8_t)(a >> 8); r[5] = (uint8_t)a;
		r[6] = 1024 >> 8; r[7] = 1024 & 0xFF;
		memcpy(r + 8, data + off, 1024);
		cps_req(r, sizeof(r));
		if (cps_out_len != 2) {
			return false;
		}
	}
	r[1] = 3;
	cps_req(r, 2);
	return cps_out_len == 2 && cps_out[0] == 'X' && cps_out[1] == 3;
}

// A CPS image sector: pseudo-random bytes, 0xFF where the seed says (a CPS leaves a lot of a sector blank)
static void image_sector(uint8_t *b, uint32_t sector, uint32_t pass)
{
	fill(b, STORAGE_SECTOR, sector * 7u + pass * 1000u + 1u);
	for (uint32_t i = 0; i < STORAGE_SECTOR; i++) {
		b[i] ^= (uint8_t)(pass * 0x5Bu + (i >> 8) * 0x11u + (sector >> 3));	// the seed's high bits don't reach fill's bytes
	}
	if (sector % 5u == 0) {
		memset(b + 2048, 0xFF, 2048);
	}
}

static bool cps_sector_is_image(uint32_t sector)
{
	// The EEPROM image sectors but the one holding the VFO records (the shim overlays the live VFO there), and the
	// whole open-firmware flash image
	return (sector < 16u && sector != 7u) || (sector >= 0x20u && sector < 0x120u);
}

static void test_cps(void)
{
	static uint8_t img[STORAGE_SECTOR], back[STORAGE_SECTOR], dev_before[BIG_SECTORS][STORAGE_SECTOR];
	static uint8_t snap_data[sizeof(nand)];
	static uint16_t snap_map[BIG_SECTORS];
	static uint8_t snap_live[MAX_BLOCKS], snap_flags[MAX_BLOCKS];
	static uint32_t snap_order[MAX_BLOCKS];
	static nand_sectors_t snap_ns;

	hf.size = BIG_SECTORS * STORAGE_SECTOR;
	hf.map = &big_flash_map;
	hf.replace = hf_replace;
	nand_create(BIG_BLOCKS, MAX_PPB);
	CHECK(boot_big());
	CHECK(storage_region() == STORAGE_REGION_OURS);

	// 1. A whole codeplug written like qdmr's driver does: every sector of the images, twice (the second pass changes
	// every byte, so every sector needs real 0 -> 1 transitions that a NOR write refuses)
	CHECK(cps_ctl(0, 0));
	uint32_t sectors = 0;
	for (uint32_t pass = 0; pass < 2; pass++) {
		uint32_t p0 = nand.programs, e0 = 0;
		for (uint32_t b = 0; b < MAX_BLOCKS; b++) {
			e0 += nand.erases[b];
		}
		uint32_t w = 0;
		for (uint32_t s = 0; s < BIG_SECTORS; s++) {
			if (!cps_sector_is_image(s)) {
				continue;
			}
			image_sector(img, s, pass);
			if (!cps_write_sector(s * STORAGE_SECTOR, img)) {
				printf("  sector %u pass %u refused\n", (unsigned)s, (unsigned)pass);
				failures++;
				break;
			}
			w++;
		}
		uint32_t e1 = 0;
		for (uint32_t b = 0; b < MAX_BLOCKS; b++) {
			e1 += nand.erases[b];
		}
		sectors = w;
		printf("  CPS write of the images, pass %u: %u sectors, %u page programs (%.2f per sector), %u block erases\n",
		       (unsigned)pass, (unsigned)w, (unsigned)(nand.programs - p0), (double)(nand.programs - p0) / w,
		       (unsigned)(e1 - e0));
		CHECK(nand.programs - p0 <= 2u * w + 2u * 64u);	// two pages a sector; a little more for the copies garbage collection makes
	}
	CHECK(sectors == 15u + 256u);
	CHECK(cps_ctl(5, 0));
	// Read back through the CPS, then again after a power cycle
	for (unsigned round = 0; round < 2; round++) {
		for (uint32_t s = 0; s < BIG_SECTORS; s++) {
			if (!cps_sector_is_image(s)) {
				continue;
			}
			image_sector(img, s, 1);
			if (!cps_read(s * STORAGE_SECTOR, back, 2048) || memcmp(back, img, 2048) != 0 ||
			    !cps_read(s * STORAGE_SECTOR + 2048, back, 2048) || memcmp(back, img + 2048, 2048) != 0) {
				printf("  round %u: sector %u reads back wrong\n", round, (unsigned)s);
				failures++;
				break;
			}
		}
		CHECK(boot_big());
	}
	CHECK(nand.violations == 0);
	// The areas the radio keeps for itself are refused (no write reaches them), the settings survived
	{
		uint8_t r[5] = { 'X', 1, 0, 0, 0x11 };		// the settings ring
		cps_req(r, 5);
		CHECK(cps_out_len == 1 && cps_out[0] == '-');
		r[3] = 0x01;
		r[4] = 0x20;					// 0x120000: the native codeplug
		cps_req(r, 5);
		CHECK(cps_out_len == 1 && cps_out[0] == '-');
	}

	// 2. The same sector write through the journal (no replace): the cost the NOR path would have on this NAND
	hf.replace = NULL;
	{
		uint32_t p0 = nand.programs;
		for (uint32_t s = 0x30; s < 0x40; s++) {
			image_sector(img, s, 5);
			CHECK(cps_write_sector(s * STORAGE_SECTOR, img));
		}
		printf("  journaled (no replace): %.1f page programs per sector\n", (double)(nand.programs - p0) / 16.0);
		CHECK(nand.programs - p0 >= 16u * 8u);
	}
	hf.replace = hf_replace;
	{
		uint32_t p0 = nand.programs;
		for (uint32_t s = 0x30; s < 0x40; s++) {
			image_sector(img, s, 6);
			CHECK(cps_write_sector(s * STORAGE_SECTOR, img));
		}
		printf("  replace: %.1f page programs per sector\n", (double)(nand.programs - p0) / 16.0);
		CHECK(nand.programs - p0 <= 16u * 3u);
	}

	// 3. A power cut at every NAND operation of one sector's CPS commit, in every tear mode, in a region that has been
	// through garbage collection: after the power cycle the sector is the old one or the new one, whole, and no other
	// sector or the settings changed
	uint32_t cuts = 0, torn = 0;
	static const uint32_t targets[] = { 0x03u, 0x0Cu, 0x25u, 0x66u, 0xA0u, 0x11Fu, 0x40u, 0x41u };
	bool gc_cut = false;
	for (unsigned t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
		for (unsigned mode = 0; mode < TEAR_MODES; mode++) {
			uint32_t sec = targets[t];
			CHECK(boot_big());
			// Churn first: a different number of rewrites each time puts the stream at another spot in its block, so
			// some commits run garbage collection
			for (uint32_t i = 0; i < 31u * t + 7u * mode; i++) {
				uint32_t cs = 0x20u + (i * 37u + t) % 0x100u;
				image_sector(img, cs, 300u + i);
				CHECK(cps_write_sector(cs * STORAGE_SECTOR, img));
			}
			for (uint32_t s = 0; s < BIG_SECTORS; s++) {
				CHECK(nand_sectors_read(&ns, s * STORAGE_SECTOR, dev_before[s], STORAGE_SECTOR));
			}
			static uint8_t want[STORAGE_SECTOR];
			image_sector(want, sec, 100u + t);
			memcpy(snap_data, &nand, sizeof(nand));
			snap_ns = ns;
			memcpy(snap_map, big_map, sizeof(big_map));
			memcpy(snap_live, big_live, sizeof(big_live));
			memcpy(snap_flags, big_flags, sizeof(big_flags));
			memcpy(snap_order, big_order, sizeof(big_order));
			uint32_t o0 = nand.ops;
			nand_sectors_stats_t g0;
			nand_sectors_get_stats(&ns, &g0);
			bool wrote = cps_write_sector(sec * STORAGE_SECTOR, want);
			uint32_t total = nand.ops - o0;
			CHECK(wrote);
			nand_sectors_stats_t g1;
			nand_sectors_get_stats(&ns, &g1);
			gc_cut = gc_cut || g1.gc_blocks != g0.gc_blocks;
			if (!wrote) {
				nand_sectors_stats_t d;
				nand_sectors_get_stats(&ns, &d);
				storage_stats_t ss;
				storage_get_stats(&ss);
				printf("  write refused: free %u used %u bad %u errors %u storage errors %u out %zu %02x\n", d.blocks_free, d.blocks_used, d.blocks_bad, d.errors, ss.errors, cps_out_len, cps_out[0]);
			}
			if (total < 2) {
				printf("  target %#x mode %u: %u ops\n", (unsigned)sec, mode, (unsigned)total);
				failures++;
			}
			for (uint32_t n = 1; n <= total; n++) {
				memcpy(&nand, snap_data, sizeof(nand));
				ns = snap_ns;
				memcpy(big_map, snap_map, sizeof(big_map));
				memcpy(big_live, snap_live, sizeof(big_live));
				memcpy(big_flags, snap_flags, sizeof(big_flags));
				memcpy(big_order, snap_order, sizeof(big_order));
				cps_init(&cps_io);
				nand.tear = (tear_t)mode;
				nand.cut_at = nand.ops + n;
				bool done = cps_write_sector(sec * STORAGE_SECTOR, want);
				nand.cut_at = 0;
				CHECK(boot_big());
				cuts++;
				nand_sectors_stats_t st;
				nand_sectors_get_stats(&ns, &st);
				torn += st.torn;
				for (uint32_t s = 0; s < BIG_SECTORS; s++) {
					CHECK(nand_sectors_read(&ns, s * STORAGE_SECTOR, back, STORAGE_SECTOR));
					bool old = memcmp(back, dev_before[s], STORAGE_SECTOR) == 0;
					bool new = memcmp(back, want, STORAGE_SECTOR) == 0;
					if (s == sec ? !(old || new) || (done && !new) : !old) {
						printf("  cut %u/%u tear %u sector %#x: sector %#x is %s\n", (unsigned)n,
						       (unsigned)total, mode, (unsigned)sec, (unsigned)s,
						       s == sec && !old && !new ? "mixed" : "wrong");
						failures++;
					}
				}
				// The radio goes on: another CPS write works and survives a power cycle
				image_sector(img, sec, 200u);
				CHECK(cps_write_sector(sec * STORAGE_SECTOR, img));
				CHECK(boot_big());
				CHECK(nand_sectors_read(&ns, sec * STORAGE_SECTOR, back, STORAGE_SECTOR) &&
				      memcmp(back, img, STORAGE_SECTOR) == 0);
			}
		}
	}
	printf("  CPS sector commit: %u power cuts, %u torn pages found%s\n", (unsigned)cuts, (unsigned)torn,
	       gc_cut ? " (with garbage collection)" : "");
	CHECK(gc_cut);
	CHECK(nand.violations == 0);

	// 4. A codeplug written through the CPS shows up in the radio: channels the codeplug API stored, a CPS edit of
	// the channel bank's sector (a new name and frequency, bits going 0 -> 1), and the radio re-reads it at close
	CHECK(boot_big());
	channel_t c;
	memset(&c, 0, sizeof(c));
	strcpy(c.name, "First");
	c.rx_hz = c.tx_hz = 461462500u;
	c.mode = RADIO_MODE_FM;
	c.power = CODEPLUG_POWER_DEFAULT;
	CHECK(codeplug_set_channel(1, &c));
	static uint8_t sec3[STORAGE_SECTOR];
	CHECK(cps_ctl(0, 0));
	CHECK(cps_read(0x3000u, sec3, 2048) && cps_read(0x3800u, sec3 + 2048, 2048));
	// Channel 1's record is at 0x3790: name, then RX and TX as BCD 10 Hz units
	CHECK(memcmp(&sec3[0x790], "First", 5) == 0);
	memset(&sec3[0x790], 0xFF, 16);
	memcpy(&sec3[0x790], "Test DMR TG201", 14);
	sec3[0x790 + 0x10] = 0x00; sec3[0x790 + 0x11] = 0x00; sec3[0x790 + 0x12] = 0x20; sec3[0x790 + 0x13] = 0x46;
	sec3[0x790 + 0x14] = 0x00; sec3[0x790 + 0x15] = 0x00; sec3[0x790 + 0x16] = 0x20; sec3[0x790 + 0x17] = 0x46;
	sec3[0x790 + 0x18] = 1;		// DMR
	sec3[0x790 + 0x2C] = 2;		// color code 2
	CHECK(cps_write_sector(0x3000u, sec3));
	uint32_t serial = codeplug_serial();
	CHECK(cps_ctl(5, 0));
	CHECK(codeplug_serial() != serial);
	CHECK(codeplug_channel(1, &c));
	CHECK(strcmp(c.name, "Test DMR TG201") == 0 && c.rx_hz == 462000000u && c.mode == RADIO_MODE_DMR && c.color_code == 2);
	CHECK(boot_big() && codeplug_channel(1, &c) && strcmp(c.name, "Test DMR TG201") == 0);
	CHECK(nand.violations == 0);
}

int main(void)
{
	test_basic();
	test_busy_hook();
	test_gc_and_wear();
	test_bad_blocks();
	test_foreign();
	test_power_cuts();
	test_settings();
	test_cps();
	if (failures) {
		printf("nandSectorsTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("nandSectorsTest: all passed\n");
	return 0;
}
