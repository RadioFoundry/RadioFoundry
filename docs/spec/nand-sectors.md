<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# NOR-like sectors over NAND

The core's storage (`core/settings`, `core/storage`, [settings-store.md](settings-store.md)) expects a NOR flash:
4 KB erase units, bytes programmed at any length and alignment, several programs into one erased unit (a settings
record, then its commit word). A NAND chip has none of that: 2 KB pages programmed once each and in order, 128 KB
erase blocks, bad blocks. `drivers/flash/nand_sectors` presents a region of NAND blocks as a device of 4 KB sectors
with NOR semantics. The board turns it into its `hal_flash_t` (the DB25-D: [hardware-db25d.md](hardware-db25d.md)
6.6). It is generic: the chip is reached through `nand_dev_t` (read, program with an optional internal data move,
erase, the bad-block mark), and there is no RTOS in it.

## 1. Records

A **record** is the content of one logical sector at one moment. A data record is two pages (half 0 holds sector
bytes 0-2047, half 1 bytes 2048-4095). An erase record is one page and says the sector reads 0xFF. Every write or erase
of a sector programs a new record somewhere else (copy on write). The old one stays as it was until its block is
erased, which happens only once nothing current is left in the block.

Every page carries 16 bytes in its spare area at spare offset 8 (column 2056). Spare bytes 0-7 stay erased: byte 0
is the factory bad-block mark, and the DB25-D stock firmware keeps its own block marks at bytes 4-5.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Sequence number, little-endian. One per record (both halves carry the same), always growing |
| 4 | 2 | Logical sector |
| 6 | 1 | Half: 0 or 1 |
| 7 | 1 | Kind: 0x5D data, 0xE5 erased |
| 8 | 4 | CRC-32 of the page's 2048 data bytes |
| 12 | 4 | CRC-32 of "NSL1" and bytes 0-11. A spare area that doesn't check isn't ours |

CRC-32 is IEEE 802.3, reflected, initial value and final XOR 0xFFFFFFFF.

## 2. The stream

Pages are written as one stream. One block is open at a time and fills in page order; every record takes the next
sequence number. So blocks ordered by their first sequence number, and pages in page order, list the records oldest
first. This is what lets the mount rebuild the map without storing a sequence number per sector in RAM.

A block takes no more pages after any failed program (the page may look blank, and the mount stops reading a block
at its first blank page), and after a mount (the last page programmed before a power cut may look blank while half
programmed, and a NAND page must not be programmed twice). The pages left over are reclaimed with the block. A block
whose first page fails is erased (or retired) at once: nothing else is in it, and left as it is it would look like
foreign data (section 4) to the next mount.

## 3. Mount

| Step | What |
|---|---|
| Bad blocks | The bad-block mark of each block (`block_bad`) |
| Foreign data | Each block is classified (section 4). If any holds foreign data the mount fails with status `NAND_SECTORS_FOREIGN`, the stats name the first such block and the count, and nothing is programmed or erased |
| Order | The first valid spare area of each block gives its first sequence number |
| Scan | Blocks oldest first, pages in order until the first blank one. A data record counts when both halves are there with the same sequence number and sector. A complete record replaces the sector's entry in the map. An erase record replaces it too, but only if an older record of the sector was seen: otherwise it isn't needed and the sector is simply unwritten |
| Verify | Every current data record is read whole and checked against its data CRCs. A page that fails (torn by a power cut after its spare area was programmed, or decayed) is set aside, up to 8, and the scan runs again: the record before it counts |
| Clean up | Blocks that hold records of ours but nothing current are erased. For each torn record found, the content the scan fell back to is written again (or an erase record), so the torn one is never the newest again. These writes collect garbage first like any write: the newest block stays closed after a mount, and without that a nearly full region could be left with no free block to collect into |
| Next | Sequence numbers go on after the highest seen. Writes start in a fresh block |

Blocks with nothing of ours in them are left as they are and erased when they are next used.

## 4. Foreign data

The region may hold data the layer didn't write: a vendor firmware that used those blocks after all, or an older
layout. The layer never erases it. Every block it hasn't proved its own is classified from its first and last pages
(page 1's spare area too) before it is used. The rules apply in this order:

| Block | Rule | Treatment |
|---|---|---|
| Ours | A valid record on page 0 or page 1 (the layer erases a block itself before programming page 0, so the whole block is ours; page 0 may be one of ours that read back wrong) | Mounted |
| Foreign | Page 0 or the last page reads back without an ECC error, isn't erased throughout (data and spare area), and has no valid record | The mount fails; nothing touched |
| Ours | Otherwise, a valid record on the last page | Mounted |
| Torn | Page 0 or the last page is unreadable (ECC fails), the other unreadable or erased: what a power cut leaves of ours (a half-programmed page, a torn erase). Firmware that writes with the chip's ECC on leaves pages that read clean | Erased when it is next used |
| Blank | Both pages erased throughout | Erased when it is next used |

The check runs again just before any block not known to be ours is erased, so a read error at the mount can't lose
a block: one found foreign then is set aside (counted in the stats) and the layer goes on in the other blocks.

What it doesn't see: foreign data only in a block's middle pages, with its first and last pages erased. Reading
every page of every free block at each boot would cost about 2000 page reads on a 32-block region. Clearing a
region that holds foreign data is the owner's decision, made outside the layer.

## 5. Write and erase

| Operation | What happens |
|---|---|
| Read | From the current record's page; 0xFF for an unwritten or erased sector. Only the bytes asked for are read |
| Write | For each sector touched: the new content's CRCs are worked out from the old record and the new bytes, without a page buffer. A write that would set a 0 bit to 1 is refused whole, as NOR can't do it. A write that changes nothing programs nothing. Otherwise two pages: each half is the old page moved inside the chip (`program` with a source page, the NAND's internal data move) with the new bytes and the spare area loaded over it, or programmed from an all-0xFF buffer when there is no old page or the write covers the whole half. Each page is read back (spare area exactly, data by CRC) before the map moves to the new record |
| Replace | The whole sector takes new content of any bits (`nand_sectors_replace`): the same two pages as a write, without the NOR rule, and no erase record before it. One record, so a power cut leaves the old or the new sector. An old page that can't be read doesn't stop it. `hal_flash_t.replace` offers it to `core/storage`, which rewrites a codeplug sector with it instead of its journal (the journal, the erase and the program add up to 11 page programs) |
| Erase | An erase record, unless the sector is unwritten or erased already |

A failed attempt goes again on the next pages, up to 3 times.

## 6. Space, wear and bad blocks

| Topic | Rule |
|---|---|
| Garbage collection | Before a new block is opened, while 2 or fewer blocks are free: the block with the fewest current pages is emptied (its current records moved to the stream under new sequence numbers, data moved inside the chip and read back), then erased |
| Wear | Blocks are opened in turn round the region. Every 16 blocks opened, the block holding the oldest data is emptied the same way, so blocks full of data that never changes take their turn too |
| Factory bad blocks | Never programmed or erased |
| A block the chip reports failing | Program failure: the block takes no more pages, its current records are moved out first, then it is retired. Erase failure: retired at once. Retiring erases it if it can and programs page 0 to 0x00 throughout (the factory bad-block form) |
| A page that reads back wrong | Written again elsewhere; the block is kept (only the chip's own failure status retires a block, so a bug can't retire the region) |

The region must hold the whole logical device plus the reserve: each logical sector can need two pages, and erase
records stay until the records they hide are gone.

## 7. Power cuts

Nothing current is ever overwritten. A new record becomes current at the mount only when both its pages carry a valid
spare area and its data matches the CRCs. A block is erased only when nothing current is in it. So:

| Cut during | After the mount |
|---|---|
| A write or erase record | The new record if it is complete and checks, else the old one. Other sectors don't move |
| Garbage collection's copy | The copy if complete, else the original, still in the block being emptied |
| A block erase | That block held nothing current; whatever is left of it is older than its copies |
| The mount's clean-up | Same as above: it only erases blocks without current records and writes records |

## 8. Cost

| Item | Size |
|---|---|
| RAM, per sector | 2 bytes (the map) |
| RAM, per block | 6 bytes (current pages, flags, first sequence number) |
| RAM, fixed | About 70 bytes. No page buffer: rewrites move pages inside the chip, reads go straight to the caller |
| Mount | One spare-area read per programmed page, plus a 2 KB read for every page of current data, plus two full page reads for each block that isn't ours (the foreign-data check) |
| A write | Two page programs and reads, plus a read of the old pages to work out the CRCs |
| A replace | The same: two page programs (the DB25-D's CPS commit; 542 programs and 8 to 10 block erases for a whole 1.06 MB codeplug) |

## 9. Testing

| Test | What |
|---|---|
| `tests/host/nandSectorsTest.c` | On a simulated NAND (one program per page, in order; programs only clear bits; internal data move; factory bad blocks; worn blocks; a page that reads back wrong): writes, erases, the NOR rules, remounts; thousands of rewrites with garbage collection and wear leveling (every block erased a similar number of times); a full device; foreign data (a block whose first page, last page or whole content someone else wrote, of three kinds, alone or among our blocks: the mount refuses and the chip is unchanged byte for byte; our blocks with a foreign-looking page; a torn first page; foreign data the mount missed found before the erase; a first page the chip fails to program, then a power cut); a power cut at every operation of a sector write, a small write, an erase, and the same with garbage collection, under two random histories, each in three ways (a half-programmed page ECC can't read, a page whose spare area made it but not all its data, a page left blank) and a torn erase. After each cut: every sector old or new, never mixed, others unchanged, a torn page dealt with once, and more writes and a remount still consistent. Then the open-firmware CPS on top (`core/cps`, `core/codeplug`) through `replace`: a whole codeplug written twice (the second time every byte changes, so every sector needs 0 to 1 bits), read back before and after a power cycle, the protected areas refused, the page programs per sector counted against the journaled path, a power cut at every operation of a CPS sector commit (six sectors, three tear kinds, with garbage collection running) with the sector old or new and everything else unchanged each time, and a channel written by the CPS showing up in `core/codeplug` after the session closes. Then `core/storage` and `core/settings` on top: saves loaded after reboots, the ring and garbage collection, a journaled sector rewrite, a cut at every operation of a save |
| `tests/host/spiNandTest.c` | The SPI NAND driver's write side at the bus |
