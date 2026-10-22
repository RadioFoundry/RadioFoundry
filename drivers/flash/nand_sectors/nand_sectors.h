// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// NOR-like 4 KB sectors over a region of NAND blocks (docs/spec/nand-sectors.md), for the core's storage
// (hal_flash.h): read anywhere, program erased bytes at any length and alignment as often as the core likes, erase a
// sector. Each logical sector lives in two NAND pages, rewritten elsewhere on every change (copy on write), found
// again at boot from what each page's spare area says. No RTOS here: the board serializes the calls.
#ifndef NAND_SECTORS_H
#define NAND_SECTORS_H

#include <stdint.h>
#include <stdbool.h>

#define NAND_SECTORS_SECTOR	4096u
#define NAND_SECTORS_PAGE	2048u		// the NAND page this layer works with: a sector is two of them
#define NAND_SECTORS_NO_PAGE	0xFFFFFFFFu
#define NAND_SECTORS_EXCLUDE	8u		// torn pages a mount can set aside
#define NAND_SECTORS_RESERVE	2u		// erased blocks kept back for garbage collection

// What a program or erase came to
typedef enum {
	NAND_OK,
	NAND_FAILED,		// the chip reported the program or erase failed: the block is worn and is retired
	NAND_ERROR,		// anything else (bus, timeout, refused): retried elsewhere, the block is kept
} nand_result_t;

// Bytes for the chip's page buffer at a column (main area from 0, spare area from NAND_SECTORS_PAGE)
typedef struct {
	uint16_t column;
	uint16_t len;
	const void *data;
} nand_chunk_t;

// The NAND region as the layer sees it: blocks and pages counted from the region's first block. The board maps them
// to the chip and keeps every operation inside the region
typedef struct {
	void *ctx;
	uint16_t pages_per_block;	// at most 255
	uint16_t blocks;
	uint16_t spare_size;		// spare bytes per page readable and programmable with the chip's ECC on (>= 24)
	// len bytes of a page from a column; false when the page's ECC fails or the bus does
	bool (*read)(void *ctx, uint32_t page, uint16_t column, void *buf, uint32_t len);
	// Program a page once. src NAND_SECTORS_NO_PAGE: from an all-0xFF buffer; otherwise page src is copied into the
	// buffer inside the chip first (an internal data move). Then the chunks are applied
	nand_result_t (*program)(void *ctx, uint32_t page, uint32_t src, const nand_chunk_t *chunks, unsigned n);
	nand_result_t (*erase)(void *ctx, uint16_t block);
	bool (*block_bad)(void *ctx, uint16_t block, bool *bad);	// the factory (or a retired block's) mark
	void (*mark_bad)(void *ctx, uint16_t block);			// retire a block for good
	// Optional (NULL: none): called once per block or sector in the long loops (the mount, garbage collection), so
	// the board can feed its watchdog while the layer keeps the CPU (docs/spec/watchdog.md 4)
	void (*busy)(void *ctx);
} nand_dev_t;

// What the last mount came to
typedef enum {
	NAND_SECTORS_UNMOUNTED,		// not mounted yet
	NAND_SECTORS_MOUNTED,
	NAND_SECTORS_BAD_SETUP,		// the geometry or the board's arrays are unusable
	NAND_SECTORS_FOREIGN,		// a block in the region holds data the layer didn't write: nothing touched
} nand_sectors_status_t;

typedef struct {
	uint8_t status;			// nand_sectors_status_t
	uint16_t foreign;		// blocks found holding foreign data
	uint16_t foreign_block;		// the first of them (region-relative)
	uint16_t blocks_bad;		// marked bad (factory or retired) or retired this session
	uint16_t blocks_free;		// holding no live data
	uint16_t blocks_used;		// holding live data
	uint16_t retired;		// blocks retired this session (the chip reported a program or erase failure)
	uint16_t torn;			// pages the last mount found torn and set aside
	uint16_t sectors_used;		// logical sectors holding data
	uint32_t seq;			// the next sequence number
	uint32_t mount_reads;		// spare-area reads the last mount took
	uint32_t writes;		// records written (sector rewrites and erase records)
	uint32_t erases;		// block erases
	uint32_t gc_blocks;		// blocks emptied by garbage collection
	uint32_t gc_copies;		// records it moved
	uint32_t leveled;		// blocks of old data moved for static wear leveling
	uint32_t verify_fails;		// pages that didn't read back as programmed
	uint32_t errors;		// operations that failed (retried or not)
} nand_sectors_stats_t;

// One layer instance. The board fills in dev, sectors and the arrays (RAM it owns; the sizes are sectors and
// dev->blocks entries); the rest is the layer's
typedef struct {
	const nand_dev_t *dev;
	uint16_t sectors;		// logical sectors: the device is sectors x 4 KB
	uint16_t *map;			// per sector: where its current record is
	uint8_t *live;			// per block: pages holding current records
	uint8_t *flags;			// per block
	uint32_t *order;		// per block: the first sequence number in it (used by the mount)
	// The layer's state
	bool mounted;
	uint16_t open;			// the block being filled, 0xFFFF for none
	uint16_t next;			// its next free page
	uint16_t rotor;			// the block allocated last (wear goes round the region)
	uint16_t opened, leveled;	// blocks opened, and the count at the last static leveling
	uint32_t seq;
	uint8_t excluded_count;
	uint16_t excluded[NAND_SECTORS_EXCLUDE];
	nand_sectors_stats_t stats;
} nand_sectors_t;

// Scan the region and rebuild the map: every block's spare areas, then the data of each sector's current record
// (a record torn by a power cut is set aside and the one before it used). Erases blocks that hold only superseded
// records. False if the geometry is unusable, or if any block holds data the layer didn't write (its first or last
// page programmed, readable and without the layer's spare area): then nothing at all is programmed or erased, and
// the stats say which block
bool nand_sectors_mount(nand_sectors_t *ns);
uint32_t nand_sectors_size(const nand_sectors_t *ns);
bool nand_sectors_read(nand_sectors_t *ns, uint32_t addr, void *buf, uint32_t len);
// NOR semantics: only erased bits may be programmed (a byte may go from 1 to 0 bits, never back). A write that would
// set a 0 bit to 1 is refused whole (false, nothing written). Writing what is already there is a no-op. Each sector
// the range touches is rewritten as a whole: a power cut leaves it old or new, never a mix, and no other sector moves
bool nand_sectors_write(nand_sectors_t *ns, uint32_t addr, const void *buf, uint32_t len);
// Replace the whole sector at addr (aligned) with 4 KB of any content, in one record: a power cut leaves the old or the
// new sector, never a mix, and no erase record is needed first. Two page programs (a write plus an erase would be
// three, and not atomic). Writing what is there is a no-op
bool nand_sectors_replace(nand_sectors_t *ns, uint32_t addr, const void *buf);
// Erase the 4 KB sector at addr (aligned): it reads 0xFF afterwards
bool nand_sectors_erase(nand_sectors_t *ns, uint32_t addr);
void nand_sectors_get_stats(const nand_sectors_t *ns, nand_sectors_stats_t *out);

#endif
