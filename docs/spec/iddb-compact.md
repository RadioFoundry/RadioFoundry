<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Compact DMR ID database (format C)

A second layout for the DMR ID database, beside format N (`open-firmware-cps.md` 7.5). It holds the same thing: a
name for each DMR ID. It is smaller, and a database refreshed from RadioID changes few 4 KB blocks, so a write with
the fast codeplug protocol (`cps-fast.md`) moves little.

- Firmware: `core/codeplug/dmr_iddb.c` reads both formats. Switch: `CONFIG_IDDB_COMPACT` (`docs/build-config.md`),
  on with `CONFIG_DMR`. A build with it lists `IDDB_COMPACT` in the debug link's VERSION features (`debuglink.md`).
- Host: `site/static/js/codeplug/iddb.js` writes and reads both.
- Tests: `tests/host/codeplugTest.c` (the radio's lookup), `site/tests/codeplug.test.mjs` (the encoder against the
  radio's lookup through `cpsSim`, the compatibility cases of section 6).

## 1. Why

Measured on a database of 133,148 records (RadioID, one country, 32 characters of text):

| Part | Format N | Format C |
|---|---|---|
| IDs | 3 bytes each (399 KB) | the gap from the record before, 1 byte for almost every record (133 KB) |
| Callsign | 6 bits per character | the same: callsigns are almost all different, so a dictionary doesn't help |
| Name, city and state | 6 bits per character | a code into a dictionary of the strings that repeat: 1 byte for the 128 most frequent, 2 for the rest |
| Total | 3.6 MB | about 1.45 MB with the slack of section 3.4 |

Format N packs records end to end, so one record added near the start moves every record after it: a monthly
RadioID refresh rewrote 874 of 878 blocks in a simulation. Format C keeps each record inside one page and leaves room
in each page, so the same refresh changes 143 pages.

LZ4 (the fast protocol's transfer coding) shrinks format N by only 8 %: the text is already packed. The dictionary,
not the wire, is where the size goes.

## 2. The database space

The database lives in the same two areas as format N (`open-firmware-cps.md` 7.5): area 1 (256 KB) and then area 2,
up to the radio's `openfw_end`. Offsets in this page are **database offsets**: offset `o` is image address
`area1 + o` while `o < 0x40000`, and `area2 + (o - 0x40000)` after that. Page `p` is the 4096 bytes at offset
`p x 4096`. Both areas start on a 4 KB boundary, so a page is a flash block.

| Pages | Content |
|---|---|
| 0 | The header (section 3.1). Nothing else, so it can be erased and written alone |
| from `index` | The index: one entry per record page |
| from `table` | The dictionary's table: one offset per entry |
| from `strings` | The dictionary's strings |
| from the first record page to the end | Record pages, in any order |

The index and the dictionary come before the record pages and keep spare room (section 4.2). New record pages are
added at the end, so nothing moves when the database grows.

## 3. Structures

All numbers are little endian.

### 3.1 Header (page 0)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | ASCII `Id`, as format N |
| 2 | 1 | `C` |
| 3 | 1 | 0. Firmware without this format reads byte 3 as the record size + 0x4A and rejects a header where it is below 0x4A: it sees no database rather than a wrong one |
| 4 | 1 | Version: 1. The radio rejects any other |
| 5 | 1 | The longest text in the database, characters (at most 48). For display |
| 6 | 2 | The first record page: where the strings' room ends. Hosts use it; the radio doesn't |
| 8 | 4 | Records |
| 12 | 4 | Size: the database offset where it ends (the end of the last record page) |
| 16 | 4 | Index offset |
| 20 | 2 | Index entries: the record pages in use |
| 22 | 2 | Dictionary entries, at most 16,512 |
| 24 | 4 | Dictionary table offset |
| 28 | 4 | Dictionary strings offset |

The rest of page 0 is 0xFF. The radio takes the header only when every offset plus its section's length lies inside
`size`, and `size` inside the space the radio has.

### 3.2 Index

`entries` entries of 5 bytes, sorted by first ID, ascending:

| Offset | Size | Field |
|---|---|---|
| 0 | 3 | The first ID in the page |
| 3 | 2 | The page number |

### 3.3 Dictionary

The table holds one 3-byte offset per entry, relative to the strings offset. A string is a length byte (1 to 48
characters), then the characters packed at 6 bits, most significant first, in the alphabet of format N (space,
`0`-`9`, `A`-`Z`, `a`-`z`, `.`). A string may contain spaces.

### 3.4 Record pages

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Records in the page |
| 2 | | The records, back to back. The rest of the page is 0xFF |

A record never crosses a page. Records in a page are sorted by ID, and each page's IDs lie between its index entry's
first ID and the next entry's.

A record is:

1. The ID as a LEB128 number (7 bits per byte, low bits first, bit 7 set when more bytes follow): the first record of
   a page stores its whole ID, every other record the difference from the record before it.
2. Three tokens. The text is the tokens that aren't empty, joined by one space.

| First byte | Token |
|---|---|
| 0x00 - 0x7F | dictionary entry 0 - 127 |
| 0x80 - 0xBF | with the next byte: entry 128 + ((first - 0x80) x 256 + next), up to 16,511 |
| 0xC0 | empty |
| 0xC1 - 0xF0 | literal of 1 to 48 characters (first - 0xC0), then the characters packed at 6 bits as a dictionary string |
| 0xF1 - 0xFF | not used: the radio stops reading the page |

The encoder splits a text at its first two spaces (callsign, first name, the rest), but the radio doesn't care: any
three tokens are a record.

## 4. Writing one

### 4.1 Fresh

1. Records sorted by ID, the first of each ID kept, each text cleaned to the alphabet and cut to 48 characters (as
   format N, `iddb.js` `clean`).
2. The dictionary: each token value that saves bytes as an entry (it appears more than once, and its uses as a code
   plus the string once cost less than as literals). The most frequent first, so the 128 most frequent get 1-byte
   codes. At most 16,512 entries.
3. Records go into pages in ID order, a page closed when the next record would take it past 90 % (3,686 bytes).
4. The index, the table and the strings each get their size plus a quarter, rounded up to whole pages.
5. A database that doesn't fit the radio's space loses records from the top of the ID range, as format N does, and
   the editor says how many.

### 4.2 Against the database it replaces

When the image already holds a format C database (read from the radio or loaded with the file), a new one is written
against it, so pages that didn't change stay byte for byte the same:

- **Dictionary**: every old entry keeps its code. New strings that pass rule 2 are added after them. Entries no
  longer used stay.
- **Pages**: a record goes to the old page whose range holds its ID. A page that overflows splits in two: the first
  half stays, the second goes to a new page at the end. A page left with no records leaves the index.
- **Index, table, strings**: rewritten where they are when they fit their room. Otherwise the database is written
  fresh (4.1), once, and the write moves all of it.

The output is deterministic: the same records against the same old database give the same bytes.

### 4.3 Order on the radio

A database write is not staged (it is larger than any staging area). It never shows a mix:

1. Page 0 erased: from here the radio has no database.
2. Every other page that changed.
3. Page 0 written last.

A write cut short leaves no database, never a wrong one. The next write sends it again. The same order applies to
format N through the fast protocol: the block holding its header goes first with the header erased, and again last.

## 5. The radio's lookup

1. The header, checked as in 3.1 and cached until the codeplug serial changes (as format N).
2. A binary search of the index for the last entry whose first ID is at most the ID: about log2(pages) reads of 5
   bytes.
3. That page, record by record through a 32-byte window: IDs are decoded and tokens skipped until the ID is reached
   or passed.
4. For the record found, each token: a code reads its table entry, then the string.

It needs no buffer beyond the 32-byte window and keeps the header fields in RAM. A lookup of a 4 KB page reads the
part of the page before the record, about 2 KB on average.

## 6. Compatibility

The editor keeps the database as records. The format on the radio is chosen when it writes, from what the radio says
it reads (`IDDB_COMPACT` in VERSION features). So a newer format never reaches a radio that can't read it.

| Case | What happens |
|---|---|
| The radio reads format C, the database is format N (an old file, or read from the radio) | The editor offers to convert it: a smaller database with room for more names and faster writes after it. Converting is a full write of the database, once |
| The radio doesn't read format C, the database is format C | Written as format N. Each text keeps up to the length the longest one needs (16, 24, 32 or 48 characters). If they don't all fit the room, the editor says how many are left out and that a firmware update makes room |
| A format C database on the radio, then older firmware flashed | The older firmware sees no database (3.1, byte 3): no names, nothing wrong. The editor, on reading that radio, marks the database to be written again in format N |
| Firmware without the fast protocol | Format C is still written, through the open-firmware CPS (`cps-protocol.md`), in full |
| TFTP (`tftp.md` 5) | Takes format N only: a format C header is refused before anything is erased. The editor's `codeplug.bin` for TFTP carries the database as format N |
| qdmr and other open-firmware CPS | Write format N. The radio reads it as before |

Format N stays readable by every build: qdmr writes it, and it is the way back.
