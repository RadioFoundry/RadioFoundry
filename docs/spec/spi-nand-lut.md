<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
# The factory block map over an SPI NAND

On some radios the SPI NAND's factory data (the calibration page among it) sits at logical addresses, with a map of
the usable blocks in physical block 0. RadioFoundry reads factory data through the map
(`drivers/flash/spi_nand/spi_nand_lut.c`) and never writes the map or the factory data. A board that uses it says so on
its hardware page.

## 1. The table

| Offset in physical block 0 | Bytes | Content |
|---|---|---|
| 0 | 4 | `LUT0` |
| 4 | blocks / 8 | One bit per block: bit n of byte 4 + n / 8 is 1 when block n is usable |

Block 0's own bit is clear: it holds the table. The table matches the chip's factory bad-block marks.

## 2. Logical to physical

1. Take out block 0 and every block whose bit is clear.
2. Take out the pool: the 10 highest usable blocks, counted from the top down to block 2, kept as spare blocks.
   Every block from the pool's lowest up is out of the logical space.
3. Logical block L is then the physical block reached by `p = L; for each block e taken out, ascending: if p >= e:
   p = p + 1`.

Block 0 alone already moves all factory data up one block: logical 0x280000 (block 20) is physical 0x2A0000 (block
21) on a chip with no bad block below it. Data read at its logical address, unmapped, comes from the wrong block.

## 3. Readers

| Case | What a reader does |
|---|---|
| No `LUT0` at physical 0 | The chip has no table: don't guess, use another copy |
| A block maps into the pool or past it | Not factory data: refuse |
| The mapped page fails its own check (a magic) | Use another copy |
