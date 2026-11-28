// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// drivers/flash/nand_blocks on a simulated NAND region (the simulation of nandSectorsTest: pages programmed once and
// in order, factory bad and worn blocks, power cuts that tear the operation in progress). After every cut the region
// is mounted again and each logical block must hold exactly what it held after some earlier call, whole: never a mix
// of two.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flash/nand_blocks/nand_blocks.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- The simulated NAND ---

#define PAGE		2048u
#define SPARE		64u
#define RAW		(PAGE + SPARE)
#define MAX_BLOCKS	24u
#define MAX_PPB		64u

typedef enum { TEAR_ECC, TEAR_DATA, TEAR_BLANK, TEAR_MODES } tear_t;

typedef struct {
	uint16_t ppb, blocks;
	uint8_t data[MAX_BLOCKS * MAX_PPB][RAW];
	bool ecc_fail[MAX_BLOCKS * MAX_PPB];
	uint16_t next[MAX_BLOCKS];
	bool factory_bad[MAX_BLOCKS];
	bool worn_program[MAX_BLOCKS], worn_erase[MAX_BLOCKS];
	uint16_t worn_from[MAX_BLOCKS];			// a worn block's programs fail from this page on
	uint32_t erases[MAX_BLOCKS];
	uint32_t violations;
	uint32_t ops, programs, reads;
	uint32_t cut_at;
	tear_t tear;
	bool off;
} sim_t;

static sim_t nand, saved;

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
}

static void factory_bad(uint16_t b)
{
	nand.factory_bad[b] = true;
	for (uint32_t p = 0; p < nand.ppb; p++) {
		memset(nand.data[b * nand.ppb + p], 0x00, RAW);
		nand.ecc_fail[b * nand.ppb + p] = true;
	}
}

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
	if (page >= (uint32_t)nand.blocks * nand.ppb || n > 2) {
		return NAND_ERROR;
	}
	if (src != NAND_SECTORS_NO_PAGE) {
		if (nand.off || src >= (uint32_t)nand.blocks * nand.ppb || nand.ecc_fail[src]) {
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
	if (page % nand.ppb < nand.next[b]) {
		nand.violations++;		// twice, or out of order
	}
	nand.next[b] = (uint16_t)(page % nand.ppb + 1u);
	uint8_t *d = nand.data[page];
	if (torn) {
		switch (nand.tear) {
		case TEAR_ECC:
			for (uint32_t i = 0; i < RAW; i++) {
				d[i] &= (uint8_t)(buf[i] | (rnd() & 0xFFu));
			}
			nand.ecc_fail[page] = true;
			break;
		case TEAR_DATA:
			for (uint32_t i = 0; i < RAW; i++) {
				d[i] &= i < PAGE / 2 || i >= PAGE ? buf[i] : 0xFF;
			}
			break;
		default:
			break;
		}
		return NAND_ERROR;
	}
	if (nand.worn_program[b] && page % nand.ppb >= nand.worn_from[b]) {
		for (uint32_t i = 0; i < RAW; i++) {
			d[i] &= (uint8_t)(buf[i] | 0x0F);
		}
		return NAND_FAILED;
	}
	for (uint32_t i = 0; i < RAW; i++) {
		d[i] &= buf[i];
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
		nand.next[b] = nand.ppb;
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

static unsigned busy_calls;
static void sim_busy(void *ctx)
{
	(void)ctx;
	busy_calls++;
}

static const uint8_t mark10[10] = { 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 };

static nand_dev_t dev = {
	.ctx = NULL, .spare_size = SPARE,
	.read = sim_read, .program = sim_program, .erase = sim_erase, .block_bad = sim_block_bad, .mark_bad = sim_mark_bad,
	.busy = sim_busy, .factory_mark = mark10, .factory_mark_len = sizeof(mark10),
};

// --- The layer and a model of what it should hold ---

#define MAX_LOGICAL	16u
#define MAX_BYTES	(MAX_LOGICAL * MAX_PPB * PAGE)

static uint16_t map[MAX_LOGICAL];
static uint8_t flags[MAX_BLOCKS];
static nand_blocks_t nb;
static uint8_t model[MAX_BYTES];
static uint8_t buf[MAX_BYTES];

static uint32_t bsize(void)
{
	return (uint32_t)nand.ppb * PAGE;
}

static bool mount(uint16_t phys, uint16_t ppb, uint16_t logical)
{
	dev.blocks = phys;
	dev.pages_per_block = ppb;
	memset(&nb, 0, sizeof(nb));
	nb.dev = &dev;
	nb.blocks = logical;
	nb.map = map;
	nb.flags = flags;
	return nand_blocks_mount(&nb);
}

static bool remount(void)
{
	return mount(dev.blocks, dev.pages_per_block, nb.blocks);
}

static bool matches_model(void)
{
	uint32_t size = nand_blocks_size(&nb);
	return nand_blocks_read(&nb, 0, buf, size) && memcmp(buf, model, size) == 0;
}

static void fill(uint8_t *p, uint32_t len, uint32_t seed)
{
	for (uint32_t i = 0; i < len; i++) {
		p[i] = (uint8_t)((seed * 2654435761u + i * 40503u) >> 13);
	}
}

static bool replace(uint32_t addr, uint32_t seed)
{
	uint8_t s[NAND_BLOCKS_SECTOR];
	fill(s, sizeof(s), seed);
	bool ok = nand_blocks_replace(&nb, addr, s);
	if (ok) {
		memcpy(&model[addr], s, sizeof(s));
	}
	return ok;
}

static void start(uint16_t phys, uint16_t ppb, uint16_t logical)
{
	nand_create(phys, ppb);
	memset(model, 0xFF, sizeof(model));
	CHECK(mount(phys, ppb, logical));
}

// --- Tests ---

static void test_setup(void)
{
	nand_create(8, 8);
	CHECK(!mount(8, 8, 8));				// no spare block
	CHECK(nb.stats.status == NAND_BLOCKS_BAD_SETUP);
	CHECK(!mount(8, 8, 0));
	CHECK(!mount(8, 1, 4));
	nb.map = NULL;
	CHECK(!nand_blocks_mount(&nb));
	CHECK(mount(8, 8, 7));
	CHECK(nand_blocks_size(&nb) == 7u * 8u * PAGE);
	CHECK(nand.ops == 0);				// a mount programs and erases nothing
	uint8_t b[16];
	CHECK(!nand_blocks_read(&nb, nand_blocks_size(&nb) - 8, b, 16));	// past the end
	CHECK(!nand_blocks_replace(&nb, 100, b));	// unaligned
	CHECK(!nand_blocks_erase(&nb, 2048));
	nand_blocks_t un = { 0 };
	CHECK(!nand_blocks_sync(&un));
}

static void test_empty_and_sequential(void)
{
	start(12, 8, 10);
	uint32_t reads = nand.reads;
	CHECK(matches_model());
	CHECK(nand.reads == reads);			// unwritten blocks read 0xFF without the chip
	// The whole device in address order, as a host writes a database: one erase per logical block, no moves
	for (uint32_t a = 0; a < nand_blocks_size(&nb); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a / NAND_BLOCKS_SECTOR + 1));
	}
	CHECK(nand_blocks_sync(&nb));
	nand_blocks_stats_t st;
	nand_blocks_get_stats(&nb, &st);
	CHECK(st.erases == 10);
	CHECK(st.moves == 0);
	CHECK(st.programs == 10u * 8u);
	CHECK(st.commits == 10);
	CHECK(st.blocks_used == 10 && st.blocks_free == 2 && st.blocks_bad == 0);
	CHECK(matches_model());
	CHECK(remount());
	CHECK(matches_model());
	CHECK(nand.violations == 0);
	CHECK(busy_calls > 0);
	// The board's busy hook before every chip operation: a page read, a program, a
	// move. A host reading the whole device back to back would otherwise keep the idle task out (the watchdog)
	uint8_t pg[NAND_BLOCKS_PAGE];
	busy_calls = 0;
	CHECK(nand_blocks_read(&nb, 0, pg, sizeof(pg)) && busy_calls == 1);
	busy_calls = 0;
	// One changed sector: the rest of its block moved inside the chip, the copy takes over
	CHECK(replace(3u * bsize() + NAND_BLOCKS_SECTOR, 777));
	CHECK(nand_blocks_sync(&nb));
	nand_blocks_get_stats(&nb, &st);
	CHECK(st.moves == 6 && st.programs == 2 && st.erases == 1);
	CHECK(busy_calls >= st.moves + st.programs);
	CHECK(matches_model());
	CHECK(remount());
	CHECK(matches_model());
	CHECK(nb.stats.seq > 10);
}

// Writes are seen before the commit; a remount before it shows the block as it was
static void test_uncommitted(void)
{
	start(12, 8, 10);
	CHECK(replace(0, 1));
	CHECK(replace(NAND_BLOCKS_SECTOR, 2));
	CHECK(nand_blocks_sync(&nb));
	uint8_t before[MAX_BYTES];
	memcpy(before, model, sizeof(before));
	CHECK(replace(2u * NAND_BLOCKS_SECTOR, 3));
	CHECK(matches_model());				// seen at once
	CHECK(remount());				// as if the power went: the open copy never took over
	memcpy(model, before, sizeof(model));
	CHECK(matches_model());
	CHECK(replace(0, 9));				// the next copy erases the half-made one when it takes it
	CHECK(nand_blocks_sync(&nb));
	CHECK(matches_model());
	CHECK(remount() && matches_model());
	CHECK(nand.violations == 0);
}

// Pages that come out all 0xFF are never programmed; an erase of an erased sector is nothing
static void test_blank_pages(void)
{
	start(12, 8, 10);
	uint8_t ff[NAND_BLOCKS_SECTOR];
	memset(ff, 0xFF, sizeof(ff));
	CHECK(nand_blocks_replace(&nb, 0, ff));
	CHECK(nand_blocks_sync(&nb));
	CHECK(nb.stats.programs == 0);
	CHECK(nb.stats.moves == 1);			// only the commit (the last page, erased)
	CHECK(remount() && matches_model());
	uint32_t ops = nand.ops;
	CHECK(nand_blocks_erase(&nb, 4u * NAND_BLOCKS_SECTOR));
	CHECK(nand.ops == ops);
	CHECK(replace(4u * NAND_BLOCKS_SECTOR, 5));
	CHECK(nand_blocks_erase(&nb, 4u * NAND_BLOCKS_SECTOR));	// written, then erased in the same copy
	memset(&model[4u * NAND_BLOCKS_SECTOR], 0xFF, NAND_BLOCKS_SECTOR);
	CHECK(matches_model());
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
	CHECK(replace(5u * NAND_BLOCKS_SECTOR, 6));
	CHECK(nand_blocks_sync(&nb));
	CHECK(nand_blocks_erase(&nb, 5u * NAND_BLOCKS_SECTOR));	// a committed sector erased
	memset(&model[5u * NAND_BLOCKS_SECTOR], 0xFF, NAND_BLOCKS_SECTOR);
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
	// The last sector of a block erased: the copy commits on its own
	CHECK(replace(bsize() - NAND_BLOCKS_SECTOR, 7));
	CHECK(nand_blocks_erase(&nb, bsize() - NAND_BLOCKS_SECTOR));
	memset(&model[bsize() - NAND_BLOCKS_SECTOR], 0xFF, NAND_BLOCKS_SECTOR);
	CHECK(nb.open_l == NAND_BLOCKS_NONE);
	CHECK(remount() && matches_model());
	CHECK(nand.violations == 0);
}

// NOR writes: any length and alignment, across pages and blocks; setting a bit back to 1 is refused whole
static void test_nor_writes(void)
{
	start(12, 8, 10);
	uint8_t w[5000];
	fill(w, sizeof(w), 3);
	uint32_t at = bsize() - 2500;			// across a block boundary
	CHECK(nand_blocks_write(&nb, at, w, sizeof(w)));
	memcpy(&model[at], w, sizeof(w));
	CHECK(matches_model());
	uint8_t z[3] = { 0, 0, 0 };
	CHECK(nand_blocks_write(&nb, 10, z, 3));	// a few bits more in an erased place
	memset(&model[10], 0, 3);
	uint8_t back[2] = { 0xFF, 0xFF };
	uint32_t ops = nand.ops;
	CHECK(!nand_blocks_write(&nb, at + 100, back, 2));	// would set bits back to 1
	CHECK(nand.ops == ops);
	CHECK(nand_blocks_write(&nb, at, w, 100));	// what is already there: nothing
	CHECK(nand.ops == ops);
	uint8_t and[4];
	memcpy(and, &model[at + 7], 4);
	for (int i = 0; i < 4; i++) {
		and[i] &= 0x0F;
	}
	CHECK(nand_blocks_write(&nb, at + 7, and, 4));	// clearing more bits of written bytes
	memcpy(&model[at + 7], and, 4);
	CHECK(matches_model());
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
	CHECK(nand.violations == 0);
}

// Writes out of order: each goes into a copy; the result is the model whatever the order
static void test_random_ops(void)
{
	start(14, 8, 10);
	for (int i = 0; i < 600; i++) {
		uint32_t sectors = nand_blocks_size(&nb) / NAND_BLOCKS_SECTOR;
		uint32_t a = (rnd() % sectors) * NAND_BLOCKS_SECTOR;
		switch (rnd() % 6) {
		case 0:
			CHECK(nand_blocks_erase(&nb, a));
			memset(&model[a], 0xFF, NAND_BLOCKS_SECTOR);
			break;
		case 1: {
			uint32_t o = a + rnd() % NAND_BLOCKS_SECTOR, n = 1 + rnd() % 3000;
			if (o + n > nand_blocks_size(&nb)) {
				n = nand_blocks_size(&nb) - o;
			}
			uint8_t w[3000];
			for (uint32_t k = 0; k < n; k++) {
				w[k] = model[o + k] & (uint8_t)rnd();
			}
			CHECK(nand_blocks_write(&nb, o, w, n));
			memcpy(&model[o], w, n);
			break;
		}
		case 2:
			CHECK(nand_blocks_sync(&nb));
			break;
		default:
			CHECK(replace(a, rnd()));
			break;
		}
		if (i % 97 == 0) {
			CHECK(nand_blocks_sync(&nb));
			CHECK(remount());
		}
		if (i % 50 == 0) {
			CHECK(matches_model());
		}
	}
	CHECK(matches_model());
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
	CHECK(nand.violations == 0);
	// Wear goes round the region: every block erased, none far more than the others
	uint32_t lo = ~0u, hi = 0;
	for (uint16_t b = 0; b < 14; b++) {
		lo = nand.erases[b] < lo ? nand.erases[b] : lo;
		hi = nand.erases[b] > hi ? nand.erases[b] : hi;
	}
	CHECK(lo > 0);
	CHECK(hi <= lo * 3u + 4u);
}

// Older copies left in the region (blocks are erased when next used): the newest always wins at a mount
static void test_newest_wins(void)
{
	start(16, 8, 4);
	for (uint32_t k = 0; k < 6; k++) {
		CHECK(replace(bsize() + 2u * NAND_BLOCKS_SECTOR, 100 + k));
		CHECK(nand_blocks_sync(&nb));
		CHECK(remount());
		CHECK(matches_model());
	}
}

static void test_bad_blocks(void)
{
	// Factory bad: never touched
	nand_create(12, 8);
	factory_bad(0);
	factory_bad(5);
	memset(model, 0xFF, sizeof(model));
	CHECK(mount(12, 8, 8));
	for (uint32_t a = 0; a < nand_blocks_size(&nb); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a + 11));
	}
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
	nand_blocks_stats_t st;
	nand_blocks_get_stats(&nb, &st);
	CHECK(st.blocks_bad == 2 && st.blocks_used == 8 && st.blocks_free == 2);
	CHECK(nand.violations == 0);

	// A block that fails a program partway: the pages before go to another block, it is retired
	start(12, 8, 8);
	for (uint32_t a = 0; a < bsize(); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a + 1));
	}
	CHECK(nand_blocks_sync(&nb));
	uint16_t next = (uint16_t)((nb.rotor + 1u) % 12u);
	nand.worn_program[next] = true;
	CHECK(replace(0, 50));
	CHECK(replace(NAND_BLOCKS_SECTOR, 51));
	CHECK(nb.open_p != next);
	CHECK(nand_blocks_sync(&nb));
	CHECK(matches_model());
	CHECK(nb.stats.retired == 1);
	CHECK(remount() && matches_model());
	nand_blocks_get_stats(&nb, &st);
	CHECK(st.blocks_bad == 1);

	// A block that fails its erase is retired and the next one used
	next = (uint16_t)((nb.rotor + 1u) % 12u);
	nand.worn_erase[next] = true;
	CHECK(replace(bsize(), 60));
	CHECK(nand_blocks_sync(&nb));
	CHECK(nb.stats.retired == 1);
	CHECK(remount() && matches_model());
	nand_blocks_get_stats(&nb, &st);
	CHECK(st.blocks_bad == 2);

	// Every spare gone: the write is refused, what was written stays
	for (uint16_t b = 0; b < 12; b++) {
		nand.worn_erase[b] = true;
	}
	CHECK(!replace(2u * bsize(), 70));
	CHECK(remount());
	CHECK(matches_model());
	CHECK(nand.violations == 0);
}

// A block that wears partway through a copy: the pages already in it (moved, new, or left erased) go to another
// block, and so on when that one fails too
static void test_worn_partway(void)
{
	start(12, 8, 8);
	uint8_t half[NAND_BLOCKS_SECTOR];
	fill(half, PAGE, 41);
	memset(&half[PAGE], 0xFF, PAGE);		// page 1 stays erased
	CHECK(nand_blocks_replace(&nb, 0, half));
	memcpy(model, half, sizeof(half));
	for (uint32_t a = NAND_BLOCKS_SECTOR; a < bsize(); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a + 40));
	}
	CHECK(nand_blocks_sync(&nb));
	// The next block wears from page 4, the one after it from page 2
	uint16_t b1 = (uint16_t)((nb.rotor + 1u) % 12u), b2 = (uint16_t)((nb.rotor + 2u) % 12u);
	nand.worn_program[b1] = true;
	nand.worn_from[b1] = 4;
	nand.worn_program[b2] = true;
	nand.worn_from[b2] = 2;
	uint8_t w[3] = { 0x00, 0x01, 0x02 };
	for (int i = 0; i < 3; i++) {
		w[i] &= model[5u * PAGE + 9u + i];
		model[5u * PAGE + 9u + i] = w[i];
	}
	CHECK(nand_blocks_write(&nb, 5u * PAGE + 9u, w, 3));	// page 5 changes: pages 0-4 moved first
	CHECK(matches_model());
	CHECK(nb.stats.retired == 2);
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
	nand_blocks_stats_t st;
	nand_blocks_get_stats(&nb, &st);
	CHECK(st.blocks_bad == 2);
	CHECK(nand.violations == 0);
	// A page of the failing copy that can't be read: the write fails, the old copy stays
	start(12, 8, 8);
	for (uint32_t a = 0; a < bsize(); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a + 70));
	}
	CHECK(nand_blocks_sync(&nb));
	uint8_t before[MAX_BYTES];
	memcpy(before, model, sizeof(before));
	CHECK(replace(0, 71));				// pages 0-1 in the open copy
	nand.ecc_fail[nb.open_p * 8u + 1u] = true;
	nand.worn_program[nb.open_p] = true;
	nand.worn_from[nb.open_p] = 2;
	CHECK(!replace(NAND_BLOCKS_SECTOR, 72));
	CHECK(remount());
	memcpy(model, before, sizeof(model));
	CHECK(matches_model());
}

// The commit (the last page, written erased) fails and the copy can't move: the write fails, the open copy is
// dropped, and a sync after it keeps the old copy instead of committing a half-programmed last page
static void test_failed_commit(void)
{
	start(12, 8, 8);
	for (uint32_t a = 0; a < bsize(); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a + 80));
	}
	CHECK(nand_blocks_sync(&nb));
	uint8_t before[MAX_BYTES];
	memcpy(before, model, sizeof(before));
	for (uint32_t a = 0; a + NAND_BLOCKS_SECTOR < bsize(); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a + 81));		// pages 0-5 in the open copy
	}
	nand.ecc_fail[nb.open_p * 8u + 1u] = true;	// so it can't be moved
	nand.worn_program[nb.open_p] = true;
	nand.worn_from[nb.open_p] = 7;
	uint8_t last[NAND_BLOCKS_SECTOR];
	fill(last, PAGE, 82);
	memset(&last[PAGE], 0xFF, PAGE);		// page 7 erased: committed by close_open
	CHECK(!nand_blocks_replace(&nb, bsize() - NAND_BLOCKS_SECTOR, last));
	CHECK(nb.open_l == NAND_BLOCKS_NONE);
	CHECK(nand_blocks_sync(&nb));
	memcpy(model, before, sizeof(model));
	CHECK(matches_model());
	CHECK(remount() && matches_model());
}

// An old page that can't be read is left erased in the copy: the block can still be written
static void test_unreadable_old_page(void)
{
	start(12, 8, 8);
	for (uint32_t a = 0; a < bsize(); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a + 3));
	}
	CHECK(nand_blocks_sync(&nb));
	nand.ecc_fail[nb.map[0] * 8u + 5u] = true;
	CHECK(replace(0, 99));
	CHECK(nand_blocks_sync(&nb));
	CHECK(nb.stats.lost == 1);
	memset(&model[5u * PAGE], 0xFF, PAGE);
	CHECK(matches_model());
	CHECK(remount() && matches_model());
}

// The old copy's last page can't be read when the copy commits: it is left erased, the commit still made
static void test_lost_last_page(void)
{
	start(12, 8, 8);
	for (uint32_t a = 0; a < bsize(); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a + 90));
	}
	CHECK(nand_blocks_sync(&nb));
	nand.ecc_fail[nb.map[0] * 8u + 7u] = true;
	CHECK(replace(0, 91));
	CHECK(nand_blocks_sync(&nb));
	CHECK(nb.stats.lost == 1);
	memset(&model[7u * PAGE], 0xFF, PAGE);
	CHECK(matches_model());
	CHECK(remount() && matches_model());
}

// Foreign data that turns up in a free block after the mount: found before the erase, set aside, never erased
static void test_foreign_late(void)
{
	start(12, 8, 8);
	uint16_t next = (uint16_t)((nb.rotor + 1u) % 12u);
	memset(nand.data[next * 8u], 0x21, PAGE);
	CHECK(replace(0, 5));
	CHECK(nb.open_p != next);
	CHECK(nb.stats.foreign == 1 && nb.stats.foreign_block == next);
	CHECK(nand.erases[next] == 0);
	CHECK(nand_blocks_sync(&nb));
	CHECK(matches_model());
	nand_blocks_stats_t st;
	nand_blocks_get_stats(&nb, &st);
	CHECK(st.blocks_free == 10);
}

// A page that can't be read: reads, NOR writes and erases that need it fail; the copy and the rest go on
static void test_read_errors(void)
{
	start(12, 8, 8);
	for (uint32_t a = 0; a < bsize(); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a + 30));
	}
	CHECK(nand_blocks_sync(&nb));
	CHECK(replace(0, 31));				// an open copy with pages 0-1
	uint32_t bad = nb.map[0] * 8u + 4u;
	nand.ecc_fail[bad] = true;
	CHECK(!nand_blocks_read(&nb, 4u * PAGE + 10u, buf, 4));
	uint8_t z = 0;
	CHECK(!nand_blocks_write(&nb, 4u * PAGE + 10u, &z, 1));
	CHECK(!nand_blocks_erase(&nb, 2u * NAND_BLOCKS_SECTOR));
	CHECK(nb.open_l == 0);				// still open, pages 0-1 kept
	nand.ecc_fail[bad] = false;
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
	// The partial write's own old bytes unreadable after the NOR check: refused, the copy goes on
	CHECK(replace(bsize(), 32));
	CHECK(nand_blocks_sync(&nb));
	CHECK(replace(bsize(), 33));			// open copy of block 1, pages 0-1
	nand.ecc_fail[nb.map[1] * 8u + 3u] = true;
	CHECK(!nand_blocks_write(&nb, bsize() + 3u * PAGE, &z, 1));
	nand.ecc_fail[nb.map[1] * 8u + 3u] = false;
	CHECK(nb.open_l == 1);
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
	// Every block worn: relocation runs out, the write fails, the old copy stays
	for (uint16_t b = 0; b < 12; b++) {
		nand.worn_program[b] = true;
		nand.worn_from[b] = 1;
	}
	uint8_t before[MAX_BYTES];
	memcpy(before, model, sizeof(before));
	CHECK(!replace(0, 34));
	CHECK(remount());
	memcpy(model, before, sizeof(model));
	CHECK(matches_model());
}

// Data the layer didn't write: the mount refuses and touches nothing
static void test_foreign(void)
{
	nand_create(12, 8);
	memset(nand.data[3 * 8], 0x12, PAGE);		// someone's page 0
	CHECK(!mount(12, 8, 8));
	CHECK(nb.stats.status == NAND_BLOCKS_FOREIGN && nb.stats.foreign == 1 && nb.stats.foreign_block == 3);
	CHECK(nand.ops == 0);
	CHECK(!nand_blocks_read(&nb, 0, buf, 16));
	nand_create(12, 8);
	memset(&nand.data[5 * 8][PAGE + 8], 0x00, 16);	// something in the spare area where the tag goes
	CHECK(!mount(12, 8, 8));
	CHECK(nb.stats.foreign_block == 5 && nand.ops == 0);
	nand_create(12, 8);
	memset(nand.data[6 * 8 + 7], 0x00, 32);		// only the last page
	CHECK(!mount(12, 8, 8));
	CHECK(nb.stats.foreign_block == 6 && nand.ops == 0);
	// The factory's test mark alone is blank; a torn page (ECC fails) is ours
	nand_create(12, 8);
	memcpy(nand.data[2 * 8], mark10, sizeof(mark10));
	nand.ecc_fail[4 * 8] = true;
	CHECK(mount(12, 8, 8));
	CHECK(nb.stats.foreign == 0);
	memset(model, 0xFF, sizeof(model));
	for (uint32_t a = 0; a < nand_blocks_size(&nb); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a + 5));
	}
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
}

// --- Power cuts ---

#define MAX_STEPS	40u
static uint8_t snaps[MAX_STEPS + 1][8u * 8u * PAGE];	// the model after each call (8 logical blocks of 8 pages)

typedef struct {
	uint8_t kind;		// 0 replace, 1 erase, 2 NOR write, 3 sync
	uint32_t addr, seed, len;
} step_t;

static bool run_step(const step_t *s)
{
	switch (s->kind) {
	case 0:
		return replace(s->addr, s->seed);
	case 1:
		if (!nand_blocks_erase(&nb, s->addr)) {
			return false;
		}
		memset(&model[s->addr], 0xFF, NAND_BLOCKS_SECTOR);
		return true;
	case 2: {
		uint8_t w[3000];
		for (uint32_t k = 0; k < s->len; k++) {
			w[k] = model[s->addr + k] & (uint8_t)(s->seed + k * 7u);
		}
		if (!nand_blocks_write(&nb, s->addr, w, s->len)) {
			return false;
		}
		memcpy(&model[s->addr], w, s->len);
		return true;
	}
	default:
		return nand_blocks_sync(&nb);
	}
}

static void test_power_cuts(void)
{
	const uint32_t S = NAND_BLOCKS_SECTOR, B = 8u * PAGE;
	const step_t steps[] = {
		{ 0, 0, 1, 0 }, { 0, S, 2, 0 }, { 0, 3 * S, 3, 0 },		// block 0, skipping a sector
		{ 0, B + S, 4, 0 }, { 2, B + 100, 5, 2500 },			// block 1, a write back into it
		{ 1, S, 0, 0 }, { 0, 2 * B, 6, 0 }, { 3, 0, 0, 0 },		// block 0 again, erased sector
		{ 0, 2 * B + 3 * S, 7, 0 }, { 0, 2 * B, 8, 0 },			// out of order in block 2
		{ 2, 3 * B - 1000, 9, 2000 },					// across blocks 2 and 3
		{ 1, 3 * S, 0, 0 }, { 0, 4 * B + 3 * S, 10, 0 },		// a block's last sector: commits
		{ 3, 0, 0, 0 },
	};
	const unsigned n = sizeof(steps) / sizeof(steps[0]);
	// The base: a full database, synced
	start(12, 8, 8);
	for (uint32_t a = 0; a < nand_blocks_size(&nb); a += S) {
		CHECK(replace(a, a / S + 1000));
	}
	CHECK(nand_blocks_sync(&nb));
	uint8_t base_model[8u * 8u * PAGE];
	memcpy(base_model, model, sizeof(base_model));
	saved = nand;
	// The states after each call, without cuts
	memcpy(snaps[0], model, sizeof(snaps[0]));
	uint32_t ops0 = nand.ops;
	for (unsigned i = 0; i < n; i++) {
		CHECK(run_step(&steps[i]));
		memcpy(snaps[i + 1], model, sizeof(snaps[0]));
	}
	uint32_t total = nand.ops - ops0;
	CHECK(total > 20);
	unsigned cuts = 0, fallbacks = 0;
	for (unsigned tear = 0; tear < TEAR_MODES; tear++) {
		for (uint32_t c = 1; c <= total; c++) {
			nand = saved;
			memcpy(model, base_model, sizeof(base_model));
			CHECK(remount());
			nand.cut_at = nand.ops + c;
			nand.tear = (tear_t)tear;
			unsigned done = 0;
			while (done < n && run_step(&steps[done])) {
				done++;
			}
			nand.off = false;
			nand.cut_at = 0;
			cuts++;
			if (!remount()) {
				CHECK(!"mount after a cut");
				continue;
			}
			// Each logical block holds its content after some call up to the one cut (a block the cut call finished
			// before the cut holds its end state), whole, and none older than the last sync that returned
			unsigned floor = 0;
			for (unsigned i = 0; i < done; i++) {
				floor = steps[i].kind == 3 ? i + 1u : floor;
			}
			CHECK(nand_blocks_read(&nb, 0, buf, nand_blocks_size(&nb)));
			for (uint32_t l = 0; l < 8; l++) {
				bool found = false;
				for (unsigned k = floor; k <= done + 1u && k <= n && !found; k++) {
					found = memcmp(&buf[l * B], &snaps[k][l * B], B) == 0;
				}
				if (!found) {
					printf("  tear %u cut %u: block %u matches no state (after %u calls)\n", tear, c, l, done);
					for (unsigned k = 0; k <= done + 1 && k <= n; k++) {
						printf("    state %u pages differing:", k);
						for (uint32_t pg = 0; pg < 8; pg++) {
							if (memcmp(&buf[l * B + pg * PAGE], &snaps[k][l * B + pg * PAGE], PAGE) != 0) {
								printf(" %u", pg);
							}
						}
						printf("\n");
					}
				}
				CHECK(found);
			}
			fallbacks += nb.stats.torn;
			CHECK(nand.violations == 0);
			// And it goes on: everything written again, synced, mounted
			memcpy(model, buf, nand_blocks_size(&nb));
			for (uint32_t a = 0; a < nand_blocks_size(&nb); a += 3u * S) {
				CHECK(replace(a, a + c));
			}
			CHECK(nand_blocks_sync(&nb));
			CHECK(remount() && matches_model());
			CHECK(nand.violations == 0);
		}
	}
	printf("  %u power cuts, %u torn commits fell back\n", cuts, fallbacks);
	CHECK(fallbacks > 0);
}

// Real geometry: 64 pages a block, 16 logical blocks (2 MB)
static void test_big_blocks(void)
{
	start(20, 64, 16);
	for (uint32_t a = 0; a < nand_blocks_size(&nb); a += NAND_BLOCKS_SECTOR) {
		CHECK(replace(a, a ^ 0x5A5A));
	}
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
	for (int i = 0; i < 40; i++) {
		CHECK(replace((rnd() % (nand_blocks_size(&nb) / NAND_BLOCKS_SECTOR)) * NAND_BLOCKS_SECTOR, rnd()));
	}
	CHECK(nand_blocks_sync(&nb));
	CHECK(remount() && matches_model());
	CHECK(nand.violations == 0);
}

// Wear goes round the region across mounts too: a mount goes on from the block taken last
static void test_wear_across_mounts(void)
{
	start(16, 8, 4);
	for (uint32_t k = 0; k < 40; k++) {
		CHECK(replace(bsize() + NAND_BLOCKS_SECTOR, 200 + k));
		CHECK(nand_blocks_sync(&nb));
		CHECK(remount());
	}
	CHECK(matches_model());
	uint32_t lo = ~0u, hi = 0;
	for (uint16_t b = 0; b < 16; b++) {
		lo = nand.erases[b] < lo ? nand.erases[b] : lo;
		hi = nand.erases[b] > hi ? nand.erases[b] : hi;
	}
	CHECK(lo > 0 && hi <= lo + 1u);
}

int main(void)
{
	test_setup();
	test_empty_and_sequential();
	test_uncommitted();
	test_blank_pages();
	test_nor_writes();
	test_random_ops();
	test_newest_wins();
	test_wear_across_mounts();
	test_bad_blocks();
	test_worn_partway();
	test_failed_commit();
	test_unreadable_old_page();
	test_foreign();
	test_lost_last_page();
	test_foreign_late();
	test_read_errors();
	test_power_cuts();
	test_big_blocks();
	if (failures) {
		printf("nandBlocksTest: %d FAILED\n", failures);
		return 1;
	}
	printf("nandBlocksTest: PASSED\n");
	return 0;
}
