<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Air mode contacts

A contact table for the digital modes beside DMR (P25 now; NXDN can join it later), inside the open-firmware codeplug
image ([open-firmware-cps.md](open-firmware-cps.md) 7.2). It names the talk groups and units of those modes on the
screens.

## 1. Why a table of its own

The contact record ([open-firmware-cps.md](open-firmware-cps.md) 7.3.5) has a name, an ID and a call type, but no mode.
DMR talk group numbers and P25 talk group numbers come from different systems and overlap. When P25 took its names
from the contact list, a DMR contact numbered 1310 labeled P25 talk group 1310 with the DMR group's name. Each entry in
this table carries its mode, so an ID is named only by an entry of its own mode.

The contact list holds a P25 or NXDN channel's talk target ([p25-codeplug.md](p25-codeplug.md) 2,
[nxdn-codeplug.md](nxdn-codeplug.md) 2), unless a P25 channel says its contacts are P25 contacts (4). Names come only
from this table. A P25 ID without an entry shows as a number, even when a contact has the same number.

## 2. Where

| | |
|---|---|
| EEPROM image | 0xC000-0xDFFF: two 4 KB sectors (`STORAGE_SECTOR`) |
| Entries | 341, 24 bytes each, slot n at 0xC000 + 24 x (n - 1), n = 1-341. The last 8 bytes (0xDFF8-0xDFFF) are unused, kept as found |
| On the wire | as the EEPROM image ([open-firmware-cps.md](open-firmware-cps.md) 7.1): the same address on the STM32 mapping |

Why there:

- The open-firmware layout ends with the zone table at 0xAEF0. [codeplug-settings.md](codeplug-settings.md) 1 leaves
  0xB000-0xDFFF for that layout to grow. This table takes the top 8 KB and leaves 0xB000-0xBFFF for that growth.
- qdmr transfers the EEPROM image up to 0xB000 only, so it never reads or writes the table. The Codeplug Editor
  transfers it as one more region.
- The firmware reads the table only from a RadioFoundry codeplug: on a foreign one (`codeplug_foreign()`) it reads as
  empty and isn't written, as the settings area.
- Start new erases both sectors, as it erases the settings area.

## 3. Entry

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Mode: the kind of the mode's conventional channel ([channel-modes.md](channel-modes.md) 4). 2 = P25; P25 trunked channels (kind 3) use the same entries. 0xFF = empty slot |
| 1 | 1 | Bits 0-1 call type: 0 unit, 1 group, 2 all call, 3 reserved. Bits 2-7 reserved: written 0, kept as found |
| 2 | 3 | ID, little endian. P25: group 1-65535, unit 1-0xFFFFFE. Ignored for all call |
| 5 | 3 | Reserved: written 0xFF, kept as found |
| 8 | 16 | Name, 0xFF padded as a contact's name. A first byte of 0xFF or 0x00 is an entry without a name: it names nothing |

The call types follow the target records of [channel-modes.md](channel-modes.md) 5.2, not the contact record's order.
A P25 all call entry names talk group 0xFFFF.

### 3.1 Validity

An entry is read for a mode when its mode byte is that mode, its call type is 0-2 and its ID is in the mode's range.
Anything else is skipped and kept byte for byte by every writer, so a later mode or field never breaks an older
reader. An erased sector is an empty table.

### 3.2 Lookup

An ID is named by the first valid entry, in slot order, of the same mode and call type (group or unit) with that ID.
A later duplicate is never used; the Codeplug Editor warns about one. Reading the table is a scan of up to 8 KB, so
the radio caches each answer, an empty one included, until the codeplug changes.

With `CONFIG_CONTACT_LISTS` and a codeplug that holds a contact list key, the lookup is scoped: an entry names a call
only when its list is one the current channel's system selects, searched in order ([contact-lists.md](contact-lists.md)
5). A codeplug with no such key keeps the rule above (contact-lists.md 6). Membership is a byte for each slot kept
outside the table, and the table's slot order and the rule above are unchanged. Without the switch the lookup is as
above.

## 4. Talk targets

A P25 channel whose parameter block has the P25 contacts bit set ([p25-codeplug.md](p25-codeplug.md) 2.4, 4) takes its
talk target from this table: the record's 16-bit contact and the entries of its RX group list are slots 1-341, read as
valid entries of mode 2 (3.1). A group entry is that talk group, a unit entry a unit call, an all call entry talk group
65535. An empty, invalid or other-mode slot is no contact (talk group 1), and in the RX group list it is skipped. On a
trunked channel the list's group entries are also the groups followed. The firmware reads a slot per use, not through
the lookup cache of 3.2. A channel without the bit uses the DMR contact list as before, and a radio never sets the bit.
The radio shows a flagged channel's target but doesn't edit the slots yet, and zone sharing sends the target by value as
a DMR contact, with the bit clear ([zone-share.md](zone-share.md) 4.2).
A firmware that predates this reads the slot number as a DMR contact index; the radio says it has the feature by the
`p25contacts` token of `features=` ([debuglink.md](debuglink.md) 3.1).

## 5. Writers

- The Codeplug Editor edits the table (its P25 contacts) and writes only the records it changed.
- The radio doesn't write the table yet. When it does, an edit is one sector rewrite through the journal
  ([settings-store.md](settings-store.md) 4).
- A tool that doesn't know the table keeps its bytes, as for any part of the image it doesn't decode.

## 6. Code

- Radio: `core/codeplug/codeplug_air_contacts.c` (read, find), built with any air mode (`CONFIG_P25`, `CONFIG_NXDN`).
  The talk target and followed groups of a flagged P25 channel: `core/p25/p25_codeplug.c`.
- Codeplug Editor: `site/static/js/codeplug/records.js` (encode and decode), `model.js` (the list), `image.js` (the
  region).
