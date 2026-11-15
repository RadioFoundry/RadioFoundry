<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Codeplug settings area

A key-value area inside the open-firmware codeplug image ([open-firmware-cps.md](open-firmware-cps.md) 7) for settings
the open-firmware layout has no field for. A new setting gets a key, not a place in the layout. Firmware and tools
that don't know a key keep it. Firmware and tools that don't know the area don't see it.

Code: `core/codeplug/codeplug_kv.c` (the area), `core/codeplug/codeplug_keys.c` (tables of keyed settings),
`site/static/js/codeplug/kv.js` (Codeplug Editor).

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
finds a later version reads the area as empty and refuses to write it, so it never destroys a newer area. Version 0
was never written: it is an invalid area (2.3), not a newer one. The
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

| State | What is there | Read | Written |
|---|---|---|---|
| valid | as above | its keys | yes |
| empty | an erased sector | no key | yes: a new area |
| bad | anything else, version 0 included | no key | yes: a new area, the old bytes dropped |
| newer | our magic and a version above 1 | no key | never, by the radio or a tool (6) |

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

| Key | Name | Type | Value | Radio's own copy | Requires |
|---|---|---|---|---|---|
| 0x0001 | ARS server | id | The presence server's DMR ID for ARS registration ([dmr-data.md](dmr-data.md) 6); 0 = ARS off | `ds_ars_server` | ARS in the build (`CONFIG_DMR_ARS`) |
| 0x0002 | IP gateway | id | The DMR ID of the radio that takes the host's off-net IP packets ([usb-network.md](usb-network.md) 3.1); 0 = none | `ds_ip_gateway` | IP gateway in the build (`CONFIG_DMR_IP_GATEWAY`) |
| 0x0003 | Call end tone | bool | Off silences the tone when a DMR call heard ends (Settings > Sound) | `sound_flags` bit 4 | DMR and tones in the build (`CONFIG_DMR`, `CONFIG_BEEP`) |
| 0x0004 | TOT warning tone | bool | Off silences the tone before the transmit time-out | `sound_flags` bit 5 | Tones in the build (`CONFIG_BEEP`) |
| 0x0005 | Battery tone | bool | Off silences the low and critical battery tones | `sound_flags` bit 6 | Tones in the build (`CONFIG_BEEP`) |
| 0x0006 | TX denied tone | bool | Off silences the tone when PTT is refused or the time-out cuts a transmission | `sound_flags` bit 7 | Tones in the build (`CONFIG_BEEP`) |

**Radio's own copy** is the setting's field in the settings store ([settings-store.md](settings-store.md) 3). Every
scalar key has one (3.2).

**Requires** is what a radio needs for the key to do anything: build switches (`CONFIG_*`) and board capabilities
(`CAP_*`), generic wording, no radio names. It is the seed of a per-radio support registry: a tool will derive from it
which keys a radio supports, so a row's requires is written for that, not for a reader's guess. A key a radio doesn't
support is still kept and carried (4).

A key is listed in four places: this table, `CODEPLUG_KEY_*` in `core/codeplug/codeplug_kv.h`, a row of a firmware
table (5.1) and a row of the Codeplug Editor's `KEYS` (7). `tools/check_keys.py` checks that they agree: the same
keys, each once, of the same type.

### 3.1 Types

A type names how a value is encoded, never how wide it is. A value's length (2.2) is the width.

| Type | Encoding |
|---|---|
| uint | Unsigned, little endian, 1 to 8 bytes. A shorter value zero-extends when read, so a setting that outgrows its width (u8 to u16 to u32) is stored in more bytes under the same key, and an older reader still reads a value that fits its width |
| int | Signed, little endian, 1 to 8 bytes. A shorter value sign-extends from its stored length |
| bool | A uint: 0 = off, anything else on; a writer stores 0 or 1 |
| enum | A uint. Values are only appended: a value never changes meaning |
| id | A uint DMR ID, at most 16776415 (`CODEPLUG_DMR_ID_MAX`). 0 is a value (the setting off), not absent |
| string | The bytes of the value, no terminator, length from the entry |
| record | Fixed fields in a fixed order, each of a stated size, for settings that are only valid together (3.2). It grows by appending fields (4) |
| record list | A key family (below): one key per record, each record's value is of the type its row names |

uint, int, bool, enum and id are the **scalars**. A scalar is one number, so it grows only in width, never by
appended fields.

**A value the reader can't take reads as if the key were absent.** That is an empty scalar, one longer than 8
bytes, a number wider than the reader's field, a number outside the range its row gives, an enum value the reader
doesn't know, and an ID above 16776415. The radio then keeps its own setting (5). Nothing is clamped: a tool that
writes 200 where a radio takes 1 to 10 hasn't set 10.

A type is never redefined, and a new meaning gets a new key.

A new key is added to this table in the same change that first writes it.

### 3.2 Giving a setting a key

- **One key per setting.** Settings that are independent get a key each, so a codeplug can carry one without the
  others, absent works per setting, and the Codeplug Editor shows each as a field.
- **A record for settings that are only valid together.** If taking one from the codeplug and the other from the
  radio's own copy could give a combination nobody set (a target ID with the other's "is a radio" flag), they are one
  key of type record, present or absent as a whole.
- **Every scalar and record key has the radio's own copy** in the settings store, and the key holds over it (5). A
  setting never lives only in the area: with another firmware's codeplug the area reads empty and can't be written,
  and the setting must still work and still be set from the menu. A key family (below) is the exception: a list is
  codeplug data and has no second copy.
- **Numbers are taken in order.** A scalar or record key gets the next free number from 0x0001 up. Related keys
  need no block of their own: a reader walks every entry whatever the numbers (5), so neighbors save nothing. The
  number is settled when the change merges; two open changes that took the same number renumber the later one.

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
- **Values only grow.** A scalar grows in width (3.1). A record grows by appended fields: a reader takes the fields
  it knows from the front and ignores the rest, and a record shorter than a reader expects gives the default for the
  missing fields. A field's meaning never changes. A new meaning gets a new key.
- **A scalar is written whole**, in the width the writer stores. **A record is written from the front**: a writer
  that knows fewer fields than the stored value holds sets its fields and keeps the bytes after them
  (`CODEPLUG_KV_HEAD`), so an older tool doesn't cut off a later version's fields.
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
| `codeplug_kv_uint`, `codeplug_kv_int`, `codeplug_kv_id` | A scalar from a value already read (3.1); false for one the reader can't take |
| `codeplug_kv_write(ops, n)` | Applies `n` sets and removes (`len` = `CODEPLUG_KV_REMOVE`) in order, then one journaled rewrite through the shared sector buffer. `len \| CODEPLUG_KV_HEAD` sets a record from the front (4). All or nothing. Moves `codeplug_kv_serial()` when a byte changed, not `codeplug_serial()`, so a key written doesn't reload the whole codeplug; a batch that changes nothing writes nothing |
| `codeplug_kv_set(key, value, len)`, `codeplug_kv_remove(key)` | One op through `codeplug_kv_write` |
| `codeplug_kv_commit(addr, sector)` | A raw sector write at flash address `addr` under the transfer rule (6): the sector is written unless the radio's must stay (`codeplug_kv_keep`), which counts as written |
| `codeplug_kv_serial()` | Moves whenever the keys may read differently: a key written, and every change of `codeplug_serial()` (a CPS commit, a TFTP write, another firmware's codeplug found, accepted or replaced). The one thing an owner of keyed settings watches |

Reads keep nothing of the area in RAM. A cache of the checked state was left out: it would cost RAM on every radio,
every path that writes the sector raw (CPS shim, fast protocol, TFTP) and every change of the foreign-codeplug state
would have to drop it, and a write made outside the running firmware (the stub, the bootloader) can't. A walk gives
the same saving for keys read together.

The writes need no built-in codeplug to be stored first. The area is apart from the channel records, so it
doesn't materialize the built-in set ([codeplug-native.md](codeplug-native.md) 2). It fails while a CPS session holds
the shared sector buffer, or when the storage region isn't ours ([settings-store.md](settings-store.md) 5).

### 5.1 Tables of keyed settings

Scalar keys are applied through tables (`core/codeplug/codeplug_keys.c`). An owner of settings has one table, a row
per key: the key, its type, the setting's range, whether the build has it, and the offset and size of a field in the
owner's settings struct, so a struct has one table. The owner is whatever holds that struct: the radio core holds
the data service's settings (`radio_keys` in `core/radio/radio_keys.h`) and the Settings > Sound tones
(`radio_tone_rows`, next to it), and a block of UI settings would be the UI's, in `core/ui/`. There is no table of all
keys: the radio core doesn't reach into the UI's settings, nor the other way.

| Call | |
|---|---|
| `codeplug_keys_apply(rows, n, &settings)` | One walk: every key that is present and holds a value the setting takes (3.1) goes into its field. True when a field changed |
| `codeplug_keys_store(rows, n, &was, &now)` | Writes the key of every field that differs, in one batch. The rule below |
| `codeplug_keys_find`, `_field`, `_set_field`, `_valid` | One row: for a menu row or a debug link field |

An owner uses them the same way everywhere: read its settings, apply the table, and set the settings once if
anything changed. A new scalar key is a row there, a row in section 3 and a row in the Codeplug Editor (7), and no
code.

A setting that isn't a plain field (bits packed into a byte) is unpacked by its owner into a plain struct first. The
tones are: `radio_settings_t.tones_off` holds a bit per tone, set when it is off, and `radio_tones_unpack` /
`radio_tones_pack` turn it into the four bools of `radio_tones_t` and back. A record key is read and written by its
owner with `codeplug_kv_get` and a `CODEPLUG_KV_HEAD` set.

### 5.2 A key and the radio's own copy

- **A key that is present holds the setting.** The owner applies its table at start and whenever
  `codeplug_kv_serial()` moves, and again when the setting changes another way, before it is saved: such a change is
  put back.
- **A key that is absent leaves the setting as the radio has it.** So a codeplug written by a tool that doesn't carry
  the area doesn't change how the radio behaves.
- **The radio's own copy follows the key.** The setting in force is what the settings store saves, so after a key
  is removed the radio keeps the key's last value.
- **Setting a keyed setting on the radio** (the menu, the debug link) is one rule, `codeplug_keys_store`:
  1. The key is written if the area can be written, whether or not it was present. A CPS read then shows the radio's
     setting.
  2. If the area can't be written and the codeplug holds no such key, the setting is saved as the radio's own.
  3. If the area can't be written and the key is present, the change is refused: the key would put the old value back.
- **Another firmware's codeplug** (`codeplug_foreign`, [codeplug-native.md](codeplug-native.md) 2): the area reads
  empty and can't be written. By the rule above every keyed setting still works and can still be set, as the radio's
  own, and the "Old codeplug" question isn't asked for it. Start new erases the area with the rest: keys from before
  the other firmware don't come back in the clean codeplug. Use anyway makes the area live again as it is stored.
- **A factory reset** resets the radio's own copies and leaves the codeplug, so a key that is present still holds.

The ARS server and the IP gateway are set from the menu (Settings > Connections > Data services, [ui.md](ui.md)) and
the debug link's DMR_DATA SETTINGS and IP_SETTINGS ([debuglink.md](debuglink.md) 3.12). The tones are set from
Settings > Sound.

Debug link view `cps`: `cpkv.state` (`valid`, `empty`: no area, `bad`: invalid, `newer`: a later version),
`cpkv.length` (data length) and `cpkv.keys` (entries).

## 6. Transfers

Every path that reads the codeplug reads the area. When a path writes the area's sector, the radio keeps its own
sector if

- its area is valid (2.3) and the sector written isn't, or
- its area is of a newer version (never written, 2.1).

Otherwise the sector goes in as sent. A file from a tool or a firmware that doesn't carry the area holds 0xFF there,
so writing it back never erases the radio's keys. A tool that wants every key gone writes a valid area with no
entries: that is taken.

The radio applies the rule itself (`codeplug_kv_commit`) where the sector is committed, so it holds for any tool,
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

The editor's keys are one table, `KEYS` in `kv.js`: the model's field, the key, the type, the bytes it stores and
the range. Decoding, encoding and validation follow the row. The Radio tab shows each key as a field. A field left
empty is an absent key. Typing a value writes the key; for an ID, 0 turns the setting off.

The editor reads and writes the sector with the other EEPROM regions and keeps entries it doesn't know. On a write
to a radio it sends (`areaForWrite`):

| Case | What goes to the radio |
|---|---|
| The radio's area is of a newer version | The radio's own, untouched |
| The edit changed no key | The radio's own. A file opened before the radio's menu changed a key doesn't put the old value back |
| The edit changed keys and the radio has a valid area | The radio's area with only those keys set or removed. Every other key keeps the radio's value |
| The edit changed keys and the radio has no valid area | The file's area |

## 8. The native codeplug

The native codeplug ([codeplug-native.md](codeplug-native.md) 4) carries these keys, with the same numbers, value
encodings and rules, as one record in this entry format. A key defined here is never renumbered or translated: a
conversion copies the entries as they are. There is one key space for both formats.

## 9. Open

- Whether the open-firmware CPS and Codeplug Studio write this range is not known. If they write erased bytes
  there, the radio keeps its sector (6). A CPS that verifies its write by reading back would then report a mismatch
  there.
- An area of a newer version can't be replaced by an older firmware or tool (2.3, 6), by design. A radio taken
  back to older firmware then runs on its own copies and can still set every setting (5.2), but no tool can push keys
  to it. A way to replace the area on purpose (a tool's "Replace the settings area", after a confirmation, which the
  radio accepts) is not built.
- The header is outside the CRC. A damaged version byte reads as an invalid area (version 0) or as a newer one.
- Per-channel settings: a key whose value starts with a channel number, or a table key. Not needed yet.
- No key is a record yet. The firmware's set from the front (`CODEPLUG_KV_HEAD`) is built and tested; the Codeplug
  Editor's comes with the first record key.
