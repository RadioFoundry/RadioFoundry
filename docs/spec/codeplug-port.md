<!-- SPDX-License-Identifier: MIT -->

# Codeplug port: vendor codeplug to RadioFoundry

How the migration tools turn a radio's vendor codeplug into RadioFoundry's and write it. Every radio's tool does it the
same way, in `tools/codeplug_port.py`; the radio tool only reads its vendor codeplug (its own dmrconf driver or a CPS
file), flashes, and gives the CPS shim's address offset. Each radio's procedure is in its `<id>-migration.md`
([index](README.md#migrating-from-stock)).

dmrconf (qdmr) is GPL and runs as an external program; the module contains none of its code. Facts marked (T) were
measured with dmrconf 0.15.1; (R) on a radio running RadioFoundry.

## 1. Steps

| Step | What | Files in the work directory |
|---|---|---|
| Read | the radio tool: stock codeplug to qdmr YAML | `backup-stock.yaml` |
| Timeslot | `clear_ts_overrides` (section 1.1) | `rfoundry-source.yaml` |
| Clean-up | `clean_group_lists` (section 2); off with `--no-cleanup` | `rfoundry-source.yaml` |
| Encode | `dmrconf encode --radio openuv380`, then `decode` | `rfoundry-encoded.dfu`, `rfoundry-encoded.yaml` |
| Contacts | `restore_contacts` (section 3) on the encoded image | `rfoundry-codeplug.dfu`, `rfoundry-codeplug.yaml` (decoded again) |
| Write | `cps_write_dfu`: what the regions hold is saved, then the image goes through the CPS shim (`tools/cps_client.py`) at the radio's offset | `codeplug-before.bin` or `openfw-window-before.bin`, by the radio tool |
| Check | `check_read_back`: read the regions back, compare bytes, decode, compare the section counts | `readback-rfoundry.dfu`, `readback-rfoundry.yaml` |

The tool reports the timeslot step, the clean-up, how many channels keep their TX contact, and RadioFoundry's limits checked on `rfoundry-source.yaml`
(what the encoder gets).

### 1.1 Contact timeslot override

The open-firmware contact record has a timeslot override (byte 0x17: bit 0 clear = override, bit 1 = TS2), and RadioFoundry
applies it over the channel's timeslot on every call to that contact. A contact without qdmr's `openGD77` extension
encodes as *override TS1* (T): 26 of 26 contacts, so every call went out on TS1 and a TS2 channel on a duplex hotspot
used the wrong slot (issue 93). `clear_ts_overrides` gives every contact without the extension
`openGD77: {timeSlotOverride: None}` before the encoder runs. A contact that has the extension is left as written.
The tool prints how many contacts it changed and, after the final decode, how many contacts still carry an override.

## 2. RX group list clean-up

A vendor CPS (TYT's in particular) heard only the talkgroups in a channel's RX group list, so codeplugs grew one list per
channel holding just that channel's TX contact. RadioFoundry always hears the talk target ([dmr-voice.md](dmr-voice.md) 1), so
on RadioFoundry such a list does nothing. It costs the channel's TX contact: qdmr's open-firmware encoder writes a channel's
TX contact or its group list, never both (T), and a channel without a contact talks to TG 9.

| Rule | |
|---|---|
| A DMR channel's list is empty | the channel loses the list |
| A DMR channel's list holds only the channel's own TX contact | the channel loses the list |
| Lists with the same contacts | merged into the first; their channels point at it |
| Lists no channel uses | removed |

Everything else is written as it was. On one vendor codeplug (541 channels, 96 contacts): 129 lists down to 11,
199 channels lost a list that only repeated their contact, 21 lists were in use (T).

## 3. TX contacts the encoder dropped

For every DMR channel that has a TX contact in `rfoundry-source.yaml` and none after encoding, the contact goes back into
the encoded image: the channel record's 16-bit TX contact index at 0x2E ([open-firmware-cps.md](open-firmware-cps.md)
7.2), 1-based in the encoded contact table. Safeguards:

- channels pair by position and must have the same name in both files;
- the contact is found in the encoded file by name and number;
- the channel record in the image must carry the channel's name (16 characters), and its bank bitmap bit must be set
  (`cps_client.decode_channel`).

Anything that doesn't match is left alone and listed. The patched file is decoded again and the TX contacts counted.
Same codeplug as above: after the clean-up, 10 channels still had a list different from their contact; all 10 got
their contact back, 403 of 403 channels keep theirs (T). Before this module, 194 of 403.

## 4. Writing

`dmrconf write --radio openuv380` reads the radio first and fails against RadioFoundry: "Cannot read from bank 1, addr 80h"
(R, issue #9); `dmrconf read` fails the same way. `cps_client.py` writes the same regions qdmr does (0x80, 0x7500,
0x20000, 0x9B000) through the same protocol, sector by sector, and only the sectors that change. The radio keeps its
quick keys, and its own VFO records (0x7590-0x75B8) differ from the file afterwards (R). A radio tool may add an offset to every address (the open-firmware image sits above the data flash base on some radios).

## 5. Codeplugs from a backup

`tools/backup_codeplug.py` turns the data flash in a backup into qdmr YAML, offline. It takes a web installer backup
bundle (`rfoundry-backup/1`, [web-flasher.md](../dev/web-flasher.md), "Backup bundles") or a raw dump
with `--board`. The bundle part it reads is the first of `spi-flash`, `c7000-data` and `c7000-data-start`; each holds
the data flash from address 0.

| Codeplug | Where it comes from | Decoded with |
|---|---|---|
| RadioFoundry | 0x120000 bytes at the board's `hal_flash_map_t` `openfw_eeprom`, as one DfuSe element at 0 | `dmrconf decode --radio openuv380` |
| Stock | the board's `[stock_codeplug]` in `board.toml`: `span`, page layout `pages`, driver `dmrconf` | that driver |

A part that is blank (all 0xFF) or not in the backup is skipped. One element over the whole image decodes the same as
the four regions qdmr writes (T). Each codeplug is written as `<name>-rfoundry.dfu` / `.yaml` or `<name>-stock.dfu` /
`.yaml`.

Page layouts:

| `pages` | Layout | Address given to dmrconf |
|---|---|---|
| `block-id` | 4 KB pages behind a translation layer: the last byte of a page is its logical block. 0 is a free page, 0xFF an erased one. A block found in two pages is refused | block × 0x1000 |

With the pages at their physical addresses, the decoder walks past the end of the span and fails ("Cannot resolve
offset") (T).
