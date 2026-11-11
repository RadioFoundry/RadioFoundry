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
  first continuation (2.4).
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
| 12 | 4 | Reserved for the continuation (2.4): 0 in a new area, kept as found by a rewrite, not checked by version 1 |

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

When 4 KB isn't enough, the reserved header bytes 12-15 will name a continuation: another sector with the same
header and entry format, holding more entries. The bytes give the continuation's offset in the EEPROM image (u32, a
multiple of 4096; 0 = none), not a fixed address. The first continuation is meant for 0xF000. A continuation's own
bytes 12-15 name the next one, so later sectors can go wherever the image map has room, with no version change.

A reader takes a key from the first sector before any continuation, and from a continuation before the one it
names. A version 1 reader of today ignores the reserved bytes, so it sees only the first sector: keys that live only
in a continuation read as absent, and the radio keeps its own values (4). A writer that doesn't know the
continuation keeps the reserved bytes and the header length as it found them.

## 3. Keys

| Key | Name | Type | Absent means | Requires |
|---|---|---|---|---|
| 0x0001 | ARS server | id | The radio keeps its own setting. The value is the presence server's DMR ID for ARS registration ([dmr-data.md](dmr-data.md) 6); 0 = ARS off | ARS in the build (`CONFIG_DMR_ARS`) |
| 0x0002 | IP gateway | id | The radio keeps its own setting. The value is the DMR ID of the radio that takes the host's off-net IP packets ([usb-network.md](usb-network.md) 3.1); 0 = none | IP gateway in the build (`CONFIG_DMR_IP_GATEWAY`) |

**Requires** is what a radio needs for the key to do anything: build switches (`CONFIG_*`) and board capabilities
(`CAP_*`), generic wording, no radio names. It is the seed of a per-radio support registry: a tool will derive from it
which keys a radio supports, so a row's requires is written for that, not for a reader's guess. A key a radio doesn't
support is still kept and carried (4).

In the firmware the keys the radio core applies are one table, `radio_keys` in `core/radio/radio_keys.h`: key, type,
whether the build has it and where the setting sits. The same table on every radio. The menu, the radio core and the
debug link go through it, so a new key is a row there and a row here.

### 3.1 Types

A type names how a value is encoded, never how wide it is. A value's length (2.2) is the width.

| Type | Encoding |
|---|---|
| uint | Unsigned, little endian, 1 to 8 bytes. A shorter value zero-extends when read, so a setting that outgrows its width (u8 to u16 to u32) is stored in more bytes under the same key, and an older reader still reads a value that fits its width |
| int | Signed, little endian, 1 to 8 bytes. A shorter value sign-extends from its stored length |
| bool | A uint: 0 = off, anything else on; a writer stores 0 or 1 |
| enum | A uint. Values are only appended: a value never changes meaning, and a reader treats a value it doesn't know as the default |
| id | A uint DMR ID, 1 to 4 bytes: an ID fits in 3, and a shorter value zero-extends as any uint. An empty value, or one above 16776415 (`CODEPLUG_DMR_ID_MAX`), reads as if the key were absent. 0 is a value (the setting off), not absent |
| string | The bytes of the value, no terminator, length from the entry |
| record list | A key family (below): one key per record, each record's value is of the type its row names |

A type is never redefined. It grows by appending bytes to the value (4), and a new meaning gets a new key.

A new key is added to this table in the same change that first writes it.

**Key families.** A feature with many instances of one record (a list of records, one per entry) owns a contiguous
key range: one key per instance, record n at base + n. The range is assigned here as one row (base, size, value
format) in the same change that first writes it. Each instance is then an ordinary key: absent, present, grown and
kept by the rules in 4, and a reader walks the range (`codeplug_kv_next`, 5) rather than looking up each key.

A list that belongs to a radio's controls is a family too: the keypad buttons, one key per button, each value the
button's action. Ranges are assigned in blocks of 256 from 0x0100 up (0x0100-0x01FF the first), so a family can grow
to its block and no further without a new row. A list that may outgrow a block is split into two families by the
change that needs it, never by moving the first.

## 4. Rules for readers and writers

- **Absent means "the codeplug says nothing".** The firmware then keeps what it already has (5). It is not the same
  as 0 or off. A tool that wants a setting off writes the key with the off value.
- **Unknown keys are kept.** A writer copies every entry it doesn't know, in order, byte for byte.
- **Values only grow.** A later version of a setting can append fields to the value. A reader takes the fields it
  knows from the front and ignores the rest. A value shorter than a reader expects gives the default for the missing
  fields. A field's meaning never changes. A new meaning gets a new key.
- **One entry per key.** A reader takes the first. A writer drops later ones.
- **Order.** A writer replaces a value in place, appends a new key at the end and closes the gap when it removes one.
- **Removing** takes out every entry of the key. Removing a key that is absent changes nothing and writes nothing. A
  set to the value the key already has writes nothing either.
- **Several changes, one rewrite.** A writer applies all the sets and removes it has in one rewrite of the sector, all
  or none: if one doesn't fit, nothing changes.
- **Writing over an invalid area** starts a new one: the header, then the entries being written. Unknown bytes in an
  invalid area aren't kept, because nothing in them could be read.
- **Erased bytes after the data** are written 0xFF, as the erased sector. The CRC covers only the data length.

## 5. Firmware

| Call | |
|---|---|
| `codeplug_kv_get(key, buf, max)` | The value's length (copies at most `max` bytes), or -1 when the key is absent or the area invalid. Checks the whole area (its CRC) each call |
| `codeplug_kv_begin(&c, lo, hi)`, `codeplug_kv_next(&c, &key, buf, max)` | A walk over the keys `lo`..`hi` in the area's order, a later entry of a key skipped. The area is checked once, at the first `next`; each later one reads the header again and ends the walk if the area changed. The cursor is on the caller's stack |
| `codeplug_kv_write(ops, n)` | Applies `n` sets and removes (`len` = `CODEPLUG_KV_REMOVE`) in order, then one journaled rewrite through the shared sector buffer. All or nothing. Bumps `codeplug_kv_serial()` when a byte changed, not `codeplug_serial()`, so a key written doesn't reload the whole codeplug; a batch that changes nothing writes nothing |
| `codeplug_kv_set(key, value, len)`, `codeplug_kv_remove(key)` | One op through `codeplug_kv_write` |
| `codeplug_kv_keep(addr, sector)` | The transfer rule (6) for a raw sector write at flash address `addr`: true when the radio's sector must stay |

Reads keep nothing of the area in RAM. A cache of the checked state was left out: it would cost RAM on every radio,
every path that writes the sector raw (CPS shim, fast protocol, TFTP) and every change of the foreign-codeplug state
would have to drop it, and a write made outside the running firmware (the stub, the bootloader) can't. A walk gives
the same saving for keys read together.

The writes need no built-in codeplug to be stored first. The area is apart from the channel records, so it
doesn't materialize the built-in set ([codeplug-native.md](codeplug-native.md) 2). It fails while a CPS session holds
the shared sector buffer, or when the storage region isn't ours ([settings-store.md](settings-store.md) 5).
While the open-firmware area holds another firmware's codeplug (`codeplug_foreign`, [codeplug-native.md](codeplug-native.md)
2) the area reads as empty and the writes fail; the menu's write is held for the user's answer like any
other codeplug write.

The radio core reads the keys at start, whenever `codeplug_kv_serial()` moves on (a menu edit, a debug link write) and
whenever `codeplug_serial()` does (a CPS commit, a TFTP write: the whole codeplug's reload), and again when the setting changes another way: a key that is present sets the setting and puts such a
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

Every path that reads the codeplug reads the area. When a path writes the area's sector, the radio keeps its own
sector if

- its area is valid (2.3) and the sector written isn't, or
- its area is of a newer version (never written, 2.1).

Otherwise the sector goes in as sent. A file from a tool or a firmware that doesn't carry the area holds 0xFF there,
so writing it back never erases the radio's keys. A tool that wants every key gone writes a valid area with no
entries: that is taken.

The radio applies the rule itself (`codeplug_kv_keep`) where the sector is committed, so it holds for any tool,
including CPS software that knows nothing of the area:

| Path | Read | Where the radio applies the rule |
|---|---|---|
| Open-firmware CPS shim (`X` 3, [open-firmware-cps.md](open-firmware-cps.md) 2.2) | yes | at the sector's commit. The data writes may come in pieces of any size: the pending sector holds the stored bytes with the data over them, and that whole sector is judged. A write of part of a valid area leaves it invalid, so it is kept |
| Fast protocol ([cps-fast.md](cps-fast.md) 3.4) | yes | at END (direct), at the slot's copy (staged, and the boot's resume) |
| TFTP `codeplug.bin` ([tftp.md](tftp.md) 3) | yes | at the sector's copy in the commit |
| qdmr | no | never writes it |
| A whole data flash backup and restore | yes | not applied: raw flash |

A kept sector is acknowledged as written, so the CPS doesn't fail. A CPS that reads the sector back to verify it
sees the radio's bytes.

The host tools apply the rule as well, before they send: the Codeplug Editor writes the area only when one of its
fields changed (7), and `tools/cps_client.py` (default regions `radio`) and `tools/cps_fast.py` send it only when the
file's is valid and the radio's isn't of a newer version. That covers radios on firmware from before the radio-side
rule. Their `verify` leaves the area out by the same rule, so a kept area isn't reported as a difference.

## 7. Codeplug Editor

The Radio tab shows the keys it knows as fields. A field left empty is an absent key. Typing a value writes the key;
0 turns the setting off. The editor reads and writes the sector with the other EEPROM regions, keeps entries it
doesn't know and rewrites the area only when one of its fields changed.

## 8. The native codeplug

The native codeplug ([codeplug-native.md](codeplug-native.md) 4) carries these keys, with the same numbers, value
encodings and rules, as one record in this entry format. A key defined here is never renumbered or translated: a
conversion copies the entries as they are. There is one key space for both formats.

## 9. Open

- Whether the open-firmware CPS and Codeplug Studio write this range is not known. If they write erased bytes
  there, the radio keeps its sector (6). A CPS that verifies its write by reading back would then report a mismatch
  there.
- Per-channel settings: a key whose value starts with a channel number, or a table key. Not needed yet.
