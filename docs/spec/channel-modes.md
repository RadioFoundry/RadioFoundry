<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Channel modes

The open-firmware channel record ([open-firmware-cps.md](open-firmware-cps.md)) says FM or DMR. Every other digital
mode, and every channel that needs more than that record holds (a trunked channel, a channel on a system), is stored
as an ordinary record plus keyed records in the codeplug settings area ([codeplug-settings.md](codeplug-settings.md)).
This spec is the generic layer: where each kind of data lives, how a mode claims its part, and the rules every reader
and writer keeps. It defines no mode's fields; a mode's own layout is in its own spec, linked from the registry (4).

What a mode needs, by category, and where it lives:

| Need | Home | Section |
|---|---|---|
| Channel parameters (access code, mode options, per-channel ID override, a fixed per-channel target) | The channel's tag: its parameter block | 2 |
| Radio-wide identity and options of a mode (unit ID, call sign, defaults) | The kind's radio-wide record | 5.5 |
| A per-system identity | The system record: own ID, the type's block | 7 |
| Call targets the contact record holds (DMR-sized numeric group and individual IDs) | The contact list, as for DMR | 5.2 |
| Call targets it can't hold (call signs, other text addresses, wider IDs, reflector or gateway names) | Targets | 5.2 |
| Receive group lists of such targets | Target lists | 5.3 |
| Systems, sites, control channels, channel plans | System records and system entries | 7 |
| Listen only (never register or transmit on a system) | The system record's flags | 7.1 |
| A system's own secret (authentication) | System secret | 7.4 |
| Traffic keys, several per system, chosen by algorithm and key ID | Traffic keys | 5.4 |
| Per zone | Reserved (5.6) | 5.6 |

## 1. Words

| Word | Meaning |
|---|---|
| Kind | What a channel is beyond FM or DMR, one byte. 0 = a plain channel (no tag) |
| System type | What a system record is, one byte |
| Tag | The keyed record that makes a channel one of a kind (2) |
| Base record | The channel's open-firmware record, which every reader sees |
| Mode module | The code (firmware and editor) that runs one kind or system type. The generic layer never decodes its bytes |
| Built | The firmware runs the kind: its mode module is in the build |
| Check | A 16-bit name check (2.2) a reference keeps, so a record replaced or renamed under it doesn't resolve |

## 2. Channel tags

Key 0x0400 + channel - 1, one per channel number (1-1024).

### 2.1 Layout

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Kind (4). 0 and 0xFF are not kinds: a value starting with either is no tag |
| 1 | 1 | System (7): record n of the system family, 0xFF = none |
| 2 | 2 | Check of the channel's name (2.2), little endian |
| 4 | 1 | Flags (below) |
| 5 | 2 | With flag bit 2 only: fallback channel number, 0 = none |
| 7 | 2 | With flag bit 2 only: check of the fallback channel's name |
| 5 or 9 | rest | The kind's parameter block: its own layout, grown by appending |

| Flag bit | Meaning |
|---|---|
| 0 | Transmit allowed. Clear: receive only, so a zeroed tag never transmits |
| 1 | The base record stands on its own (a channel whose record is a real FM or DMR channel, such as a mixed one). Clear: the base record is a safe stand-in (2.3) |
| 2 | A fallback channel follows the header. Clear: the system's fallback, if any |
| 3 | Reserved: written 0, kept as found |
| 4-7 | The kind's own |

The firmware writes parameter blocks of up to 64 bytes and reads and keeps any length. A short block reads with its
missing fields at their zero value, so every field is chosen so that zero is the safe default.

### 2.2 Name check

FNV-1a, 32 bit (offset basis 2166136261, prime 16777619), over the 16 octets of the name as a channel record stores
it, every octet from the first 0x00 or 0xFF on taken as 0x00. The result's halves XORed: `(h ^ (h >> 16)) & 0xFFFF`.

| Name | Check |
|---|---|
| (empty) | 0x706C |
| `A` | 0x18A9 |
| `T3 Ops` | 0x1687 |
| `Sixteen chars!!!` | 0xCDD2 |

### 2.3 Live tags and the base record

A tag is **live** when its check equals the check of the channel's name and either flag bit 1 is set or the base
record is DMR and receive only. Only a live tag counts; any other is **stale**: kept as found, never applied.

The base record of a tagged channel without flag bit 1 is a **safe stand-in**: DMR, receive only, the frequencies of
the channel. A reader that knows no tags (an older firmware, another tool) sees a receive-only DMR channel and never
transmits on it. A tool that edits the record (renames it, makes it FM or allows TX) makes the tag stale, so the
channel is a plain one from then on, never a mix of the two.

The radio takes from a live tag: the kind, the system, and transmit allowed (flag bit 0) in place of the record's
receive-only bit.

### 2.4 Writes

A writer stores the base record first, then the tag, so a cut between the two leaves a harmless record. When a channel
at a number is written, the tag there:

| Case | Tag |
|---|---|
| No channel was stored at the number | Removed: a new channel starts plain |
| The channel is of the tag's kind and the tag was live for the old name | Follows: the check of the new name, the system, flag bit 0 from the channel. The rest is kept byte for byte |
| The tag's check is the old or the new name's | Removed: the channel was made plain, or a plain channel would make it live |
| Otherwise | Kept as found (stale) |

A channel of a kind is always written in its safe stand-in shape unless its tag has flag bit 1, also where no tag
follows it (a copy to another number is a receive-only DMR channel). A copy made on the radio (the channel editor's
Save as new, Copy here) then takes the source's tag: the copy's name check, system and flag bit 0, the fallback and
the parameter block byte for byte (`codeplug_tag_copy`). Where the source's tag has flag bit 1, the copy's record
first takes the source record's mode, receive-only bit and bandwidth (`codeplug_base_copy`), so the copy is the same
channel on both sides; if that write fails, the copy's tag gets flag bit 1 clear, which is what its record then is.
Deleting a channel removes its tag.

Where the tag that follows has flag bit 1, the base record is a channel of its own and the radio shows the kind's
mode, receive-only bit and bandwidth in place of the record's: a write from the radio keeps those three as stored.

A kind's module that changes a channel's block on the radio writes the record, lets the tag follow, then sets the
front of the block in place (`codeplug_tag_rewrite`): the system and flag bit 0 from the writer; the fallback, the
other flags and the block's bytes after the ones written stay as stored. With no tag of the kind there, it writes a
new one.

## 3. Key families

All are record lists ([codeplug-settings.md](codeplug-settings.md) 3), reserved as whole ranges here so a later mode
never renumbers:

| Range | Family | Record n | Section |
|---|---|---|---|
| 0x0100-0x01FF | Systems | system n | 7.1 |
| 0x0200-0x02FF | System entries (sites, channels, plan rows) | entry n | 7.2 |
| 0x0300-0x03FF | System secrets | system n's | 7.4 |
| 0x0400-0x07FF | Channel tags | channel n + 1 | 2 |
| 0x0800-0x09FF | Reserved for DMR Tier III's own records | | |
| 0x0A00-0x0AFF | A kind's radio-wide record | kind n | 5.5 |
| 0x1000-0x17FF | Targets | target n | 5.2 |
| 0x1800-0x18FF | Target lists | list n | 5.3 |
| 0x1900-0x1CFF | Traffic keys | entry n | 5.4 |
| 0x1D00-0x1DFF | Reserved: per-zone records | zone n | 5.6 |

Every family keeps unknown bytes: a writer that changes a record it doesn't fully know changes only the fields it
knows and keeps the rest, and a record of a kind or type it doesn't know is kept whole.

## 4. The registry

One list of kinds and one of system types. The firmware reads `core/codeplug/codeplug_modes.h`, the Codeplug Editor
`site/static/js/codeplug/modes.js`; `tools/check_modes.py` checks that both and the tables below hold the same rows.
Ids are only appended and never reused. A kind and a system type that belong together share the id.

| Kind | Name | Short | Built when | Spec |
|---|---|---|---|---|
| 1 | DMR Tier III trunked | T3 | `CONFIG_DMR_TRUNK` | [dmr-trunking-codeplug.md](dmr-trunking-codeplug.md) |
| 2 | P25 conventional | P25 | `CONFIG_P25` and a board whose air interface receives P25 (`radio_p25_fits`) | [p25-codeplug.md](p25-codeplug.md) |
| 3 | P25 trunked | P25T | `CONFIG_P25_TRUNK` and a board whose air interface receives P25; receive, and transmit where the system allows it ([p25-trunking.md](p25-trunking.md) 5, 7) | [p25-codeplug.md](p25-codeplug.md) |
| 4 | NXDN conventional | NXDN | `CONFIG_NXDN` and a board whose air interface carries NXDN (`radio_nxdn_fits`) | [nxdn-codeplug.md](nxdn-codeplug.md) |
| 5 | Conventional DMR AES-256 | AES | `CONFIG_DMR_AES` | See below |

| System type | Name | Short | Built when | Spec |
|---|---|---|---|---|
| 1 | DMR Tier III system | T3 | `CONFIG_DMR_TRUNK` | [dmr-trunking-codeplug.md](dmr-trunking-codeplug.md) |
| 2 | Reserved: P25 conventional system (identity and keys) | | | |
| 3 | P25 trunked system | P25T | `CONFIG_P25_TRUNK` | [p25-codeplug.md](p25-codeplug.md) |
| 4 | NXDN conventional system | NXDN | `CONFIG_NXDN` | [nxdn-codeplug.md](nxdn-codeplug.md) |
| 5 | Conventional DMR AES-256 | AES | `CONFIG_DMR_AES` | See below |

A conventional AES channel uses the normal DMR frequencies, contact, color code and timeslot. Its base record is receive-only.
The type 5 system block starts with one identity byte. Provision each traffic key in the traffic-key family with its
system index, algorithm 0x25, an eight-bit PI key ID in the low byte of the ID field, and exactly 32 key bytes.
The channel tag parameter block starts with algorithm 0x25 and the PI key ID. Missing keys inhibit PTT. AES applies
to voice. The editor keeps them in the system's traffic key table (`aesSystem.traffic`, masked until shown, with
**Generate** filling a key from `crypto.getRandomValues()`) and omits traffic keys when saving an image without keys. On the radio the AES key row cycles
through provisioned system number/key ID pairs and None. Save applies the selection.

A mode claims an id by adding a row to all three in one change, with its spec: the parameter block, the type's
system block and channel tail, its targets' address format, its traffic keys' algorithm ids and its radio-wide
record. The order of work is in [Adding a mode](../dev/adding-a-mode.md).

## 5. Mode-scoped records

### 5.1 References

A record that points at a record of a family stores 4 octets: the record's number + 1 (0 = none) and the check (2.2)
of its name, both little endian. A reference resolves only while the record there has that name and the expected
kind; a record deleted, renamed or replaced under it reads as none. Nothing cascades: each reference is checked when
read.

### 5.2 Targets

A call target a mode needs that the contact record can't hold: a call sign, a reflector or gateway name, a text
address, an ID wider than 24 bits.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Kind (4) |
| 1 | 1 | Flags: bits 0-1 call type (0 individual, 1 group, 2 all, 3 other), bits 2-3 reserved, bits 4-7 the kind's |
| 2 | 16 | Name, as a channel record's (0xFF or NUL padded) |
| 18 | rest | Address: the kind's encoding |

**Where a target lives.** A mode whose targets fit the contact record (a numeric group or individual ID up to 24 bits
with a call type) uses the contact list as DMR does, and its channels point at contacts. A mode with other addresses
uses this family. A target that belongs to one channel only and is never picked from a list (a repeater's own call
sign on that channel) lives in the channel's parameter block. A mode spec says which of the three each of its
addresses uses.

**Editor.** The contact picker of a channel asks the kind's module which list it offers: contacts, its targets, or
both. A target's address is edited by the kind's module; the name and call type are generic fields.

**Sharing.** Zone sharing does not carry targets yet: no kind in this tree refers to one. When one does, a shared
channel is to carry every target its parameter block refers to: name, kind, flags and address; the receiver uses a
target of the same kind and address it has, else adds one, and rewrites the reference. Targets are never secret.

### 5.3 Target lists

A receive group list of targets: the target header (offsets 0-17, call type bits unused) followed by references (5.1)
to targets, 4 octets each. A reference that no longer resolves is skipped. Contacts are listed in the contact
record's own RX group lists, never here.

### 5.4 Traffic keys

Keys that encrypt traffic, any number per system, each chosen by algorithm and key ID.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | System (7): every key belongs to one |
| 1 | 1 | Algorithm id: the system type's own numbering |
| 2 | 2 | Key ID, little endian |
| 4 | 1 | Flags: bits 0-3 reserved, 4-7 the type's |
| 5 | rest | The key's octets |

A conventional channel that uses keys names a system in its tag (2.1), of a type that holds its identity and keys;
such a system needs no system channels. The channel's parameter block holds the algorithm id and key ID it selects,
so several channels on one system use different keys, and a mode that changes key per call looks the key up by the
key ID the call carries. A key belongs to a system so that one rule covers all secrets (7.4).

### 5.5 A kind's radio-wide record

Key 0x0A00 + kind: one record of the kind's own layout, grown by appending, for its radio-wide identity (unit ID,
call sign) and options. Absent = the kind's defaults. A per-system identity is in the system record (7), a
per-channel one in the parameter block; the narrowest one set counts.

### 5.6 Per zone (reserved)

Key 0x1D00 + zone index is reserved for a record that applies a mode's settings to a zone. No layout is defined; a
writer keeps one it finds.

## 6. Radio behavior

A channel of a kind the radio doesn't run (any kind not built, a built one the board can't carry, and ids it doesn't
know) is **parked**: the radio
tunes its receive frequency with the DMR modem off, the squelch never opens, PTT is refused as a mode not in this
build, scan and dual watch skip it, roaming doesn't take it as a repeater and it can't be copied to the VFO (the VFO
holds plain FM and DMR only). Its mode word is the kind's short name (`?` for an unknown id), and Home says the mode
is not in this build. A built kind's mode module takes the channel instead.

The firmware applies tags when it reads a channel (`codeplug_channel`), so every caller sees the kind. A codeplug with
no tag pays one walk of the tag range per change of the area.

## 7. Systems

A system is what channels of a kind share: a trunked network, or a conventional mode's identity and keys. Records of
a system type; the generic layer knows only the headers below.

### 7.1 System record

Key 0x0100 + n, n = 0-254 (0xFF is "none" in a tag).

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | System type (4). 0 and 0xFF: no system |
| 1 | 1 | Flags (below) |
| 2 | 1 | Sharing: 0 off, 1 identity only, 2 whole system with key (8) |
| 3 | 1 | Identity length L: the first L octets of the type's block say which system this is (a network and system ID). Two records of one type and the same nonempty identity are the same system. L = 0 leaves the system without an identity for matching |
| 4 | 4 | This radio's ID on the system, little endian; 0 = the kind's radio-wide one (5.5). Never shared |
| 8 | 2 | Fallback channel, 0 = none |
| 10 | 2 | Its name check (2.2) |
| 12 | 16 | Name, as a channel record's |
| 28 | rest | The type's block: the identity first, then the type's fields, grown by appending |

| Flag bit | Meaning |
|---|---|
| 0 | Needs its secret (7.4): without it the system can't be used |
| 1 | Transmit side allowed: the radio may register, affiliate and transmit on the system where the type's mode module does. Clear: **listen only**: the radio never registers, affiliates or transmits there, whatever a channel's tag says. A zeroed or short record is listen only |
| 2-3 | Reserved: written 0, kept as found |
| 4-7 | The type's own |

Listen only is a per-system choice of the user. It is not a sharing rule: a share carries the flag as set (8), and a
receiver never forces it. A channel's own TX flag (2.1) still applies on a system that allows the transmit side.

A type's mode module may name network IDs the radio refuses to transmit on (`noTx(fields)`). For such a system the
editor shows listen only, locks the Transmit choice, hides the fields only a transmitter reads (the module marks them
`txOnly`) and writes the flag clear on save and export. The firmware enforces the rule whatever is stored. The P25
trunked type's IDs are in [p25-codeplug.md](p25-codeplug.md) 3.

### 7.2 System entries

Key 0x0200 + n, n = 0-255: the parts of a system that come in numbers. Each entry names its system and a class, so one
family holds sites, their channels and a channel plan for every type.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | System n |
| 1 | 1 | Class (below) |
| 2 | 1 | Group: the site (or other list) the entry belongs to; 0xFF = the whole system's |
| 3 | 1 | Flags: bit 0 a control channel, bits 1-3 reserved, bits 4-7 the type's |
| 4 | rest | The class's body |

| Class | Body |
|---|---|
| 0 Channel | Channel number (2, the system's own numbering), RX Hz (4), TX Hz (4, 0 = none), then the type's tail |
| 1 Site | The type's (site identity). The entries whose group equals this site's group are its channels |
| 2 Plan row | The type's (one row of a channel plan: how channel numbers become frequencies). Group: the row's identifier |
| 3-127 | Reserved for generic classes |
| 128-255 | The type's own |

Entries are read in number order within a system and class; the numbers say nothing else. A system's entries go
when the system goes.

### 7.3 Channels on a system

A channel names its system in its tag (2.1). Its own talk target is the record's contact or a target (5.2), its
receive groups the record's RX group list or a target list (5.3). For modes with fallback, a channel's fallback (2.1)
overrides the system's (7.1). A fallback channel is an ordinary channel the radio moves to when the system can't be used.

### 7.4 System secret

Key 0x0300 + n: system n's own secret (an authentication key), the type's layout. Traffic keys are in their own family
(5.4).

## 8. Secrets

System secrets (7.4) and traffic keys (5.4) are **secrets**.

- The Codeplug Editor shows a secret only as set or not set until the user asks to see it, and leaves secrets out of
  a file export unless the user turns secrets on for that export and confirms it. Codeplug attachments in diagnostic
  reports omit secrets.
- Zone and channel sharing carries a system's secrets only at the system's "whole system with key" level, and only
  when the sender turns keys on for that share ([zone-share.md](zone-share.md)).
- A firmware reads them only for the mode module that uses them; the debug link never shows one.
