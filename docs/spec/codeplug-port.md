<!-- SPDX-License-Identifier: MIT -->

# Codeplug port: stock codeplug to ODR

How the migration tools turn a radio's stock codeplug into ODR's and write it. Every radio's tool does it the same
way, in `tools/codeplug_port.py`; the radio tool only reads its stock codeplug (its own dmrconf driver or a CPS
file), flashes, and gives the CPS shim's address offset. Users: [md380_migrate.py](md380-migration.md),
[dm32_migrate.py](dm32-migration.md).

dmrconf (qdmr) is GPL and runs as an external program; the module contains none of its code. Facts marked (T) were
tested with dmrconf 0.15.1; (R) on an MD-380 running ODR.

## 1. Steps

| Step | What | Files in the work directory |
|---|---|---|
| Read | the radio tool: stock codeplug to qdmr YAML | `backup-stock.yaml` |
| Clean-up | `clean_group_lists` (section 2); off with `--no-cleanup` | `odr-source.yaml` |
| Encode | `dmrconf encode --radio openuv380`, then `decode` | `odr-encoded.dfu`, `odr-encoded.yaml` |
| Contacts | `restore_contacts` (section 3) on the encoded image | `odr-codeplug.dfu`, `odr-codeplug.yaml` (decoded again) |
| Write | `cps_write_dfu`: what the regions hold is saved, then the image goes through the CPS shim (`tools/cps_client.py`) at the radio's offset | `codeplug-before.bin` (MD-380), `openfw-window-before.bin` (DM-32) |
| Check | `check_read_back`: read the regions back, compare bytes, decode, compare the section counts | `readback-odr.dfu`, `readback-odr.yaml` |

The tool reports the clean-up, how many channels keep their TX contact, and ODR's limits checked on `odr-source.yaml`
(what the encoder gets).

## 2. RX group list clean-up

Stock CPS (TYT's in particular) heard only the talkgroups in a channel's RX group list, so codeplugs grew one list per
channel holding just that channel's TX contact. ODR always hears the talk target ([dmr-voice.md](dmr-voice.md) 1), so
on ODR such a list does nothing. It costs the channel's TX contact: qdmr's open-firmware encoder writes a channel's
TX contact or its group list, never both (T), and a channel without a contact talks to TG 9.

| Rule | |
|---|---|
| A DMR channel's list is empty | the channel loses the list |
| A DMR channel's list holds only the channel's own TX contact | the channel loses the list |
| Lists with the same contacts | merged into the first; their channels point at it |
| Lists no channel uses | removed |

Everything else is written as it was. On one stock MD-380 codeplug (541 channels, 96 contacts): 129 lists down to 11,
199 channels lost a list that only repeated their contact, 21 lists were in use (T).

## 3. TX contacts the encoder dropped

For every DMR channel that has a TX contact in `odr-source.yaml` and none after encoding, the contact goes back into
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

`dmrconf write --radio openuv380` reads the radio first and fails against ODR: "Cannot read from bank 1, addr 80h"
(R, issue #9); `dmrconf read` fails the same way. `cps_client.py` writes the same regions qdmr does (0x80, 0x7500,
0x20000, 0x9B000) through the same protocol, sector by sector, and only the sectors that change. The radio keeps its
quick keys, and its own VFO records (0x7590-0x75B8) differ from the file afterwards (R). The DM-32's tool adds
0x700000 to every address.
