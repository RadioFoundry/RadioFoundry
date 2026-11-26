<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Contact lists

Named groups of contacts, and the rule that decides which of them name a received call. A user on several networks
(a DMR network, a club net, a county P25 system) gets the right names even when their IDs overlap. Each system selects
the lists to read, and the radio names a call through the system of the channel it is on.

The contacts themselves don't move: the DMR contact table ([open-firmware-cps.md](open-firmware-cps.md) 7.3.5) and the
air mode contact table ([air-contacts.md](air-contacts.md)) keep their layout, so every tool that reads them still does.
A list is a name, and a contact belongs to a list by a byte kept beside the tables, in the codeplug settings area's
continuation ([codeplug-settings.md](codeplug-settings.md) 2.4).

Code: `core/codeplug/codeplug_contact_lists.c` (keys, membership, the edit rule), the lookup entry point
`codeplug_name_lookup`. Codeplug Editor: `site/static/js/codeplug/contactlists.js`. Switch: `CONFIG_CONTACT_LISTS`
([build-config.md](../build-config.md)).

## 1. Concepts

| Term | Meaning |
|---|---|
| Contact list | A named set of contacts. At most 32, numbered 0-31. A list exists while its name key (3) is present and not empty |
| Membership | The list a contact is in: one list or none. A contact is never in two |
| ID database | A built-in list, number 0xFE: the DMR ID database ([iddb-compact.md](iddb-compact.md)). Always present, read-only, selectable like any list |
| Selection | A system's two list choices: primary and secondary, searched in that order |
| Network | The conventional system type ([channel-modes.md](channel-modes.md) 7.5) a plain channel points at. Shown as "network" in the editor and on the radio |

List bytes, used for membership and selection:

| Byte | Meaning |
|---|---|
| 0-31 | The list with that number |
| 0xFE | The ID database |
| 0xFF | None: no contact list |
| anything else | Reads as 0xFF. A writer never stores it |

A contact is in list 0xFF when no list holds it. The ID database holds no contacts of the radio's tables, so 0xFE is
never a membership byte; a writer never stores it there, and a reader takes it as 0xFF.

Rules that follow from the model:

- Many systems may select one list. A system selects at most two.
- A system with no selection, or whose lists name nothing the call matches, gives no name: the radio shows the raw ID.
- A channel with no system (5.1) gives no names.
- A list that no contact is in is valid. It names nothing until contacts join it.
- A selection that names a list that doesn't exist skips that list.

## 2. Proposed storage location

**Proposed (pending maintainer review).** The keys of section 3 live in the codeplug settings area's first
continuation, EEPROM image 0xF000-0xFFFF ([codeplug-settings.md](codeplug-settings.md) 2.4), not in the main sector.

| Choice | Why |
|---|---|
| A continuation of the settings area | The format and the transfer rule exist already. A list edit is one journaled sector rewrite: atomic, and the main sector (0xE000) loses no room |
| The area stays one key space | A key family or table is an ordinary key: absent, kept, grown by the rules of codeplug-settings.md 4 |
| 0xF000, not 0xB000 | 0xB000-0xBFFF stays free for growth of the open-firmware layout. qdmr stops at 0xB000, so it never touches 0xF000 |

A radio or tool that predates the continuation reads no list key. It then behaves as before (all contacts, then the ID
database), and any tool that keeps the bytes it doesn't decode keeps the lists.

## 3. Keys

All keys are in the continuation and nowhere else ([codeplug-settings.md](codeplug-settings.md) 2.4). Values are little endian where numeric.

| Key | Name | Value | Size |
|---|---|---|---|
| 0x1E00 + n, n = 0-31 | List name | The name of list n: string, 1-16 bytes, UTF-8, no terminator. Absent or empty: list n doesn't exist | 1-16 per list |
| 0x1F00 + s, s = 0-254 | System lists | System s's selection: byte 0 the primary list byte, byte 1 the secondary | 2 per selecting system |
| 0x2000 | DMR membership | One list byte for each DMR contact slot: byte i is slot i + 1 (1-1024) | up to 1024 |
| 0x2001 | P25 membership | One list byte for each air mode contact slot: byte i is slot i + 1 (1-341) | up to 341 |
| 0x2002 | Channel network | One byte for each channel: byte i is channel i + 1 (1-1024), the number of its network system, or 0xFF for none | up to 1024 |
| 0x2003 | Contact table check | u32: the check of the contact tables (3.3) | 4 |

The ranges 0x1E00-0x1EFF, 0x1F00-0x1FFF and 0x2000-0x20FF are reserved whole (the unused numbers are for later list
features). Numbers 0x1E20-0x1EFF and 0x2004-0x20FF are unassigned.

### 3.1 Encodings

- **List name.** A name of more than 16 bytes, or with a byte below 0x20, makes the list not exist. Two lists may have
  the same name; the editor warns.
- **System lists.** A value of one byte is the primary list only (the secondary reads 0xFF). A value of two bytes or
  more is read from the front; later bytes are kept and ignored. The key is absent for a system with no selection.
  The primary and secondary may name the same list; the second search then finds nothing new.
- **Membership.** A value shorter than the table reads 0xFF for the missing bytes. A writer may leave off trailing
  0xFF bytes and removes the key when every byte is 0xFF. A value longer than the table is read from the front.
- **Channel network.** The same shortening rules. A byte names a system by number ([channel-modes.md](channel-modes.md) 7.1): the record
  must exist and be of the Conventional type ([channel-modes.md](channel-modes.md) 7.5). A byte that names anything else reads as 0xFF.

### 3.2 Size

Entries cost 4 bytes of framing (codeplug-settings.md 2.2) plus the value. The worst case is 32 names of 16 bytes,
255 selections, and full membership and channel tables: 32 x 20 + 255 x 6 + 4 x 4 + 1024 + 341 + 1024 + 4 = 4563
bytes, more than the 4080 bytes of one sector's entries. Trailing 0xFF bytes are not stored, so a codeplug that uses
a part of each table fits with room to spare. A write that doesn't fit changes nothing (codeplug-settings.md 4). A
second continuation sector would take the overflow ([codeplug-settings.md](codeplug-settings.md) 2.4); nothing here needs one yet.

### 3.3 Contact table check

If a tool other than the radio or the Codeplug Editor rewrites, reorders or deletes contacts, a membership byte points
at the wrong contact. The check lets the radio see that.

The check is a CRC-32 (IEEE 802.3, reflected, as `storage_crc32` and the settings area's own CRC) over one byte stream:

1. For DMR contact slot 1 to 1024, in order: the 5 bytes at offset 0x10 of the slot (the ID, 4 bytes as stored, then
   the call type byte), as [open-firmware-cps.md](open-firmware-cps.md) 7.3.5.
2. For air mode contact slot 1 to 341, in order: the first 5 bytes of the slot (mode, call type and ID, as
   [air-contacts.md](air-contacts.md) 3).

Empty and invalid slots are included as stored. Names and every other byte are left out, so a rename never voids
membership. The stream is 5 x (1024 + 341) = 6825 bytes. A build without an air mode still reads the air slots for the
check, and never names from them.

**The void rule.** Membership is **void** when a membership key (0x2000 or 0x2001) is present and the check key is
absent, not 4 bytes, or not equal to the check computed from the tables as stored. Void membership reads as no
contact being in a list (every contact is list 0xFF). Names come only from the ID database then, if a selection names
it. Nothing else changes: the lists, the selections and the channel network stay. A codeplug with no membership key has
nothing to void. The channel network and the selections don't depend on the tables, so they aren't void.

The radio doesn't repair a void membership. It leaves the keys as found, and a radio-side contact edit while void
leaves them alone too (4), so it never confirms a wrong assignment. Only the Codeplug Editor, which sees the contacts
and the lists together, writes a new check: it flags the codeplug, and the user assigns the contacts again.

## 4. Edit rules

Every write of a contact by the radio (`codeplug_set_contact`, `codeplug_delete_contact`, and the contacts a zone
import adds) goes through one function that changes the tables, the membership and the check together:

| Edit | Membership | Check |
|---|---|---|
| A contact is set in an empty slot | 0xFF, unless the edit names a list | Recomputed |
| A contact is set in a used slot (an edit of it) | Kept, unless the edit names a list | Recomputed |
| A contact is deleted | The slot's byte becomes 0xFF | Recomputed |
| A zone import adds contacts | 0xFF for each new contact; a contact found by name and ID is kept as it is | Recomputed once |
| An edit while membership is void (3.3) | Not changed | Not changed |

- The contact record is written first, then the keys in one batch (one journaled sector rewrite, codeplug-settings.md
  4). A power cut between the two leaves a check that doesn't match: the membership is void and names disappear. They
  are never wrong.
- When the area can't be written (another firmware's codeplug, a newer area, a CPS session in progress) the contact edit
  proceeds as before and membership is left, which voids it if a check exists. The radio edits no list name, selection
  or channel network in this version: those come from the Codeplug Editor.
- The air mode contact table isn't written by the radio yet (air-contacts.md 5). When it is, its edits use the same
  function.
- The Codeplug Editor writes membership and the check together on every save that has contacts and lists. Deleting or
  reordering contacts there moves membership with them.

## 5. Lookup

The radio names a received call (the caller and the talk group on the display) through one entry point,
`codeplug_name_lookup(selection, mode, id, group, out)`.

### 5.1 The selection

| Channel | Selection |
|---|---|
| Has a tag that names a system (a trunked channel, an AES channel, an NXDN one) | That system's lists (key 0x1F00 + system) |
| A plain channel (DMR or P25 conventional) with a network in the channel network key | The lists of that network system |
| A plain channel with no network, a network of another type, or a missing one | None: no names |
| No channel record (the VFO) | None: no names |

A tag's system wins over the network key. The channel network key is read for channels without a tagged system only.

### 5.2 Walk

1. Take the primary list byte, then the secondary. A byte of 0xFF, or a list that doesn't exist, is skipped.
2. For a list number 0-31: search the contacts of the call's table (the DMR contact table for a DMR call, the air mode
   table for the call's mode) whose membership byte equals the number, by call type (group or private/unit) and ID.
   The first match in slot order names the call. Void membership (3.3) matches nothing.
3. For 0xFE: ask the DMR ID database. It answers for a DMR private ID only. For a DMR group (talk group), an all call
   and every P25 ID it has no answer, and the walk goes on.
4. The first list that names the call wins. If none does, the raw ID is shown.

The ID database as the primary list therefore names a private caller by callsign before any contact of the secondary
list. A user who wants a contact's name to win puts the contact list first.

### 5.3 Scope

Lists scope the names of received calls only: the caller and the talk group on the display, in DMR (including
Tier III) and P25. The radio's contact browser, the dial picker and find still show every contact in both tables, in
every list and none. A contact in no list is still dialed and found, and it names no call.

### 5.4 Caches

An answer is cached while the channel's selection is the same. A change of channel to one with another selection, a
change of the codeplug and a change of the keys drops the cached answers, so a name never carries over from one
network to another.

## 6. Feature switch

`CONFIG_CONTACT_LISTS` ([build-config.md](../build-config.md)). Off:

- A lookup is as before: the first contact of the table with the ID and call type, then the ID database.
- The keys stay in the codeplug and the Codeplug Editor still edits them. The radio ignores them.
- The Conventional system type isn't built: a channel's network is ignored, and a system of that type is kept as any
  type the build doesn't know.

On, with a codeplug that has no list key, nothing is named: no selection, no names. The Codeplug Editor offers a
migration on opening such a codeplug: a Default network and list holding every contact, with every plain channel
assigned to it.

## 7. Open

- The storage location (2) is proposed, pending maintainer review.
- Applying a network's own ID to conventional transmit is not specified. It is a follow-up.
- Zone sharing carries a channel by value ([zone-share.md](zone-share.md)): a received channel has no network, and the
  receiver assigns one.
- The VFO has no selection (5.1). Whether it should take the last channel's is open.
- The radio can't edit lists, selections or the channel network. A menu for them is not specified.
