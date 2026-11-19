<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Codeplug port: manufacturer codeplug to RadioFoundry

This page describes how the tools turn a radio's manufacturer codeplug into RadioFoundry's and write it. Every radio
does it the same way, in `tools/codeplug_port.py`. A radio's migration tool (`tools/*_migrate.py`) only reads the
manufacturer codeplug (through a dmrconf driver or a CPS file), flashes, and gives the CPS shim's address offset. The
user steps are in the [user manual](../user/README.md); which radios have a migration tool is in
[feature-matrix.md](../feature-matrix.md).

dmrconf (qdmr) is GPL and runs as an external program; the module contains none of its code. The dmrconf behavior on
this page is that of dmrconf 0.15.1.

| Part | Where |
|---|---|
| Shared steps | `tools/codeplug_port.py` |
| CPS writer and record decoder | `tools/cps_client.py` |
| Offline backup extractor | `tools/backup_codeplug.py` |
| A board's flash map and manufacturer codeplug layout | the board registry (`tools/boards.py`): `hal_flash_map_t` `openfw_eeprom`, `[stock_codeplug]` in `board.toml` |
| In-browser classifier | `site/static/js/codeplug/detect.js` `classify` |

## 1. Steps

| Step | What | Files in the work directory |
|---|---|---|
| Read | The migration tool: manufacturer codeplug to qdmr YAML | `backup-stock.yaml` |
| Timeslot | `clear_ts_overrides` (section 1.1) | `rfoundry-source.yaml` |
| Clean-up | `clean_group_lists` (section 2); off with `--no-cleanup` | `rfoundry-source.yaml` |
| Encode | `dmrconf encode --radio openuv380`, then `decode` | `rfoundry-encoded.dfu`, `rfoundry-encoded.yaml` |
| Contacts | `restore_contacts` (section 3) on the encoded image | `rfoundry-codeplug.dfu`, `rfoundry-codeplug.yaml` (decoded again) |
| Write | `cps_write_dfu`: the tool saves what the regions hold, then writes the image through the CPS shim (`tools/cps_client.py`) at the radio's offset | `codeplug-before.bin` or `openfw-window-before.bin`, by the migration tool |
| Check | `check_read_back`: read the regions back, compare bytes, decode, compare the section counts | `readback-rfoundry.dfu`, `readback-rfoundry.yaml` |

The tool reports the timeslot step, the clean-up, how many channels keep their TX contact, and RadioFoundry's limits
checked on `rfoundry-source.yaml` (what the encoder gets).

### 1.1 Contact timeslot override

The open-firmware contact record has a timeslot override (byte `0x17`: bit 0 clear = override, bit 1 = TS2), and
RadioFoundry applies it over the channel's timeslot on every call to that contact. dmrconf encodes a contact without
qdmr's `openGD77` extension as *override TS1*, so every call goes out on TS1, and a TS2 channel on a duplex hotspot uses
the wrong slot. `clear_ts_overrides` gives every contact without the extension `openGD77: {timeSlotOverride: None}`
before the encoder runs. A contact that has the extension is left as written. The tool prints how many contacts it
changed and, after the final decode, how many contacts still carry an override.

## 2. RX group list clean-up

Some manufacturer CPS programs make a radio hear only the talkgroups in a channel's RX group list, so their codeplugs
often hold one list per channel with only that channel's TX contact. RadioFoundry always hears the talk target
([dmr-voice.md](dmr-voice.md) 1), so on RadioFoundry such a list does nothing. It costs the channel's TX contact:
qdmr's open-firmware encoder writes a channel's TX contact or its group list, never both, and a channel without a
contact talks to TG 9.

| Rule | Result |
|---|---|
| A DMR channel's list is empty | The channel loses the list |
| A DMR channel's list holds only the channel's own TX contact | The channel loses the list |
| Lists with the same contacts | Merged into the first; their channels point at it |
| Lists no channel uses | Removed |

Everything else is written as it was.

## 3. TX contacts the encoder dropped

For every DMR channel that has a TX contact in `rfoundry-source.yaml` and none after encoding, the tool puts the
contact back into the encoded image: the channel record's 16-bit TX contact index at `0x2E`
([open-firmware-cps.md](open-firmware-cps.md) 7.2), 1-based in the encoded contact table. Safeguards:

- Channels pair by position and must have the same name in both files.
- The contact is found in the encoded file by name and number.
- The channel record in the image must carry the channel's name (16 characters), and its bank bitmap bit must be set
  (`cps_client.decode_channel`).

Anything that doesn't match is left alone and listed. The patched file is decoded again, and the TX contacts counted.

## 4. Writing

`dmrconf write --radio openuv380` reads the radio first and fails against RadioFoundry ("Cannot read from bank 1, addr
80h"); `dmrconf read` fails the same way. `cps_client.py` writes the same regions qdmr does (`0x80`, `0x7500`,
`0x20000`, `0x9B000`) through the same protocol, sector by sector, and only the sectors that change. The radio keeps
its quick keys, and its own VFO records (`0x7590`-`0x75B8`) differ from the file afterwards. A migration tool can add
an offset to every address, for a board whose open-firmware image sits above the data flash base.

### 4.1 DfuSe file to flat image

`dfu_image` turns a dmrconf file into one flat image in the STM32 layout of
[open-firmware-cps.md](open-firmware-cps.md) 7.1 (file offset = wire address before the radio's offset), which is
what `cps_client.channel_addr` and the writer use. `image_to_dfu` and `dfu_regions` place elements the same way.

| dmrconf driver | EEPROM target | Flash target | In the flat image |
|---|---|---|---|
| `openuv380` (the migration tools) | Empty | `0x80`, `0x7500`, `0x20000`, `0x9B000` | As in the file |
| `opengd77` (the MK22 mapping) | `0x80`, `0x7500` | `0x0`, `0x7B000`, both from 0 | The flash target moves up by `0x20000` |

A non-empty EEPROM target selects the MK22 placement. Elements that still overlap are refused.

The placement covers codeplug files only. An ID database encoded with `opengd77` is one target at the MK22 address
`0x30000` and has a different record size from the `openuv380` one, so moving it doesn't make it valid. Encode the
database with `openuv380`, as the migration tools do.

## 5. Codeplugs from a backup

A data flash backup can hold two codeplugs: RadioFoundry's, and the manufacturer's that the radio held before the
install. The tools tell them apart from the bytes and extract each.

`tools/backup_codeplug.py` turns the data flash in a backup into qdmr YAML, offline. It takes a web installer backup
bundle (`rfoundry-backup/1`, or `odr-backup/1` from before the rename; [web-flasher.md](../dev/web-flasher.md),
"Backup bundles") or a raw dump with `--board`. The board's layout comes from the board registry (`tools/boards.py`),
so a `hal_flash_map_t` in a board family directory is found too. The tool reads the first bundle part of `spi-flash`,
`c7000-data`, and `c7000-data-start`; each holds the data flash from address 0.

| Codeplug | Where it comes from | Decoded with |
|---|---|---|
| RadioFoundry | `0x120000` bytes at the board's `hal_flash_map_t` `openfw_eeprom`, as one DfuSe element at 0 | `dmrconf decode --radio openuv380` |
| Manufacturer | The board's `[stock_codeplug]` in `board.toml`: `span`, page layout `pages`, driver `dmrconf` | That driver |

The Codeplug Editor's qdmr import reads the YAML of either. The tool skips a part that is blank (all `0xFF`), not in
the backup, or not decoded by its driver, and still extracts the other. A backup taken on the manufacturer's firmware
can end before the RadioFoundry codeplug. One element over the whole image decodes the same as the four regions qdmr
writes. Each codeplug is written as `<name>-rfoundry.dfu` / `.yaml` or `<name>-stock.dfu` / `.yaml`.

Page layouts:

| `pages` | Layout | Address given to dmrconf |
|---|---|---|
| `block-id` | 4 KB pages behind a translation layer: the last byte of a page is its logical block. 0 is a free page, `0xFF` an erased one. A block found in two pages is refused | Block × `0x1000` |
| `linear` | The span as one block | Its own address |

The pages go to dmrconf at their logical addresses. With the pages at their physical addresses, the decoder walks past
the end of the span and fails ("Cannot resolve offset").

In the browser, `classify` (`site/static/js/codeplug/detect.js`) recognizes the layout of an image or a data flash
dump without dmrconf:

1. It counts the in-use records of the open-firmware layout ([open-firmware-cps.md](open-firmware-cps.md) 7) that
   look valid (printable names, only `0x00` or `0xFF` after them) and those that don't. An in-use bit over an
   erased record counts as neither. The image is a RadioFoundry codeplug when some records are good and the bad ones
   are no more than a quarter of the good ones.
2. Both layouts keep their general settings in the chip's first sectors. A radio put back on its manufacturer firmware
   keeps RadioFoundry's records where the manufacturer codeplug doesn't reach. So a manufacturer DMR ID with no
   RadioFoundry DMR ID beside it means the manufacturer software wrote last, and the manufacturer layout wins.
3. Otherwise it tries each manufacturer layout the page converts, with the same good-to-bad rule.
4. An image with no in-use records is blank; anything else is unknown.

The page converts only the manufacturer layouts it has a converter for ([feature-matrix.md](../feature-matrix.md)).

### 5.1 The manufacturer codeplug ported

`tools/backup_codeplug.py` also ports the manufacturer codeplug to RadioFoundry, by the same steps as a migration tool
(section 1, without the radio). It writes `<name>-ported.yaml` and `.dfu`, and `<name>-ported.bin`, the
`0x120000`-byte open-firmware image that the web Codeplug Editor and Restore take. `cps_client.py write` writes the
`.bin` to a radio that runs RadioFoundry. The tool prints the counts before and after, the sections over RadioFoundry's
limits and what they drop, and stops when channels went in and none came out. `--no-port` skips the port;
`--no-cleanup` skips the RX group list clean-up.

The web pages convert a manufacturer codeplug in the browser into the Codeplug Editor's model (`importStock`), the way
its qdmr import does and with the same note codes: channels, contacts, RX group lists, zones, the radio ID, and the
boot text, each cut to RadioFoundry's limits. The page writes the result as an open-firmware image.

### 5.2 A mixed codeplug

On a board whose `[stock_codeplug]` span overlaps its open-firmware image (the board's `hal_flash_map_t`), writing the
manufacturer codeplug again (with the manufacturer's firmware or CPS, or a restore) replaces RadioFoundry's radio ID,
zones, and first channels, and keeps the rest. The open-firmware decode of such a dump succeeds: the contacts are
RadioFoundry's, and the ID and zones are the manufacturer's bytes (for example, a radio ID with no name and 0 zones).
The tool flags a RadioFoundry codeplug with no valid radio ID, or with channels and no zone, on such a board, and says
to use the manufacturer one instead. When the bundle doesn't say RadioFoundry ran (`firmware_now`), or the input is a
raw dump, nothing shows that RadioFoundry ever wrote a codeplug there, so the tool writes no RadioFoundry files.
