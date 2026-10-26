<!-- SPDX-License-Identifier: MIT -->
# Codec placement: moving the stock codec to where RadioFoundry wants it

Status: implemented. Which radios have run the placed codec on the air is in the radios' own codec and hardware pages;
one STM32 radio's placed codec has been checked only in an emulator (Open items).

Each radio still runs the AMBE+2 codec its own stock firmware carries, merged on the user's machine
([../dev/codec.md](../dev/codec.md)). What changes is where it runs. Until now RadioFoundry left the codec at the addresses
stock linked it to and built its memory map around them. That ties RadioFoundry's layout to one stock build per radio, and
splits RadioFoundry's flash and RAM into pieces:

The stock addresses cost differently by SoC family:

| Family | What the stock addresses cost, before the move |
|---|---|
| STM32 radios | Flash sectors are kept for the codec at its stock address, so the application is split around them or stops inside a sector where the code starts |
| HR_C7000 radios | The RAM window is the union of the stock builds' addresses and splits RAM in two. A build whose codec ran at an address inside RadioFoundry's RAM can't be supported |

Each radio's before and after is in its codec page (`codec-<id>.md`, or the codec section of its hardware page).

## Rule

The merge tool moves the codec as one block and rewrites the absolute addresses inside it, so it runs where the
board's memory map puts it. The bytes come from the user's own stock file or radio, and only ever pass between the
user's radio, files and machine. The relocation is mechanical: a word that held an address in the old block gets the
same offset in the new one. Nothing else in the codec changes.

Which words to rewrite is a property of each stock build, not something to guess on the user's machine. It comes from
a **codec map**: one per supported stock build, made once by static analysis and kept with the profiles.

## Codec maps

A map is a JSON file, `tools/rfoundry/rfoundry/codec_maps/<board>/<profile>.json`, in format `rfoundry-codec-map/1`. It
holds addresses, lengths, offsets and hashes, never codec bytes. rfoundry, `tools/hrc7000_codec.py` and the web flasher
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

The maps are in `tools/rfoundry/rfoundry/codec_maps/<board>/`, one per stock code window the repository supports. A map is found by the SHA-256 of the stock code window,
so builds with byte-identical code share one (`codec_maps/README.md`).

The analyzer that makes the maps reads stock images, so it lives with the other stock analysis tools outside this
repository. It handles both instruction sets. A new stock build needs a run of it and a review of its notes before its
map is added here.

A map is used only for the build its hashes name. The merge checks the stock file (or the carved code and tables) by
SHA-256, applies the map, and checks the result.

## HR_C7000 radios

The codec already sits in a RadioFoundry slot in program flash (0x030E8000) and is copied to RAM at boot, so it moves at that
copy.

| Item | Before | After |
|---|---|---|
| Slot | Version 1: header, code, tables. The code's run address is stock's | Version 2: the same bytes, unchanged from stock, plus the relocation list (the map's `self_refs` offsets; `table_refs` too once tables move) after the tables |
| Boot | Copies the code to its stock address | Copies the code to the board's codec window and adds (window - stock run address) to each listed word, then checks the signature |
| RAM | Low region, codec window at stock's address, high region | One region for RadioFoundry from 0x10000; the codec window at the top of SRAM, sized to the largest code among the board's maps; the start-up stack below it |
| Tables | Copied to their stock modem SRAM address | Unchanged: the board reserves the union of its builds' table windows |
| Slot identity | CRC-32 and SHA-256 of the stock bytes | The same: the slot keeps stock bytes, so a backup or a kept slot is still recognized by the profile hashes |

The codec window (in SRAM) is the largest code among the board's maps, and the table window (in modem SRAM) is the
union of its builds' tables. Both are in the radio's codec page and the board's linker script.

Version 2 is the only slot format the firmware and the tools write. A radio with a version 1 slot reports no codec
(FM works). The web flasher rebuilds such a slot as version 2 when the build has a codec map (`c7000v1.js`, kept
only until the developer radios are moved); otherwise the codec is installed again from the stock file. The firmware
also refuses a slot whose code doesn't fit the window or whose list names a word outside the code. A build without a
codec map can't be written.

The runtime stub's window (`HRC_STUB_BASE`) follows the codec window. The slot tool is `tools/hrc7000_codec.py`.

## STM32 radios

The codec runs in place from internal flash, so it moves at the merge, and the firmware finds it through a
**descriptor**: 128 bytes at 0x080FFF80, the end of sector 11.

| Field | Content |
|---|---|
| Magic, version | "DESC", 1 |
| Profile tag | Which stock build; the firmware supports only the builds whose calling convention it knows |
| Code | Flash address, length, CRC-32 |
| Table windows | Count, then up to five (address, length, CRC-32) |
| CRC-32 | Of the 124 bytes before it |

All STM32 stock codecs seen so far are builds of one library, with the same entry offsets, argument lists and state
sizes. The codec page of the first radio hosted ("Per-frame calls") specifies them. Its V02.06 build is the reference
build that the other codec research pages compare with; [radios.md](radios.md) links each radio's pages.

Entry points stay in the firmware, as offsets from the code start. Every window must lie inside the flash the board
leaves to the codec (a board's `memory.ld` and its codec page give the sectors).

A board can take its codec from more than one stock file (`[codec] stock` in `board.toml` lists them). The files hold
the same codec, but each may put its tables at other addresses. The board's codec windows are then the union of every
file's layout: the image must stay out of all of them, and keeping the codec keeps all of them. A merge writes and
checks the layout of the file it merges, never the union.

First step, no table addresses rewritten: the code moves as one block to the start of the last flash sector's free
part, the tables stay at their stock addresses (cut to the map's minimal window), and the application gets the sectors
below as one piece. The merge rewrites the code's literal words or movw/movt pairs that point into the code
(`self_refs`) and the calls from the islands in the table windows back into the code (`branches`). Each radio's
addresses, sectors and rewrite counts are in its codec page.

The firmware (`boards/<radio>/vocoder_<radio>.c`) checks the descriptor, that the code and every table window are
where this build expects them with the expected CRCs, and the code's first word. Only then does it call the codec, and
it reports the profile with an `-s11` suffix (`<profile>-s11`). A build from before the move
reports the stock id, so the flashing tools see that the codec isn't where the new layout keeps it.

**One id, many builds.** A board reports one codec id for every stock build it accepts; the descriptor's tag tells
the builds apart. A release accepts only the tags it was built with:

| Update | Codec kept? |
|---|---|
| To the same release or a newer one | Yes, whichever supported build the codec came from |
| To a release older than the one that added the codec's build | No: that release doesn't know the build's tag, finds no codec and runs FM only |

The flashing tools see the same id either way, so they keep the codec. Going back to such a release needs the codec
merged again from a stock file that release supports.

Later step: rewrite the `table_refs` and pack code and table objects into one sector (sector 11). The maps hold what
that needs (objects, groups, compaction), so no further analysis of the stock builds is required.

**Moving to the new layout.** The application now covers the sectors where stock firmware, and RadioFoundry builds from before
the move, keep the codec code. Updates from such a build erase it, so the flashing tools need the codec from somewhere
else:

* The copy they keep. rfoundry and the web flasher read the codec from every radio running RadioFoundry over the debug link
  (READ_MEM), in the stock or the placed layout, check it against the profile, and store it on the user's machine.
  The copy is written as the stock file's merge would write it.
* Otherwise the stock file, as for a first install. There is no read-back during the update itself: only developer
  radios carry the old layout.

## Open items

* A hardware test of voice TX and RX with the moved code on each STM32 radio that has not had one (see the radios' codec
  pages). An emulator run with only the placed windows mapped gives the same air bits and PCM as the stock code at its
  stock address (60 frames, plain and stressed).
* The later step above (table moves and one-sector packing).
