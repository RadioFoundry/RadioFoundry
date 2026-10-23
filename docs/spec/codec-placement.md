<!-- SPDX-License-Identifier: MIT -->
# Codec placement: moving the stock codec to where ODR wants it

Status: implemented, 2026-10-19. Hardware: DM-1701, DM-32UV and DB25-D (018 and 020) checked on the radio; MD-380
checked in an emulator only (Open items).

Each radio still runs the AMBE+2 codec its own stock firmware carries, merged on the user's machine
([../dev/codec.md](../dev/codec.md)). What changes is where it runs. Until now ODR left the codec at the addresses
stock linked it to and built its memory map around them. That ties ODR's layout to one stock build per radio, and
splits ODR's flash and RAM into pieces:

| Radio | What the stock addresses cost, before the move |
|---|---|
| DM-1701 | Flash sectors 7, 8, 10 and 11 (512 KB) are kept for about 140 KB of codec; the application gets sectors 3-6 (336 KB) and sector 9 apart |
| MD-380 | The application stops inside sector 6 (275 KB), where the code starts |
| DM-32UV | The RAM window is the union of two builds' addresses (101 KB for 83 KB of code) and splits RAM in two |
| DM-1801 Pro, DB25-D | RAM is split in two around the codec |
| DB25-D with stock 020 | Not supported: its codec ran at an address inside ODR's RAM |

## Rule

The merge tool moves the codec as one block and rewrites the absolute addresses inside it, so it runs where the
board's memory map puts it. The bytes come from the user's own stock file or radio, and only ever pass between the
user's radio, files and machine. The relocation is mechanical: a word that held an address in the old block gets the
same offset in the new one. Nothing else in the codec changes.

Which words to rewrite is a property of each stock build, not something to guess on the user's machine. It comes from
a **codec map**: one per supported stock build, made once by static analysis and kept with the profiles.

## Codec maps

A map is a JSON file, `tools/odrflash/odrflash/codec_maps/<board>/<profile>.json`, in format `odr-codec-map/1`. It
holds addresses, lengths, offsets and hashes, never codec bytes. odrflash, `tools/hrc7000_codec.py` and the web flasher
(through `data/flash-boards.json`) read the same files.

| Field | Content |
|---|---|
| `profile`, `board`, `isa` | The build's id, the radio, and `thumb2` (STM32) or `csky` (HR_C7000) |
| `stock` | File name, SHA-256 (null when the build is known only from a radio read), version |
| `code` | Stock flash address, length, run address (the flash address on STM32; the RAM address stock copies it to on HR_C7000), SHA-256, the length reachable from the entries, the alignment a move must keep |
| `entries` | Entry points as offsets from the code start |
| `self_refs` | Every word or movw/movt pair in the code that holds an address inside the code's own run range: offset, value. Rewritten when the code moves |
| `table_refs` | Every site in the code that holds the address of codec data: offset, kind (literal word or instruction pair), target. Rewritten when the tables move |
| `islands` | Code that stock keeps inside a table window (STM32), with `island_ptrs` (code words that point at it) and `island_refs` (its references to codec data) |
| `branches` | The calls from the islands into the code: site, target. Re-encoded when the code moves |
| `objects` | The data objects the table refs point into: start, length (and a conservative length), and how the length is known |
| `groups` | Objects the code indexes across, which must keep their distance |
| `table_windows` | The stock table windows (as the profiles carve them), and the minimal window from the first object to the end of the last, with its SHA-256 |
| `outside_refs` | References from the codec to anything outside its windows; empty for every supported build |
| `compaction` | The packed size of code plus objects and islands, for the one-sector packing |

The repository has 13 maps: the DM-1701 V02.06, the MD-380 D002.032, six DM-32UV code windows (which cover its twelve
stock builds), the DM-1801 Pro 049 and the four DB25-D builds. A map is found by the SHA-256 of the stock code window,
so builds with byte-identical code share one (`codec_maps/README.md`).

The analyzer that makes the maps reads stock images, so it lives with the other stock analysis tools outside this
repository. It handles both instruction sets. A new stock build needs a run of it and a review of its notes before its
map is added here.

A map is used only for the build its hashes name. The merge checks the stock file (or the carved code and tables) by
SHA-256, applies the map, and checks the result.

## HR_C7000 radios (DM-32UV, DM-1801 Pro, DB25-D)

The codec already sits in an ODR slot in program flash (0x030E8000) and is copied to RAM at boot, so it moves at that
copy.

| Item | Before | After |
|---|---|---|
| Slot | Version 1: header, code, tables. The code's run address is stock's | Version 2: the same bytes, unchanged from stock, plus the relocation list (the map's `self_refs` offsets; `table_refs` too once tables move) after the tables |
| Boot | Copies the code to its stock address | Copies the code to the board's codec window and adds (window - stock run address) to each listed word, then checks the signature |
| RAM | Low region, codec window at stock's address, high region | One region for ODR from 0x10000; the codec window at the top of SRAM, sized to the largest code among the board's maps; the start-up stack below it |
| Tables | Copied to their stock modem SRAM address | Unchanged: the board reserves the union of its builds' table windows |
| Slot identity | CRC-32 and SHA-256 of the stock bytes | The same: the slot keeps stock bytes, so a backup or a kept slot is still recognized by the profile hashes |

| Radio | Codec window (SRAM) | Table window (modem SRAM) | Builds |
|---|---|---|---|
| DM-32UV | 0x43A00..0x58000 | 0x18000E48..0x18003E28 | DM32 032 to 049 |
| DM-1801 Pro | 0x43A00..0x58000 | 0x18000EF0..0x18003E68 | the radio's 049 program flash |
| DB25-D | 0x44E00..0x58000 (the 020 code, 78,172 bytes, is the largest) | 0x180014AC..0x18004360 | 018 (three stock files), 020 (a program flash read) |

Version 2 is the only slot format the firmware and the tools write. A radio with a version 1 slot reports no codec
(FM works). The web flasher rebuilds such a slot as version 2 when the build has a codec map (`c7000v1.js`, kept
only until the developer radios are moved); otherwise the codec is installed again from the stock file. The firmware
also refuses a slot whose code doesn't fit the window or whose list names a word outside the code. A build without a
codec map can't be written.

The runtime stub's window (`HRC_STUB_BASE`) follows the codec window. The slot tool is `tools/hrc7000_codec.py`.

## STM32 radios (DM-1701, MD-380, MD-9600)

The codec runs in place from internal flash, so it moves at the merge, and the firmware finds it through a
**descriptor**: 128 bytes at 0x080FFF80, the end of sector 11.

| Field | Content |
|---|---|
| Magic, version | "DESC", 1 |
| Profile tag | Which stock build (0x0206 V02.06, 0x2032 D002.032, 0x0609 and 0x1645 for the MD-9600's 06.09 and 16.45); the firmware supports only the builds whose calling convention it knows |
| Code | Flash address, length, CRC-32 |
| Table windows | Count, then up to five (address, length, CRC-32) |
| CRC-32 | Of the 124 bytes before it |

Entry points stay in the firmware, as offsets from the code start. Every window must lie inside the flash the board
leaves to the codec (DM-1701 sectors 10 and 11, MD-380 and MD-9600 sectors 8-11).

A board can take its codec from more than one stock file (`[codec] stock` in `board.toml` lists them). The files hold
the same codec, but each may put its tables at other addresses. The board's codec windows are then the union of every
file's layout: the image must stay out of all of them, and keeping the codec keeps all of them. A merge writes and
checks the layout of the file it merges, never the union.

A board can take its codec from more than one stock file (`[codec] stock` in `board.toml` lists them). The files hold
the same codec, but each may put its tables at other addresses. The board's codec windows are then the union of every
file's layout: the image must stay out of all of them, and keeping the codec keeps all of them. A merge writes and
checks the layout of the file it merges, never the union.

First step, no table addresses rewritten:

| Radio | Code moves to | Tables | Free for ODR |
|---|---|---|---|
| DM-1701 V02.06 | 0x080E3A74 in sector 11, right after the end of the codec's data (the map's minimal window ends at 0x080E3A72) | Stay at 0x080D19F0, cut to 0x12082 bytes (sectors 10, 11) | Sectors 3-9: 0x0800C000..0x080BFFFF, 720 KB in one piece (was 336 KB + 128 KB) |
| MD-380 D002.032 | 0x080E0000, the start of sector 11, below the small windows | Stay: the main window in sector 8, the four small ones in sectors 10 and 11 | Sectors 3-7: 0x0800C000..0x0807FFFF, 464 KB in one piece (was 275 KB); sector 9 is unused |
| MD-9600 06.09 and 16.45 | 0x080E0000, the start of sector 11 | Stay in sector 9 at the build's address (0x080AB13C or 0x080AC2BC); no small windows | Sectors 3-7: 0x0800C000..0x0807FFFF, 464 KB in one piece; sectors 8 and 10 are unused |

What the merge rewrites (the map's `self_refs` and `branches`):

| Radio | In the code | In the table window |
|---|---|---|
| DM-1701 V02.06 | One literal word that points into the code | 12 calls (BL) from the code stock keeps in the window back into the code |
| MD-380 D002.032 | Two movw/movt pairs that point into the code | 12 calls (BL), as on the DM-1701 |
| MD-9600 06.09 and 16.45 | One literal word that points into the code | 12 calls (BL), as on the DM-1701 |

The firmware (`boards/<radio>/vocoder_<radio>.c`) checks the descriptor, that the code and every table window are
where this build expects them with the expected CRCs, and the code's first word. Only then does it call the codec, and
it reports the profile with an `-s11` suffix (`dm1701-v0206-csv-s11`, `md380-d2032-s11`, `md9600-s11` for both MD-9600 builds). A build from before the move
reports the stock id, so the flashing tools see that the codec isn't where the new layout keeps it.

Later step: rewrite the `table_refs` and pack code and table objects into one sector (sector 11). The maps hold what
that needs (objects, groups, compaction), so no further analysis of the stock builds is required.

**Moving to the new layout.** The application now covers the sectors where stock firmware, and ODR builds from before
the move, keep the codec code. Updates from such a build erase it, so the flashing tools need the codec from somewhere
else:

* The copy they keep. odrflash and the web flasher read the codec from every radio running ODR over the debug link
  (READ_MEM), in the stock or the placed layout, check it against the profile, and store it on the user's machine.
  The copy is written as the stock file's merge would write it.
* Otherwise the stock file, as for a first install. There is no read-back during the update itself: only developer
  radios carry the old layout.

## Open items

* MD-380: a hardware test of voice TX and RX with the moved code. An emulator run with only the placed windows mapped
  gives the same air bits and PCM as the stock code at its stock address (60 frames, plain and stressed).
* The later step above (table moves and one-sector packing).
