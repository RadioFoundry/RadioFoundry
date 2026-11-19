<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# The AMBE codec

DMR voice needs DVSI's AMBE+2 codec. The codec is proprietary, so RadioFoundry never contains it: not in the
repository, a release, or a test vector file. Each radio runs the codec that its own manufacturer firmware carries.
The user supplies that firmware file, or a read of the radio's flash, and a tool merges the codec into the
RadioFoundry image on the user's machine.

This page covers how the codec reaches a radio and how updates keep it. For which radios take which files, see each
board's codec maps (see [Codec maps](#codec-maps)) and its `board.toml` `[codec]` table.

## How the codec reaches a radio

A merge tool reads the codec from the user's file, moves it to where the board's memory map puts it, and writes it
with the image. Each platform has its own tool and hosting:

| Platform | Merge tool | Where the codec runs |
|---|---|---|
| `stm32f4` | `tools/rfoundry` (`rfoundry/stockfw.py`), or `tools/codec_extract.py` for a file | In place, from the last flash sectors. A [descriptor](#the-descriptor) says where each window is |
| `hrc7000` | `tools/hrc7000_codec.py` | In RAM. The image carries it in a [codec slot](#the-codec-slot) at the end of program flash, and the firmware copies it to RAM at boot |

Each tool does the following:

- It recognizes each supported file by SHA-256 and refuses any other file.
- It moves the codec as the build's codec map says, and checks the result.
- It writes the merged image outside tracked files (default `build/`), or, in rfoundry and the web flasher, straight
  to the radio. The merged codec exists only on the user's machine and in the radio.
- It leaves the flashing to the radio's flash tool.

For a source build, put the file in `codecs/`. `make` then runs the board's tool after every build
(`tools/codec_auto.py`; see [codecs/README.md](../../codecs/README.md)). For a release image, rfoundry and the web
flasher merge in memory, or keep the codec that's already in the radio (see [Keep and merge](#keep-and-merge)).

Without a codec, the vocoder reports itself absent, and the radio runs FM only and shows "No codec: voice off".

## Codec maps

A codec map says where one manufacturer build keeps its codec and which words and instructions in it hold addresses.
The maps are in `tools/rfoundry/rfoundry/codec_maps/<board>/<profile>.json`, one per supported build, in the format
`rfoundry-codec-map/1`. A map holds addresses, lengths, offsets, and hashes, never codec bytes.

A tool finds a map by the SHA-256 of the build's code window, so one map serves every build whose code window is
byte-identical. A build whose window differs, even only in words outside the codec, gets its own map. To find which
files a board accepts, read its maps: `stock.name`, `stock.version`, and `stock.sha256` name each one.

| Field | Content |
|---|---|
| `format`, `profile`, `board`, `isa` | The format, the build's id, the board, and `thumb2` (`stm32f4`) or `csky` (`hrc7000`) |
| `stock` | The build's file name, version, and SHA-256. The SHA-256 is null when the build is known only from a read of a radio |
| `code` | The code window: flash address, length, run address (the flash address on `stm32f4`, the RAM address it's copied to on `hrc7000`), SHA-256, the length reachable from the entries, and the alignment a move must keep |
| `entries` | The entry points, as offsets from the code's start |
| `self_refs` | Every word or `movw`/`movt` pair in the code that holds an address inside the code's own run range: offset and value. Rewritten when the code moves |
| `table_refs` | Every site in the code that holds the address of codec data: offset, kind (`word` or `movw_movt`), and target. Rewritten when the tables move |
| `islands`, `island_ptrs`, `island_refs`, `branches` | Code kept inside a table window, the code words that point at it, its references to codec data, and its calls into the code |
| `objects`, `groups`, `table_windows` | The data objects the table references point into, the objects the code indexes across (which must keep their distance), and the table windows with the minimal window and its SHA-256 |
| `outside_refs` | References from the codec to anything outside its windows. Empty for every supported build |
| `compaction` | The packed size of the code, objects, and islands |

The following read the same files:

- rfoundry: `rfoundry/codecmap.py`.
- The `hrc7000` slot tool: `tools/hrc7000_codec.py`.
- The web flasher: `site/static/js/flasher/codecmap.js`, through the `data/flash-boards.json` the site build writes.

The Python and JavaScript readers share test vectors (`tools/rfoundry/tests/codecmap_vectors.json`).

A map is used only for the build its hashes name. A move rewrites each listed site and leaves every other byte as it
is. Each site must hold exactly the value that the source layout implies before it's rewritten, which refuses a wrong
build, a wrong layout, or a damaged copy. The same rewrite, run backwards, takes a placed copy back to the build's own
windows (`codecmap.relocate`), so a copy read from a radio is checked against the build's SHA-256s.

## The descriptor

On an `stm32f4` radio, the codec runs in place from internal flash. The merge moves every codec window (the code and
each table window) as one block into the last flash sectors, and the application gets the sectors below as one
piece. Each window keeps its address modulo 16 and its bytes in order. The firmware finds the codec through a
descriptor, the last 128 bytes of the last flash sector:

| Field | Content |
|---|---|
| Magic, version | `DESC`, 1 |
| Profile tag | Which build. The firmware supports only the builds whose calling convention it knows |
| Code | Flash address, length, CRC-32 |
| Table windows | Count, then up to five (address, length, CRC-32) |
| CRC-32 | Of the 124 bytes before it |

The firmware (`boards/<b>/vocoder_<b>.c`, hosted by `platform/stm32f4/hrc/vocoder.c`) checks the descriptor, that the
code and every table window are where this build expects them with the expected CRCs, and the code's first word. Only
then does it call the codec. It reports the codec's profile with an `-s11` suffix (`<profile>-s11`) as `codec=` in
the debug link's VERSION reply ([debuglink.md](../spec/debuglink.md) 3.1).

The placed CRC-32 of each window follows from the build's CRC-32 and the map alone, without the codec bytes
(`codecmap.relocate_crcs`), because CRC-32 is affine over XOR and only the listed sites change.

A board can take its codec from more than one file (`[codec] stock` in `board.toml`). The files hold the same codec,
but each can put its tables at other addresses. The board's codec windows are then the union of every file's layout:
the image stays out of all of them, and keeping the codec keeps all of them. A merge writes and checks the layout of
the file it merges, never the union.

A board reports one codec id for every build it accepts; the descriptor's tag tells them apart. A release accepts only
the tags it was built with, so an update to an older release that doesn't know the codec's build finds no codec and
runs FM only. To go back to such a release, merge the codec again from a file that release supports.

**State.** `platform/stm32f4/hrc/vocoder.c` keeps one CCM block, the size of the encoder's state, for either the
encoder's state or the decoder's, never both. Voice is simplex, and each half of the init touches only its own state,
so the first encode after a decode runs the encoder's init on the zeroed block, and the first decode after an encode
runs the decoder's.

## The codec slot

On an `hrc7000` radio, the image carries the codec in a slot at the end of program flash, after the first stage and
the application on a board that has a first stage. The slot is format version 2: a header, the code and tables
unchanged from the manufacturer build, and the relocation list (the map's `self_refs` offsets) after the tables.

At boot, `platform/hrc7000/vocoder.c` copies the code to the board's codec window at the top of SRAM, adds the
difference between the window and the build's run address to each listed word, and checks the signature. It copies the
tables to their own address in modem SRAM. The codec window is the largest code among the board's maps, and the table
window is the union of its builds' tables; both are in the board's linker script, and the profile prefix is in its
`board.mk`. The firmware refuses a slot of another version, a slot whose code doesn't fit the window, and a list that
names a word outside the code.

Because the slot keeps the manufacturer bytes, a backup or a kept slot is still recognized by the map's hashes. A
build without a codec map can't be written.

## Keep and merge

A flash sector can only be erased whole, so each board's linker script keeps the application out of the sectors that
hold the codec. An update can then leave the codec alone (keep) or write it again (merge).

| Platform | Application | Codec | Keep |
|---|---|---|---|
| `stm32f4` | The sectors from the application's start to the codec sectors | The last sectors, found through the descriptor | Yes: the update writes only the application's sectors |
| `hrc7000` | From the application base to the slot | The slot at the end of the region | Per route: through the stub, yes (it writes only the image's sectors); through a loader that erases the whole area, no |

Each board's sectors and codec windows come from the board registry and its linker script (see
[Sector maps](#sector-maps)).

rfoundry and the web flasher (`site/static/js/flasher/decide.js`) pick the mode by the same rules. `rfoundry flash
--dry-run` prints the sectors either mode would erase and write.

1. **Keep when the radio says its codec is there.** Flash can't be read back through an `stm32f4` radio's bootloader,
   so the tools ask the running firmware first, over the debug link (`rfoundry probe`, or the start of
   `rfoundry flash`). The VERSION reply's `codec=` is the firmware's own check, the same one that gates DMR voice. If
   it names the board's codec profile (`codec.id` in the registry), keep is safe. The answer is remembered for 30
   minutes in rfoundry's settings directory, because the debug link is gone once the radio is in its bootloader.
2. **Move the codec when it's in a former layout.** If the radio reports the id of an earlier layout (the registry's
   `codec_former`), the codec sits where this release's image goes. The tools read it back and merge it into the new
   layout (see [Moving to a new layout](#moving-to-a-new-layout)).
3. **Otherwise merge.** The tools write the codec from the copy they saved from this radio, if they have one, or else
   from the user's file. Manufacturer firmware never holds the codec where RadioFoundry keeps it, so
   `--stock-installed` never makes keep safe.
4. **`--codec keep` forces keep.** Where the radio is known to have no codec, keep expects none, so the check after
   flashing doesn't count `codec=absent` as a failure. The radio is known to have no codec when it reported none, or
   when the user says it runs its manufacturer firmware (`--stock-installed`, or the web flasher's "Original firmware")
   and no manufacturer version of the board holds the codec where RadioFoundry keeps it (the board's `codec.installed`
   is empty).

A wrong guess is harmless. The firmware never runs the codec without checking it first. If the bytes aren't the
expected codec, FM works, DMR calls show "No codec: voice off", and the debug link reports `codec=absent`. After every
flash, rfoundry waits for the radio to restart and reads `codec=` again. To fix the radio, flash again with `--stock`.

### The saved copy

Whenever rfoundry or the web flasher sees a radio running RadioFoundry, it reads the codec over the debug link
(READ_MEM), in any layout the codec can have, checks it against the map's hashes, and saves it: in rfoundry's settings
directory (`rfoundry/codecsave.py`) or in the browser (`site/static/js/flasher/codecsave.js`). It writes that copy
instead of asking for the user's file whenever the radio's codec can't be kept or checked: no answer over the debug
link, the radio back on its manufacturer firmware, or no codec reported. There is no read-back during the update
itself.

### Moving to a new layout

A board's layout is where its merge puts the codec: the application region in `memory.ld`, the placement of each
profile in `tools/rfoundry/rfoundry/stockfw.py`, and the profiles the firmware pins in `boards/<b>/vocoder_<b>.c`.
They change together, and a changed layout gets a new codec id, so a radio reports which layout it holds. A profile
lists the layouts earlier releases used as `former` entries, with their codec id, placement, and placed CRCs. The
firmware accepts only its current layout: a descriptor that names a window below the board's codec flash is refused
before any byte of the codec is read.

When a new layout gives the application flash that held the codec, the first install of the new layout moves it,
inside the normal install:

| Step | What happens | If it stops here |
|---|---|---|
| 1. Probe | The radio reports a former codec id over the debug link | Nothing is written |
| 2. Read back | READ_MEM reads every window where the descriptor says the codec is, and the tools check the copy against the map. They read it on every install that moves the codec, even with a copy saved before | Nothing is written |
| 3. Save | The tools store the copy on the user's machine, one per radio by its chip UID (INFO_CHIP), and never delete it. rfoundry writes `<config>/codecs/<board>-<codec id>-<uid>.bin` and reads it back; the web flasher uses the browser's IndexedDB | Nothing is written |
| 4. Plan | A merge from the copy, moved to the current layout, or else from the user's file or the codec merged into the image. With none, or with keep asked for, the install is refused. Where the radio can't be read, only a copy saved from the same radio (same UID) is used. A copy the tools couldn't store or read back isn't used | Nothing is written |
| 5. Write | Bootloader route: the plan's sectors are erased with the application's first sector (the vector table) first, and written with it last (`vector_last` in `rfoundry/plan.py`, `vectorLast` in `site/static/js/flasher/plan.js`). Stub route: the first sector is erased first, then the other sectors are written, then the first sector without its first word, a CRC check, and the first word | The bootloader starts (the first sector is blank or the image is incomplete), or the new application starts without a valid descriptor and runs FM only |
| 6. Check | The radio reports the current codec id | |

To finish a move that stopped, run the same install again. It finds no codec to keep and merges from the saved copy (a
radio in its bootloader can't give its UID, so the copy saved last), or from the same file or merged image. The
bootloader and the stub are never written: every plan is checked against the board's protected ranges before the
first erase.

A plain image written through the stub over flash whose descriptor names a codec window is refused before anything is
erased (`stub/f4stub.py`, the web flasher's `f4stub.js`), because that install would lose the codec without a copy.

### An interrupted update

All erases come first, then the writes, both in the board's sector order, so the sector with the vector table is the
first one written back. An update cut off before that sector leaves the bootloader running. Cut off later, the radio
can boot half an application: a crash, never a bricked radio. Enter the bootloader by hand (the board's key
combination at power on) and flash again.

## Sector maps

Each board's sector map comes from the board registry:

- The application region and the codec symbols come from the board's linker script.
- The sectors are the platform's default geometry (`default_sectors` in `platform.toml`) from the application's start,
  or `app+slot` on `hrc7000`. A board's `[flash] sectors` overrides them.
- The protected range is the flash from the platform's `flash_base` to the application.
- The `stm32f4` codec windows come from the codec profiles (`stockfw.PROFILES`), which rfoundry reads when it loads the
  registry.

`tools/boards.py json --for rfoundry` writes the maps to `tools/rfoundry/rfoundry/registry.json`, which rfoundry,
`tools/release.py`, and the stub tools read. `tools/ci.sh` fails when the file is out of date.

A release manifest lists the codec's windows and profile (see [Release artifacts](release.md#release-artifacts)).
rfoundry and the web flasher refuse a manifest with two ranges or with a profile without the `-s11` suffix, and
`tools/stm32_dfu.py` refuses a release image whose manifest lists two ranges.

## In the firmware

| Part | Where |
|---|---|
| The interface the core calls | `hal/hal_vocoder.h` (`board.vocoder`) |
| `stm32f4` hosting: state init, per-frame calls, the descriptor checks | `platform/stm32f4/hrc/vocoder.c` |
| Which builds a radio accepts: entry offsets, placement, CRCs | `boards/<b>/vocoder_<b>.c` |
| `hrc7000` hosting: the slot header and the copy to RAM | `platform/hrc7000/vocoder.c` and `platform/hrc7000/codec_slot.h`; the slot and windows in the board's linker script, the profile prefix in its `board.mk` |
| AMBE frame FEC and the voice path around the codec | `core/dmr/ambe` |

The host tests of the voice path (`tests/host/ambeVoiceTest.c`) use vectors recorded from the codec's FEC, not the
codec itself.

## Adding a radio's codec

1. Describe the codec in the radio's spec: the files it ships in, their format, the code and table windows, the entry
   points, the calling convention, and the state sizes.
2. Add a codec map for each supported build to `tools/rfoundry/rfoundry/codec_maps/<board>/`.
3. Add the profiles to the board's merge tool and the hosting to its vocoder file.
4. Fill in the board's `board.toml` `[codec]` table: the tool, and `stock` (the SHA-256 of each file the codec is
   merged from) or `profile` (for a codec in a linker slot). `codecs/` and `tools/codec_auto.py` find the tool there
   (see [Board registry](board-registry.md#codec)).
