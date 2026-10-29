<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Stock TYT-family codeplugs

The vendor firmware of the UV380 family (a two-region codeplug with 3000 channels) and of the 256 KB family (one
256 KB codeplug image) keeps its codeplug in the radio's SPI flash, in a layout of its own. RadioFoundry doesn't use that layout: it keeps the open-firmware layout
([open-firmware-cps.md](open-firmware-cps.md) 7) in the same flash. A backup taken right after RadioFoundry was installed over
the stock firmware therefore still holds the stock codeplug. This spec covers how the Codeplug Editor tells the two
layouts apart (section 2) and the stock records it converts (sections 3-4).

(T) marks a value checked with qdmr's `dmrconf` 0.15.1 on files: `dmrconf encode` run on YAML test files with one
field changed at a time and the binaries compared, `dmrconf decode` run on binaries changed one byte at a time, and a
SPI flash dump of a UV380 family radio that `dmrconf decode` reads as a codeplug when its two regions are put in a
DfuSe file.

Code: `site/static/js/codeplug/stock_tyt.js` (the records), `site/static/js/codeplug/detect.js` (section 2),
`site/static/js/editor/files.js` (the editor's Open), `site/static/js/flasher/codeplug.js` (the Tools page), tests in
`site/tests/stock.test.mjs`.

## 1. Where the codeplug is

Addresses in this spec are SPI flash addresses. dmrconf's binary for these radios is a DfuSe file whose element
addresses are the same addresses (T).

| Family | dmrconf keys | Regions |
|---|---|---|
| UV380 | `uv390`, `md2017` and the keys of the radios sold under other names | 0x002000-0x03FFFF and 0x110000-0x19FFFF |
| 256 KB | the family's one key | 0x002000-0x03FFFF (the 256 KB codeplug image) |

Radios sold under two names write the same bytes apart from the programming time (T). `uv390` and `md2017` write the
same records at the same addresses as the base model, plus tables this spec doesn't cover (T).

RadioFoundry's flash map on these radios puts its emulated EEPROM, settings ring, journal and second DMR ID database
area at addresses that overlap the stock tables ([settings-store.md](settings-store.md) 1). Until the user writes a
RadioFoundry codeplug and an ID database, those areas still hold the stock bytes. Claiming the settings area erases
it, and on some boards the zone table (0x0149E0) lies inside it. A conversion therefore reads the stock codeplug from
a copy taken before the claim, such as the installer's backup ([web-flasher.md](../dev/web-flasher.md), "Channels
after an install").

Files that carry the same codeplug:

| File | Content |
|---|---|
| SPI flash dump (16 MB, in a backup `.zip` as `spi-flash.bin`) | Every address as above |
| 256 KB family CPS file (`.rdt`, 262709 bytes) | A 549-byte header, the 256 KB image from address 0, a 16-byte trailer |
| 256 KB family raw image (262144 bytes) | The 256 KB image from address 0 |
| UV380 family CPS file (`.rdt`, 852533 bytes) | dmrconf requires this size (T). Where its regions sit in the file is not known: a 549-byte header, the image from 0, then 0x110000-0x19FFFF did not decode. The editor reports the file as not supported |

## 2. Telling the layouts apart

The editor decides from the bytes, in this order. A file can hold both layouts in part: a radio that moved to RadioFoundry and
was programmed keeps stock bytes wherever RadioFoundry hasn't written. The open-firmware layout wins when it has evidence.

1. **Open-firmware evidence.** Each record the open-firmware layout marks as in use counts as good or bad:
   - the general settings (open-firmware-cps.md 7.3.2): good when the DMR ID is valid BCD and not 0 and the callsign
     is a valid name; bad when the callsign field holds bytes that aren't a name;
   - every channel whose in-use bit is set (7.3.3): good when both frequencies are valid BCD and the name is valid;
     a record of all 0xFF bytes is erased flash and doesn't count;
   - every zone whose in-use bit is set (7.3.4) and whose record isn't all 0xFF, every contact that decodes (7.3.5), every RX group list whose count is
     set (7.3.6): good when the name is valid and not empty.

   A valid name is Latin-1 text (0x20-0x7E, 0xA0-0xFE) up to the first 0x00 or 0xFF, with only 0x00 or 0xFF after
   it. A UTF-16 name, as the stock layout stores them, fails: its second byte is 0x00 and printable bytes follow.
   The layout is the open-firmware one when there is at least one good record and at most one bad record for every
   four good ones.

   One exception: a radio put back on its stock firmware after RadioFoundry. The stock software rewrites only its own
   codeplug, so RadioFoundry's records past it still count as good, but both layouts keep their general settings in the
   chip's first sectors, which the stock codeplug covers. When the open-firmware general settings have no DMR ID and
   the stock ones (3.1) hold a valid one (1 to 16776415), the stock software wrote last: step 2 decides.
2. **Stock evidence.** The channel table of each family (section 3.2) is checked: a record is in use when its first
   name character is not 0 and its mode (byte 0 bits 0-1) is 1 or 2. An in-use record is good when both frequencies are
   valid BCD and the name is valid UTF-16 (each character 0x0020 or above and not 0xFFFF, 0x0000 after the first
   0x0000). The UV380 table is checked first, then the 256 KB table. Each needs at least one good record and at
   most one bad one for every four good ones.
3. **Blank.** No record in use in either layout: an erased or empty codeplug. The editor opens it as an empty
   open-firmware codeplug.
4. Anything else is a codeplug in a layout the editor doesn't know.

A UV380 family `.rdt` file is recognized by its size alone (section 1) and reported as not supported.

The stock dump above has no open-firmware evidence (its general settings are erased) and 16 good
stock channels. A RadioFoundry codeplug image of the same radio has good open-firmware records and no stock channels.

## 3. Records

All multi-byte numbers are little endian. Text is UTF-16LE, padded with 0x0000. Frequencies and tones use the same
encodings as the open-firmware layout (open-firmware-cps.md 7.3.3): a frequency is 8 BCD digits of 10 Hz units in
a LE 32-bit word, and a tone is a LE 16-bit word, 0xFFFF for none, BCD tenths of a Hz for CTCSS (67.0 Hz = 0x0670),
or bit 15 set for DCS with the three octal digits in the low 12 bits and bit 14 set when inverted (D754I = 0xC754)
(T).

### 3.1 General settings (0x2000)

| Address | Size | Field |
|---|---|---|
| 0x2040 | 20 | Boot text line 1, 10 characters |
| 0x2054 | 20 | Boot text line 2, 10 characters |
| 0x2084 | 3 | The radio's DMR ID, a 24-bit number |
| 0x20B0 | 32 | Radio name, 16 characters |

dmrconf also writes the programming time near 0x2002 as BCD, so two encodes of one file differ there (T).

### 3.2 Tables

| Table | UV380 family | 256 KB family | Record size | In use when (T: dmrconf decode skips the others) |
|---|---|---|---|---|
| Channels | 0x110000, 3000 | 0x01EE00, 1000 | 64 | the name is not empty (section 2 adds the mode and frequency checks) |
| Contacts | 0x140000, 10000 | 0x005F80, 1000 | 36 | the name is not empty and the call type is not 0 |
| RX group lists | 0x00EC20, 250 | 0x00EC20, 250 | 96 | the name is not empty |
| Zones | 0x0149E0, 250 | 0x0149E0, 250 | 64 | the name is not empty |
| Zone extensions | 0x031000, 250 | none | 224 | belongs to the zone with the same index |

Records refer to each other by 1-based index into the table; 0 means none. A reference to a record that isn't in use
is dropped (dmrconf leaves such a zone member out, T). A name that isn't valid UTF-16 (section 2) counts as empty, so
erased flash (0xFF) reads as unused.

RadioFoundry's settings ring and sector journal (0x011000-0x01AFFF, [settings-store.md](settings-store.md) 1) lie inside the
RX group list and zone tables of both families. Until RadioFoundry claims the area, the stock bytes there are intact. The claim
erases it, and RadioFoundry then writes its own records. The area counts as claimed when a 256-byte slot from 0x011000 to
0x018FFF starts with the settings magic "OSET", or 0x019000 or 0x01A000 starts with a journal magic ("OJNL", "OJN2").
A claimed area holds no stock records: list and zone records that overlap it are skipped, and the conversion reports
it (note `settings_area`). A backup taken before the claim keeps them.

### 3.3 Channel (64 bytes)

| Offset | Bits | Field |
|---|---|---|
| 0x00 | 0-1 | Mode: 1 FM, 2 DMR. Unused records hold 3, or 1 with an empty name |
| 0x00 | 3 | FM bandwidth: 1 = 25 kHz, 0 = 12.5 kHz |
| 0x01 | 1 | RX only |
| 0x01 | 2-3 | Time slot: 1 = TS1, 2 = TS2 |
| 0x01 | 4-7 | Color code, 0-15 |
| 0x04 | 4 | VOX |
| 0x04 | 5 | Power: 1 high, 0 low (dmrconf writes `Low` and `Mid` both as 0) |
| 0x04 | 6-7 | Admit criterion: 0 always, 1 channel free, 2 CTCSS/DCS (FM), 3 color code (DMR) |
| 0x06 | 16 | TX contact, an index into the contacts |
| 0x08 | 8 | TX timeout in 15 s units; 0 = off |
| 0x0A | 8 | Scan list index |
| 0x0C | 8 | RX group list index |
| 0x0F | 8 | 0xFF in every record seen |
| 0x10 | 32 | RX frequency |
| 0x14 | 32 | TX frequency |
| 0x18 | 16 | RX tone (decode) |
| 0x1A | 16 | TX tone (encode) |
| 0x20 | 32 bytes | Name, 16 characters |

The FM squelch level is not stored per channel: changing qdmr's channel squelch changes no byte (T).

### 3.4 Contact (36 bytes)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 3 | DMR ID or talkgroup, a 24-bit number; 0xFFFFFF for all call |
| 0x03 | bits 0-1 | Call type: 1 group, 2 private, 3 all call; 0 unused |
| 0x03 | bit 5 | Ring (call alert tone) |
| 0x04 | 32 | Name, 16 characters |

Unused records hold FF FF FF C0 and a zero name (T).

### 3.5 RX group list (96 bytes)

A 16-character name (32 bytes), then 32 contact indexes (16 bits each); the first 0 ends the list. dmrconf writes
only group call contacts into a list (T).

### 3.6 Zone (64 bytes) and zone extension (224 bytes)

A zone record is a 16-character name (32 bytes), then the first 16 channel indexes of its A list. On the UV380
family, the extension with the same index holds 48 more A channels (offset 0x00, to 64), then 64 B channels (offset
0x60). The first 0 ends each list. The 256 KB family has 16 channels per zone and no B list: dmrconf writes a longer
or two-list zone as two zones (T).

## 4. Conversion to the open-firmware layout

The Codeplug Editor and the Tools page build an open-firmware codeplug from a stock one
(`stock_tyt.js` `importStock`), the same way a qdmr import fills the editing model ([qdmr-yaml.md](qdmr-yaml.md)).

| Stock | Open-firmware |
|---|---|
| Radio name, DMR ID, boot text | Radio tab. The name is cut to 8 characters |
| Channels | Channels, numbered in table order. Power high = level "High" (9), low = "Low" (3). Squelch is the radio's default. Bandwidth only for FM. TX timeout as is, but the stock 0 (off) becomes the radio's TX timeout (note `tot_off`) |
| Contacts | Contacts with time slot override off. The ring flag is dropped |
| RX group lists | RX group lists, 32 contacts each at most |
| Zones | A zone with a B list becomes two zones, "Name A" and "Name B", as for a qdmr import |
| Admit criterion, scan lists, everything else | Not converted. Scan lists are reported |

Names keep their characters; characters outside Latin-1 become '?' when the record is written. The open-firmware
layout holds 1024 channels, 1024 contacts, 76 RX group lists and 68 zones. When a table has more, the first records
in table order are kept, except that contacts and RX group lists that a kept channel or list refers to are kept
first. Each loss is reported with the qdmr import's note codes (`too_many_channels`, `shortened`, `bad_frequency`,
`scan_lists` and the others).

## 5. Open points

- The 256 KB family addresses are from dmrconf's binary for that family only. No SPI flash dump of that family has been
  checked, so whether the vendor firmware keeps the image at SPI address 0 is unverified.
- The UV380 family `.rdt` layout (section 1).
- Power levels between low and high, and the meaning of channel bytes not listed in 3.3.
