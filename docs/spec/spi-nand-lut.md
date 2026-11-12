<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
# The stock block map over an SPI NAND

Some radios' stock firmware addresses its SPI NAND partitions (the factory calibration page among them) by logical
address and keeps a map of the usable blocks in physical block 0. RadioFoundry reads a stock partition through the map
(`drivers/flash/spi_nand/spi_nand_lut.c`) and never writes the map or the partitions. A board that uses it says so on
its hardware page.

## 1. The table

| Offset in physical block 0 | Bytes | Content |
|---|---|---|
| 0 | 4 | `LUT0` |
| 4 | blocks / 8 | One bit per block: bit n of byte 4 + n / 8 is 1 when block n is usable |

Block 0's own bit is clear: it holds the table. The stock firmware writes the table on its first boot from a scan of
the factory bad-block marks and keeps it after that.

## 2. Logical to physical

1. Take out block 0 and every block whose bit is clear.
2. Take out the pool: the 10 highest usable blocks, counted from the top down to block 2. The stock firmware keeps them
   as spare blocks. Every block from the pool's lowest up is out of the logical space.
3. Logical block L is then the physical block reached by `p = L; for each block e taken out, ascending: if p >= e:
   p = p + 1`.

Block 0 alone already moves every partition up one block: logical 0x280000 (block 20) is physical 0x2A0000 (block
21) on a chip with no bad block below it. A partition read at its logical address, unmapped, reads the wrong block.

## 3. Readers

| Case | What a reader does |
|---|---|
| No `LUT0` at physical 0 | The radio has no table (the stock firmware never ran on this chip): don't guess, use another copy |
| A block maps into the pool or past it | Not a stock partition: refuse |
| The mapped page fails its own check (a magic) | Use another copy |
