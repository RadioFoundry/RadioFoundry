<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DM-32UV stock codeplug

The Baofeng DM-32UV vendor firmware keeps its codeplug in the 16 MB data flash, in a paged layout of its own.
RadioFoundry never writes there: its codeplug is at 0x700000 ([dm32-migration.md](dm32-migration.md) 2). A data flash
read of a radio that ran the stock firmware therefore still holds the stock codeplug, also after RadioFoundry is
installed. This page covers how the stock records are found (section 1), the records the converter reads (section 2),
detection (section 3) and the conversion (section 4).

Method (clean room, [CLEANROOM.md](../CLEANROOM.md)): data only. Data flash reads from two radios were compared with
`dmrconf decode` output of the same codeplugs (dmrconf run as an external program), and with reads taken before and
after a single change in the stock radio's menu. No vendor or qdmr source was read. (R) marks a field proven by such
an on-radio change; (D) one that matches dmrconf's decode on both radios; (O) one seen in the data only.

Code: `site/static/js/codeplug/stock_baofeng.js`, tests in `site/tests/stock_baofeng.test.mjs`.

## 1. Pages and logical blocks

| Item | Value |
|---|---|
| Area | 0x001000-0x0C8FFF, 200 pages of 4 KB ([hardware-dm32.md](hardware-dm32.md) 10.1) |
| Block number | The last byte of a page (+0xFFF): the logical block the page holds. 0x00 is a page given up, 0xFF an erased one |
| Logical image | Block n at n × 0x1000. A block's data is its first 0xFFF bytes |
| Writes | A change copies the block to a free page and sets the old page's block number to 0x00 (R). So one page per block number is in use |
| Calibration | Block 2: the factory calibration. Never written |

Booting the stock firmware changes no byte of the area (R).

## 2. Records

Offsets are into the logical block. Numbers are little-endian. Text is ASCII, ending at the first 0x00 or 0xFF; the
stock software leaves old text after the end, so bytes past it carry no meaning.

### 2.1 Blocks

| Block | Contents |
|---|---|
| 0x04 | Settings. +0x01 boot line 1 (14 bytes), +0x0F boot line 2 (14 bytes) (D). Passwords at +0x439 ([dm32-stock-cps.md](dm32-stock-cps.md) 2) |
| 0x0F | RX group lists (2.4) |
| 0x11 | Scan lists (not read) |
| 0x12 | Channels 1-84 (2.2) |
| 0x13-0x41 | Present on both radios, erased or zero, no channel past 84 seen (section 5) |
| 0x42 | TX contact per channel (2.2) |
| 0x44 | Contacts (2.3) |
| 0x5C | Zones (2.5) |
| 0x65, 0x66 | Roaming zones and channels (not read) |
| 0x67 | Radio IDs (2.6) |

### 2.2 Channel

Block 0x12: +0x00 the channel count (u16), records of 48 bytes from +0x10, channel n at +0x10 + 48 × (n − 1).

| Offset | Field |
|---|---|
| 0x00 | Name, 16 bytes (D) |
| 0x10, 0x14 | RX, TX frequency: 8 BCD digits of 10 Hz, as a little-endian word (D) |
| 0x18 | Bit 4: digital (D). Bits 1-2: power, 0 low, 1 mid, 2 high (D) |
| 0x19 | Bit 7: wide (25 kHz) FM (D). Bits 2-6: scan list number, 0 none (D, lists 1 and 3 seen). Bits 0-1 unknown |
| 0x1D | Bit 4: time slot 2 (D). Bits 0-3: color code (D). Bit 5 unknown |
| 0x1F | RX group list number, 1-based, 0 none (D) |
| 0x21, 0x23 | RX, TX tone, u16. 0xFFFF or 0 none. CTCSS: 4 BCD digits of 0.1 Hz (0x1035 = 103.5 Hz) (D). DCS: bit 15 set, the code's three digits read as a decimal number in bits 0-11 (0x8114 = D276) (D on three codes, section 5) |
| 0x1A, 0x1C, 0x20 | Unknown. Squelch, TX timeout and admit criterion are not mapped |

TX contact (block 0x42): two bytes per channel, channel n at 2 × (n − 1). Byte 1 is the contact number, 1-based, 0
none (R: one channel changed from TG 99 to 311 to 311340, and only this byte changed, 06 to 05 to 03). Byte 0 is 0x01
on every digital channel seen; on analog channels it varies. dmrconf 0.15.1 decodes no TX contact for this radio.

### 2.3 Contact

Block 0x44, 24-byte records from +0, contact n at 24 × (n − 1). A record with an empty name is unused; numbers past it
still count.

| Offset | Field |
|---|---|
| 0x00 | u16, 0 or 1 (O). Not read |
| 0x02 | Name, 16 bytes (D) |
| 0x13 | DMR ID, u24 (D) |
| 0x16 | Type: 3 private call, 4 group call (D) |

### 2.4 RX group list

Block 0x0F: +0x00 a byte (0x1F on both radios: five lists, as a bit map) (O), records of 109 bytes from +0x10.

| Offset | Field |
|---|---|
| 0x00 | 1 for list 1, 0 for the others (O). Not read |
| 0x01 | Name, 11 bytes (D) |
| 0x0C | Members: up to 32 DMR IDs, u24 each, ending at 0 or 0xFFFFFF (D). IDs, not contact numbers |

### 2.5 Zone

Block 0x5C: +0x00 the zone count (D), +0x01-0x0F unknown (O); records of 145 bytes from +0x10.

| Offset | Field |
|---|---|
| 0x00 | Name, 16 bytes (D) |
| 0x10 | Channel count (D) |
| 0x11 | Channel numbers, 1-based u16, up to 64 (D) |

The stock zones have no B list.

### 2.6 Radio ID

Block 0x67: +0x00 the ID count (D), records of 16 bytes from +0x10: DMR ID (u24) then the name, 13 bytes (D). The
converter takes the first valid ID and its name as the radio's.

## 3. Detection

A read is a stock DM-32UV codeplug when its pages map (no block number on two pages) holds block 0x12 and its
channels give evidence: a record with a valid name and both frequencies is good, any other bad. At least one good
record and at most one bad one for every four good ones, as for the TYT layouts
([codeplug-stock-tyt.md](codeplug-stock-tyt.md) 2).

Inputs: the whole data flash (16 MB, `data-memory.bin` in a backup), or its start (`original-channels.bin`, up to the
end of the area).

## 4. Conversion

`importStock` fills the editing model as the TYT converter does ([codeplug-stock-tyt.md](codeplug-stock-tyt.md) 4),
with the same note codes.

| Stock | Open-firmware |
|---|---|
| First radio ID, boot text | DMR ID, radio name (cut to 8), boot lines |
| Channels | Table order. Power low, mid, high = "Low" (3), "Mid" (6), "High" (9). TX timeout is the radio's. Squelch the radio's default. Bandwidth only for FM. A DCS code with a digit above 7 is dropped (note `bad_tone`). Every DCS code comes as normal (N), with note `dcs_polarity`, until the polarity bit is mapped (5) |
| TX contact | The channel's contact (digital channels only) |
| Contacts | Contacts with time slot override off |
| RX group lists | Lists with members or used by a channel. A member ID without a contact becomes a group contact named by its ID |
| Zones | Zones |
| Scan lists, roaming, everything else | Not converted. Scan lists are reported |

Channels past the first block are not read; their number is reported as `too_many_channels`.

The web installer runs the conversion as its last step (`c7000ui.js` `bringChannels`, [web-flasher.md](../dev/web-flasher.md)):
from the stock area its check step read, or read through RadioFoundry's CPS at physical addresses, then written at
0x700000 by the Codeplug Editor's transport. The stock pages are never written, so the original stays on the radio.

## 5. Open points

- Channels past 84, contacts past 170 and zones past 28: the blocks that hold them (0x13 and up, 0x45 and up,
  0x5D and up) and whether they carry a header. The two codeplugs seen fit in the first block of each.
- TX contact byte 0, and contact numbers above 255.
- DCS: two codes read with a digit above 7 (0x8054, 0x8732), which dmrconf shows as D074 and D1742. The polarity bit.
- Squelch, TX timeout, RX only, VOX, admit criterion and talkaround.
