<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Block-mapped NAND for the ID database

The sector layer ([nand-sectors.md](nand-sectors.md)) keeps 2 bytes of RAM per 4 KB sector: 8 KB for 16 MB, more
than a small radio has free. The ID database doesn't need what it buys (any sector rewritten alone, any order): it
is written mostly in address order, a whole database or the pages a refresh changed. `drivers/flash/nand_blocks`
presents a region of NAND blocks as one linear device with 1 byte of RAM per physical block and 2 per logical block:
under 300 bytes for 16 MB. It uses the sector layer's NAND interface (`nand_dev_t`) and has no RTOS in it.

A board maps it into its `hal_flash_t` after the open-firmware flash image's first 1 MB, so the image, and the ID
database's room in it ([iddb-compact.md](iddb-compact.md) 2), runs on into it with no change to the code that reads or
writes the image ([hardware-d578uv.md](hardware-d578uv.md) 6.3).

## 1. Copies

A logical block is one NAND block of data (64 pages of 2 KB: 128 KB). Each has at most one **current copy**, a
physical block; a logical block never written has none and reads 0xFF without touching the chip.

A write never changes a copy. It goes into the **open copy**: a fresh block, erased just before, filled in page
order. Pages the writes pass over are moved from the current copy inside the chip (the chip's internal data move:
the page never crosses the bus), and pages the current copy never programmed stay erased. A page whose new content is
all 0xFF isn't programmed either. The open copy becomes current (**commits**) when its last page is programmed:

- when a write reaches the last page,
- when a write goes to another logical block, or to a page below the open copy's next one (the open copy commits,
  then a new one opens),
- at `nand_blocks_sync`: the board's `hal_flash_t.sync`, which `codeplug_reload` calls after every codeplug or ID
  database write session (`storage_sync`), and which the board also calls before any write to its other flash
  layer, so a header or a staging record written there never gets ahead of the data.

The last page is always programmed, last, with the commit's tag. Reads see the open copy's pages as soon as they are
written. Until the commit a power cut leaves the logical block as it was before the copy began; after it, as the
copy holds it. Never a mix of the two, and no other block is affected. A host writing a database sends each block's
pages in address order, so each block is copied once.

The old copy isn't erased at the commit: blocks are erased when they are next taken, from the next free block after
the last one taken (wear goes round the region). One spare block is the least the layer needs; more ride out bad
blocks.

## 2. The tag

Every page programmed carries 16 bytes in its spare area at spare offset 8 (column 2056), as the sector layer's
pages do: spare bytes 0-7 stay erased (byte 0 is the factory bad-block mark).

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Sequence number, little-endian: one per copy, every page of it the same, always growing |
| 4 | 2 | Logical block |
| 6 | 1 | Page in the block |
| 7 | 1 | Kind: 0xB1 |
| 8 | 4 | CRC-32 of the page's 2048 data bytes |
| 12 | 4 | CRC-32 of "NBL1" and bytes 0-11. A spare area that doesn't check isn't ours |

CRC-32 is IEEE 802.3, reflected, initial value and final XOR 0xFFFFFFFF. A page moved from the old copy carries a
new tag with the old page's data CRC.

## 3. Mount

The mount programs and erases nothing. For each physical block:

| Step | What |
|---|---|
| Bad | The bad-block mark (`block_bad`): never touched again |
| Complete | The last page's tag checks and the page's data matches its data CRC: a complete copy of its logical block. Of the complete copies of a logical block, the one with the highest sequence number is current. A last page whose tag checks but whose data doesn't was cut while it was programmed: counted (`torn`), and the copy before it stays current |
| Ours | A tag on page 0 or the last page: the block is ours, erased when it's next taken |
| Foreign | Otherwise, page 0 or the last page readable, programmed and without a tag: the mount fails (`NAND_BLOCKS_FOREIGN`, the first such block in the stats) and the board offers no ID database range. The rules are the sector layer's (nand-sectors.md 4), and the check runs again just before a block not known to be ours is erased |

Sequence numbers go on after the highest seen.

## 4. Failures

| What | Treatment |
|---|---|
| An erase fails (the chip says so) | The block is retired (marked bad) and the next free block taken |
| A program fails | The page may be half programmed: never programmed again. A new block takes the copy's pages so far (moved inside the chip, their tags as they are), the failing block is retired, and the page is tried there. Up to 3 times |
| A page of the old copy can't be read when it is moved | It is lost either way: it is left erased in the copy, so the block can still be written (counted, `lost`). The host's checksums show the difference and it sends the page again |
| No free block left | The write fails; the copy current before it stays |

## 5. RAM and costs

| | |
|---|---|
| RAM | 2 bytes per logical block, 1 per physical block, about 24 bytes of state: 416 bytes for 128 logical blocks (16 MB) in 160 physical |
| Mount | 2 spare-area reads per block, and one page read per complete copy |
| A block written whole, in order | One erase and its programmed pages |
| One 4 KB sector changed | One erase, 2 programs, up to 62 internal moves |

## 6. Diagnostics

`nand_blocks_get_stats`: blocks used, free and bad, retired this session, torn commits and lost pages found, commits,
programs, moves, erases, errors. The AT-D578UV shows used, free, bad and lost on its diagnostics page (the `iddb`
line), or `off` with the mount's status and the first foreign block.

## 7. Tests

`tests/host/nandBlocksTest.c`, on the simulated NAND of the sector layer's tests: whole devices written in order,
random writes, erases and NOR writes against a model, older copies left in the region, blank pages never programmed,
factory bad blocks, blocks that wear partway through a copy (and the block taking over wearing too), every spare gone,
unreadable pages, foreign data at the mount and found later, and a power cut at every operation of a script for each
kind of torn page: after each, every logical block holds what it held after some call, whole, and none older than the
last sync that returned.
