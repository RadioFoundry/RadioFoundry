<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Codeplug settings area

A key-value area inside the open-firmware codeplug image ([open-firmware-cps.md](open-firmware-cps.md) 7) for settings
the open-firmware layout has no field for. A new setting gets a key, not a place in the layout. Firmware and tools
that don't know a key keep it. Firmware and tools that don't know the area don't see it.

Code: `core/codeplug/codeplug_kv.c` (firmware), `site/static/js/codeplug/kv.js` (Codeplug Editor).

## 1. Where

| | |
|---|---|
| EEPROM image | 0xE000-0xEFFF: one 4 KB sector, the erase unit (`STORAGE_SECTOR`) |
| On the wire | as the EEPROM image ([open-firmware-cps.md](open-firmware-cps.md) 7.1): the same address on the STM32 mapping |

Why there:

- The firmware reads nothing in the EEPROM image past the zone table (it ends at 0xAEF0).
- qdmr and the Codeplug Editor transfer the EEPROM image up to 0xB000 and keep the bytes they don't decode. qdmr never
  reads or writes the area.
- Room on both sides. The open-firmware layout, if it grows, grows up from the zone table: 0xB000-0xDFFF (12 KB) is
  left for that, so the area isn't the first thing a longer zone table meets. 0xF000-0xFFFF is left for the area's
  own continuation (2.4).
- The settings store, the journal, the calibration and the native area are elsewhere on every board.
- Factory reset doesn't touch the open-firmware image, so it keeps the area too.
- One sector: an edit is one journaled sector rewrite ([settings-store.md](settings-store.md) 4). A power cut leaves
  the old area or the new one, never a mix.

The area sits at a fixed address and starts with a magic. Nothing else points to it. A pointer would be a second
thing a foreign write can wipe apart from the data.

## 2. Format

All values are little endian.

### 2.1 Header (16 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic: the bytes `52 46 4B 56` ("RFKV") |
| 4 | 1 | Version: 1 |
| 5 | 1 | Header length: 16. Entries start here, so a later version can add header fields that older readers skip |
| 6 | 2 | Data length: the bytes of entries after the header, at most 4096 minus the header length |
| 8 | 4 | CRC-32 of the entries (IEEE 802.3, reflected, as `storage_crc32`) |
| 12 | 4 | Reserved for the continuation (2.4): 0 in a new area, kept as found by a rewrite, not checked |

The version changes only if the entry framing changes so that a version 1 reader would misread it. A reader that
doesn't know the version reads the area as empty and refuses to write it, so it never destroys a newer area. The
cost falls on older firmware: it loses every key at once and runs on its own settings. So a version change is the last
resort. Everything else grows without one: the header through its length field, a value by appending fields (4),
the area by its continuation (2.4).

### 2.2 Entries

Packed one after another, from the header's end to the data length:

| Size | Field |
|---|---|
| 2 | Key: 0x0001-0x7FFF are assigned in section 3, 0x8000-0xFFFE are for development and never in a release. 0x0000 and 0xFFFF are not keys |
| 2 | Value length in bytes |
| n | Value |

### 2.3 Validity

The area is valid when the magic matches, the version is 1, the header length is at least 16, the header and data
fit in the sector, every entry fits inside the data length and the CRC matches. Anything else (erased flash, another
tool's bytes, a write cut short outside the journal) reads as an empty area. No key is present then.

### 2.4 Continuation (reserved, not built)

When 4 KB isn't enough, the reserved header bytes 12-15 will name a continuation: a second sector in the same entry
format, at EEPROM image 0xF000, holding more entries. A reader takes a key from the first sector before the
continuation. A version 1 reader of today ignores the reserved bytes, so it sees only the first sector: keys that live
only in the continuation read as absent, and the radio keeps its own values (4). A writer that doesn't know the
continuation keeps the reserved bytes and the header length as it found them.

## 3. Keys

| Key | Name | Value | Meaning |
|---|---|---|---|
| 0x0001 | ARS server | u32 | The presence server's DMR ID for ARS registration ([dmr-data.md](dmr-data.md) 6). 0 = ARS off |
| 0x0002 | IP gateway | u32 | The DMR ID of the radio that takes the host's off-net IP packets ([usb-network.md](usb-network.md) 3.1). 0 = none |

An ID above 16776415 (`CODEPLUG_DMR_ID_MAX`) reads as if the key were absent.

A new key is added to this table in the same change that first writes it.

## 4. Rules for readers and writers

- **Absent means "the codeplug says nothing".** The firmware then keeps what it already has (5). It is not the same
  as 0 or off. A tool that wants a setting off writes the key with the off value.
- **Unknown keys are kept.** A writer copies every entry it doesn't know, in order, byte for byte.
- **Values only grow.** A later version of a setting can append fields to the value. A reader takes the fields it
  knows from the front and ignores the rest. A value shorter than a reader expects gives the default for the missing
  fields. A field's meaning never changes. A new meaning gets a new key.
- **One entry per key.** A reader takes the first. A writer drops later ones.
- **Order.** A writer replaces a value in place, appends a new key at the end and closes the gap when it removes one.
- **Writing over an invalid area** starts a new one: the header, then the entries being written. Unknown bytes in an
  invalid area aren't kept, because nothing in them could be read.
- **Erased bytes after the data** are written 0xFF, as the erased sector. The CRC covers only the data length.

## 5. Firmware

| Call | |
|---|---|
| `codeplug_kv_get(key, buf, max)` | The value's length (copies at most `max` bytes), or -1 when the key is absent or the area invalid |
| `codeplug_kv_set(key, value, len)` | Writes one key: one journaled rewrite through the shared sector buffer. Bumps `codeplug_serial()` |

`codeplug_kv_set` needs no built-in codeplug to be stored first. The area is apart from the channel records, so it
doesn't materialize the built-in set ([codeplug-native.md](codeplug-native.md) 2). It fails while a CPS session holds
the shared sector buffer, or when the storage region isn't ours ([settings-store.md](settings-store.md) 5).
While the open-firmware area holds another firmware's codeplug (`codeplug_foreign`, [codeplug-native.md](codeplug-native.md)
2) the area reads as empty and `codeplug_kv_set` fails; the menu's write is held for the user's answer like any
other codeplug write.

The radio core reads the keys at start and whenever `codeplug_serial()` moves on (a menu edit, a CPS commit, a
TFTP write), and again when the setting changes another way: a key that is present sets the setting and puts such a
change back before it is saved. A key that is absent leaves the setting as the radio had it. Nothing of the area is
kept in RAM between reads. The
radio's own copy of each setting stays in the settings store (`ds_ars_server`, `ds_ip_gateway`,
[settings-store.md](settings-store.md) 3). So a codeplug written by a tool that doesn't carry the area doesn't change
how the radio behaves.

Every way the radio sets one of these settings writes the key: the menu (Settings > Connections > Data services,
[ui.md](ui.md)) and the debug link's DMR_DATA SETTINGS and IP_SETTINGS ([debuglink.md](debuglink.md) 3.12). A CPS
read shows the radio's setting.

Debug link view `cps`: `cpkv.state` (`valid`, `empty`: no area, `bad`: invalid, `newer`: an unknown version),
`cpkv.length` (data length) and `cpkv.keys` (entries).

## 6. Transfers

Every path that reads the codeplug reads the area. A path that writes it takes the file's area only when it is valid
(2.3) and the radio's isn't of a newer version; otherwise the radio keeps its own. A file from a tool or a firmware
that doesn't carry the area holds 0xFF there, so writing it back never erases the radio's keys.

| Path | Read | Write |
|---|---|---|
| Codeplug Editor (open-firmware CPS or fast protocol) | yes | only when one of its fields changed (7) |
| TFTP `codeplug.bin` ([tftp.md](tftp.md) 3) | yes | valid and not newer, checked by the radio at the commit |
| Fast protocol, `tools/cps_fast.py` ([cps-fast.md](cps-fast.md)) | yes | valid and not newer, checked by the tool |
| Open-firmware CPS, `tools/cps_client.py` (default regions `radio`) | yes | valid and not newer, checked by the tool |
| qdmr | no | never |
| A whole data flash backup and restore | yes | yes, as raw flash |

The open-firmware CPS and the fast protocol move raw blocks, so the radio can't tell an erased area in a file from
one the user meant to erase: the host tool applies the rule. TFTP stages the whole file first, so the radio applies
it. A tool that wants every key gone writes a valid area with no entries.

## 7. Codeplug Editor

The Radio tab shows the keys it knows as fields. A field left empty is an absent key. Typing a value writes the key;
0 turns the setting off. The editor reads and writes the sector with the other EEPROM regions, keeps entries it
doesn't know and rewrites the area only when one of its fields changed.

## 8. The native codeplug

The native codeplug ([codeplug-native.md](codeplug-native.md) 4) carries these keys, with the same numbers, value
encodings and rules, as one record in this entry format. A key defined here is never renumbered or translated: a
conversion copies the entries as they are. There is one key space for both formats.

## 9. Open

- Whether the open-firmware CPS writes this range is not known. If it writes erased bytes there, the area reads as
  empty and the radio keeps its own settings (5).
- Per-channel settings: a key whose value starts with a channel number, or a table key. Not needed yet.
