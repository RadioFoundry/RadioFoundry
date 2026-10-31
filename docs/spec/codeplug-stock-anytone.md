<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Stock AnyTone codeplugs

The vendor firmware of the AnyTone radios with SPI NAND keeps its codeplug in tables of its own across the NAND.
RadioFoundry keeps its own codeplug in its own NAND blocks and can't read these tables once it runs. The web flasher
therefore reads them before the install, through the stock firmware's PC mode, and writes the converted codeplug
through RadioFoundry's CPS afterwards ([web-flasher.md](../dev/web-flasher.md), "Channels after an install"). Which
radios have this layout: [feature-matrix.md](../feature-matrix.md) and each radio's hardware page.

Everything here is from a static analysis of two stock firmware releases, which agree on every address and marker,
and untested on a radio. (I) marks an inferred value.

Code: `site/static/js/codeplug/stock_anytone.js` (the tables), `site/static/js/flasher/anytone.js` (`AnytoneProgram`,
the PC mode), `site/static/js/flasher/migrate.js` (`readAnytone`, `convertAnytone`), `tools/rfoundry/rfoundry/anytone.py`
(`Program`); tests in `site/tests/stock_anytone.test.mjs`, `site/tests/anytone.test.mjs` and
`tools/rfoundry/tests/test_anytone.py`.

## 1. PC mode

The stock firmware answers on its USB serial port while it runs normally (not in the update mode).

| Frame | Answer |
|---|---|
| `PROGRAM` | `51 58 06` ("QX", ACK): the PC mode, shown on the screen |
| `R`, address (4 bytes, big-endian), count (1 byte) | `W`, address, count, the data, the 8-bit sum of address, count and data, `06` |
| `END` | `51 58 06`, then the radio restarts |

Reads ask for 16 bytes at most, as the vendor's programming software does. A malformed answer, one for another
address or a bad sum is asked again, three times in all. Addresses are NAND byte addresses. The firmware's read goes
through its bad block remap and picks the current block of a pair itself (section 2).

## 2. Block pairs

Each table sits in a pair of 128 KB blocks at X and X + 0x20000. The last 16 bytes of a block carry its markers:

| Offset in the block | Bytes | Meaning |
|---|---|---|
| 0x1FFF4 | `22 33 44 55` | a copy under way |
| 0x1FFF8 | `AA AA 55 55` | obsolete |
| 0x1FFFC | `55 55 AA AA` | valid |

The first block is current when it is valid and not obsolete; otherwise the second is. An erased pair reads as its
second block. The PC mode's `R` applies the rule: a read in a block that isn't current returns the bytes 0x20000
further on. The reader therefore asks for addresses in the pair's first block and never reads the markers. A read
must stay inside one 128 KB block, and an address in the second block of a pair returns the wrong data unless that
block is current itself.

## 3. Tables

| Table | Pair (X) | Record | Count |
|---|---|---|---|
| Channels | 0x00800000 + (n / 128) x 0x40000 | 128 bytes in two halves: bytes 0-63 at n % 128 x 64, bytes 64-127 at 0x2000 + n % 128 x 64 | 4000 |
| Zone names | 0x02540000 | 32 bytes, the name at 0 | 250 |
| Zone members | 0x01000000 | 0x200 bytes: 250 channel numbers (u16 LE), 0xFFFF empty, gaps allowed | 250 |
| Radio IDs | 0x02580000 | 32 bytes: the ID in 8 BCD digits (big-endian) at 0, the name at 5 | 250 |
| RX group lists | 0x02980000 | 0x200 bytes: 64 contact numbers (u32 LE), the first at or above 0x4E20 ends it; the name at 0x100 | 250 |
| Contacts | 0x02680000 + (n / 1000) x 0x40000 | 100 bytes at n % 1000 x 100 | 10000 |
| Scan lists | 0x01080000 + (n / 16) x 0x40000 | 0xB6 bytes at n % 16 x 0x200: the name at 0x0F, 50 channel numbers at 0x20 | 250 |

Names are 17 bytes, NUL ended, in GBK. The reader keeps ASCII names; one with other bytes (Chinese) reads as no name
and gets a default. The reader takes the zones first and reads only the channels they name.

### 3.3 Bitmaps

Which records exist, one bit each, least significant bit first:

| Bitmap | Address | Bytes | A set bit |
|---|---|---|---|
| Zones | 0x024C1300 | 32 (250 bits) | the zone exists |
| Radio IDs | 0x024C1320 | 32 | the ID exists |
| Scan lists | 0x024C1340 | 32 | the list exists |
| Hidden zones | 0x024C1360 | 32 | the zone is hidden |
| Channels | 0x024C1500 | 500 | the channel exists |
| RX group lists | 0x025C0B10 | 32 | the list exists |
| Contacts | 0x02640000 | 1250 | the contact is **free** (clear = in use) |

The radio drops a channel's reference to a list or contact that doesn't exist; the reader does the same. An erased
radio has every bit set, so a zone also needs a name.

### 3.1 Channel (the first half)

| Offset | Field |
|---|---|
| 0-3 | RX frequency, 8 BCD digits, most significant first, 10 Hz units |
| 4-7 | TX offset, the same |
| 8 | bits 1-0 type (0 analog, 1 digital, 2 analog and digital, transmitting analog, 3 digital and analog, transmitting digital); 3-2 power (0 low, 1 mid, 2 high, 3 turbo); 5-4 bandwidth, 0 narrow, nonzero wide (12.5 / 25 kHz (I)), analog only; 7-6 offset direction (0 none, 1 up, 2 down) |
| 9 | bits 1-0 RX tone type, 3-2 TX tone type (0 none, 1 CTCSS, 2 DCS); 4 reverse; 5 TX prohibited; 7 talkaround (TX on the RX frequency) |
| 0x0A / 0x0B | TX / RX CTCSS index into the 51 standard tones (62.5-254.1 Hz); 0x33 is the channel's own tone |
| 0x0C / 0x0E | TX / RX DCS, u16 LE: the low 9 bits, 3 per octal digit (D023 = 0x013); bit 9 inverted |
| 0x10 | the channel's own CTCSS tone, u16 LE, 0.1 Hz |
| 0x14 | TX contact number, u32 LE |
| 0x18 | radio ID number |
| 0x1B | scan list number, 0xFF none (I) |
| 0x1C | RX group list number, 0xFF none |
| 0x20 | color code |
| 0x21 | bit 0: time slot 2 (the order TS1 = 0 from the menu (I)) |
| 0x23 | name |

### 3.2 Contact

| Offset | Field |
|---|---|
| 0 | call type: 0 private, 1 group, 2 all call |
| 1 | name |
| 0x13, 0x28, 0x31, 0x42, 0x53 | further text fields (16, 8, 16, 16, 16 bytes) |
| 0x27 | a flag, not known |
| 0x23-0x26 | DMR ID, 8 BCD digits, most significant first |

## 4. Conversion

`importAnytone` fills the editing model as the TYT conversion does ([codeplug-stock-tyt.md](codeplug-stock-tyt.md) 4),
with the same limits and notes:

| Stock | RadioFoundry |
|---|---|
| A channel in a zone | a channel; one named in two zones is one channel in both |
| Type 2 / 3 | FM / DMR, the mode it transmits in (note `mixed_mode`) |
| Power low, mid, high, turbo | Low, Mid, High, High |
| TX prohibited | RX only |
| Talkaround, offset direction | the TX frequency |
| The radio ID most channels use | the radio's DMR ID and name |
| Scan lists | dropped (note `scan_lists`) |
| A channel in no zone | not read (note `zone_only`) |

## 5. Open

- Every address, marker and bitmap polarity is from static analysis; a dump of a real radio would confirm them.
- Which radio ID is the default; the reader takes the one most channels use.
- Bandwidth values 2 and 3; the squelch level and the channel's remaining bytes.
- Names in GBK: converting Chinese names to the radio's character set.
