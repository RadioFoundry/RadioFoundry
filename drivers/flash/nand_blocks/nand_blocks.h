// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A large linear device over a region of NAND blocks, for data written mostly in address order (the ID database):
// docs/spec/nand-blocks.md. Each logical block (one NAND block of data) lives in one physical block; a change is copied
// on write into a fresh block page by page as the writes come, and the new copy takes over when its last page is
// programmed. RAM is 2 bytes per logical block and 1 per physical block, so 16 MB costs under 300 bytes where the
// sector layer (nand_sectors) would need 8 KB. The same NAND interface as the sector layer (nand_dev_t); no RTOS here:
// the board serializes the calls.
#ifndef NAND_BLOCKS_H
#define NAND_BLOCKS_H

#include <stdint.h>
#include <stdbool.h>
#include "flash/nand_sectors/nand_sectors.h"

#define NAND_BLOCKS_PAGE	2048u
#define NAND_BLOCKS_SECTOR	4096u		// the erase unit offered to the core: two pages
#define NAND_BLOCKS_NONE	0xFFFFu

// What the last mount came to
typedef enum {
	NAND_BLOCKS_UNMOUNTED,
	NAND_BLOCKS_MOUNTED,
	NAND_BLOCKS_BAD_SETUP,		// the geometry or the board's arrays are unusable
	NAND_BLOCKS_FOREIGN,		// a block in the region holds data the layer didn't write: nothing touched
} nand_blocks_status_t;

typedef struct {
	uint8_t status;			// nand_blocks_status_t
	uint16_t foreign;		// blocks found holding foreign data
	uint16_t foreign_block;		// the first of them (region-relative)
	uint16_t blocks_bad;		// marked bad (factory or retired)
	uint16_t blocks_used;		// holding a logical block's current copy
	uint16_t blocks_free;		// the rest: the spares the copies rotate through
	uint16_t retired;		// retired this session (the chip reported a program or erase failure)
	uint16_t torn;			// copies the last mount found with a last page that didn't check
	uint16_t lost;			// pages of an old copy that couldn't be read when copied (left erased)
	uint32_t seq;			// the next sequence number
	uint32_t commits;		// copies that took over
	uint32_t programs;		// pages programmed with new data
	uint32_t moves;			// pages moved from the old copy inside the chip
	uint32_t erases;
	uint32_t errors;		// operations that failed (retried or not)
} nand_blocks_stats_t;

// One instance. The board fills in dev, blocks and the two arrays (RAM it owns: map has blocks entries, flags
// dev->blocks); the rest is the layer's. The device is blocks x pages_per_block x 2 KB
typedef struct {
	const nand_dev_t *dev;
	uint16_t blocks;		// logical blocks (fewer than dev->blocks: the rest are spares)
	uint16_t *map;			// per logical block: the physical block of its current copy, NONE if never written
	uint8_t *flags;			// per physical block
	// The layer's state
	bool mounted;
	uint16_t open_l;		// the logical block being copied, NONE for none
	uint16_t open_p;		// the physical block it's copied into
	uint16_t next;			// pages below this in the open copy are current; the rest are still the old copy's
	uint16_t rotor;			// the block allocated last (wear goes round the region)
	uint32_t open_seq;
	nand_blocks_stats_t stats;
} nand_blocks_t;

// Scan the region: each block's bad mark and its last page's tag (and data CRC); the newest complete copy of each
// logical block counts. Programs and erases nothing. False if the geometry is unusable, or if any block holds data
// the layer didn't write (first or last page programmed, readable, without the layer's tag): the stats say which
bool nand_blocks_mount(nand_blocks_t *nb);
uint32_t nand_blocks_size(const nand_blocks_t *nb);
// Reads see every write so far, committed or not
bool nand_blocks_read(nand_blocks_t *nb, uint32_t addr, void *buf, uint32_t len);
// NOR semantics: only erased bits may be programmed. A write that would set a 0 bit to 1 is refused whole
bool nand_blocks_write(nand_blocks_t *nb, uint32_t addr, const void *buf, uint32_t len);
// The 4 KB sector at addr (aligned) replaced by any content
bool nand_blocks_replace(nand_blocks_t *nb, uint32_t addr, const void *buf);
// The 4 KB sector at addr (aligned) reads 0xFF afterwards
bool nand_blocks_erase(nand_blocks_t *nb, uint32_t addr);
// Finish the open copy: the rest of its pages moved from the old one and the commit programmed. Until then a power
// cut leaves the block as it was before its copy began (never a mix)
bool nand_blocks_sync(nand_blocks_t *nb);
void nand_blocks_get_stats(const nand_blocks_t *nb, nand_blocks_stats_t *out);

#endif
