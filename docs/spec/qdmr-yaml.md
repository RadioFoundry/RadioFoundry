<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# qdmr YAML codeplug

qdmr and its command-line tool `dmrconf` save a codeplug as a YAML file, the "extensible codeplug". The Codeplug
Editor opens and saves this file, so a codeplug can move between qdmr and the editor in both directions. This spec
covers the file as qdmr 0.15 writes it, the editor's mapping to the open-firmware records
([open-firmware-cps.md](open-firmware-cps.md) 7), and what each direction leaves out.

Sources: qdmr's published manual (the "Extensible Codeplug File Format" chapter), files written by qdmr 0.15.1, and
`dmrconf encode` run on test files, with the binary it writes decoded by the editor's own decoder. qdmr's source is
GPL and was not read ([CLEANROOM.md](../CLEANROOM.md)).

Code: `site/static/js/codeplug/yaml.js` (the YAML subset), `site/static/js/codeplug/qdmr.js` (the mapping),
`site/static/js/editor/qdmrpart.js` (the editor's Open and Save), tests in `site/tests/qdmr.test.mjs`.

## 1. Syntax

The file is one YAML 1.2 document. qdmr uses a small part of YAML, and `yaml.js` reads that part:

| Construct | Example | Notes |
|---|---|---|
| Document markers | `---` first, `...` last | Optional. A second document is refused |
| Block map | `settings:` and indented keys | Two-space indent |
| Block list | `  - dmr:` | qdmr indents a list two spaces under its key |
| Flow map | `{id: cont1, name: TG 9, number: 9}` | Contacts, radio IDs and group lists are written this way |
| Flow list | `A: [ch1, ch2]`, `B: []` | References and paths |
| Plain scalar | `name: Repeater TS1`, `name: 201` | A plain `201` reads as a number; names are converted back to text |
| Quoted scalar | `password: ""`, `'It''s'`, `"a\tb"` | Double quotes take the usual escapes |
| Null | `rxTone: ~` | |
| Tag | `power: !default ""` | `!default`: use the radio's global value |
| Comment | `# ...` | After whitespace, outside quotes |
| Block scalar | `|`, `>` with `-` or `+` | Read for completeness; qdmr doesn't write them |

Anchors, aliases, complex keys and tabs in indentation are refused with an error. The writer quotes a string when the
plain form would read back as something else (a number, `true`, empty, leading space, `: ` or `#`).

## 2. Structure

The top-level keys, in qdmr's order:

| Key | Holds | Editor |
|---|---|---|
| `version` | qdmr version that wrote the file | Written as `0.15.1`; not checked on import |
| `settings` | Radio-wide settings: `power`, `defaultID`, `boot`, `audio`, `tone`, `gnss`, `dmr` and others | Part, see 4.1 |
| `radioIDs` | The radio's DMR IDs: `- dmr: {id, name, number}` | Yes |
| `contacts` | `- dmr: {...}` digital contacts, `- dtmf: {...}` DTMF contacts | Yes |
| `groupLists` | RX group lists: `{id, name, contacts: [refs]}` | Yes |
| `channels` | `- dmr: {...}` and `- fm: {...}` channels | Yes |
| `zones` | `{id, name, A: [refs], B: [refs]}` | Yes |
| `scanLists` | Scan lists | Dropped |
| `positioning` | `- aprs: {...}` APRS systems, `- dmr: {...}` DMR position reporting | APRS only |
| `roaming` | Roaming channels and zones | Dropped |
| `commercial` | `encryptionKeys` | Dropped |
| `sms` | Message format and templates | Dropped |

Older qdmr versions and the manual name the element types `digital` and `analog`. The import accepts both spellings;
the export writes `dmr` and `fm`, as qdmr 0.15 does.

### 2.1 References

Every element has an `id` that is unique in the file. Other elements refer to it by that id: a channel's `contact`,
`groupList`, `radioId` and `aprs`, a zone's `A` and `B`, a group list's `contacts`, an APRS system's `revert`. qdmr
writes ids as a prefix and a count (`ch12`, `cont3`, `grp1`, `zone2`, `aprs1`, `id1`), and the export does the same
in table order. The import doesn't depend on the form. A reference to an id the file doesn't define is dropped.

### 2.2 Values

| Kind | Forms read | Form written |
|---|---|---|
| Frequency | `439.5625 MHz`, `145500 kHz`, `Hz`, `GHz`, a bare number in MHz | `439.5625 MHz` |
| Duration | `3 min`, `45 s`, `500 ms`, a bare number in seconds, `infinite` / `off` (0), `!default` | `3 min`, `45 s`, `infinite` |
| Sub-tone | `{ctcss: 67}`, `{ctcss: 67 Hz}`, `{dcs: 23}`, `{dcs: -754}` (negative = inverted), `~` | `{ctcss: 67 Hz}`, `{dcs: -754}`, `~` |
| Power | `Min`, `Low`, `Mid`, `High`, `Max`, `!default` | Same |
| Location | Maidenhead locator, 4, 6 or 8 characters, or `""` | 6 characters |
| Boolean | `true`, `false` | Same |

## 3. Elements

### 3.1 Radio IDs and settings

| qdmr | Editor | Notes |
|---|---|---|
| `settings.defaultID` | | Names the radio ID that is the radio's own |
| default radio ID `number` | Radio, DMR ID | |
| default radio ID `name` | Radio, radio name | Shortened to 8 characters |
| other radio IDs | Channel DMR ID | A channel whose `radioId` names another ID gets that ID as its own DMR ID |
| `settings.boot.message1`, `message2` | Boot text lines 1 and 2 | 16 characters |
| `settings.dmr.groupCallHangTime`, `privateCallHangTime` | Group and private hang | In 500 ms steps (open-firmware-cps.md 7.3.2, the unit our firmware assumes) |
| `settings.gnss.fixedPositionEnabled`, `fixedPosition` | APRS: use fixed position, latitude, longitude | Applied to every APRS configuration |
| `settings.audio.vox` | | Decides a channel's VOX when the channel says `!default` |

The export writes one radio ID for the radio (`id1`) and one more for each distinct channel DMR ID. Other settings
(`power`, `audio`, `tone`, the boot logo and password, talker alias encoding and so on) are radio settings the
codeplug image doesn't hold in the editor's model; the import ignores them and the export leaves them out, so qdmr
uses its defaults.

### 3.2 Contacts

| qdmr | Editor |
|---|---|
| `dmr.name` | Name (16 characters) |
| `dmr.type`: `GroupCall`, `PrivateCall`, `AllCall` | Call type: group, private, all |
| `dmr.number` | ID (all call: 16777215) |
| `dmr.openGD77.timeSlotOverride`: `None`, `TS1`, `TS2` | Timeslot: channel's, 1, 2 |
| `dmr.ring` | Dropped (written as `false`) |
| `dtmf.name`, `dtmf.number` | DTMF contact name and code |

In the export, DTMF contacts follow the digital contacts and continue their id count.

### 3.3 RX group lists

`name` and `contacts` map directly. A list keeps its first 32 contacts.

### 3.4 Channels

Both types:

| qdmr | Editor | Notes |
|---|---|---|
| `name` | Name | 16 characters |
| `rxFrequency`, `txFrequency` | RX, TX frequency | 10 Hz grid |
| `power` | Power byte: `Min` 1, `Low` 3, `Mid` 6, `High` 9, `Max` 10, `!default` 0 | open-firmware-cps.md 7.3.3. Another stored level exports as `!default` |
| `timeout` | TX timeout | 15 s steps, rounded up; `infinite` and `!default` are off |
| `rxOnly` | RX only | |
| `vox` | VOX on or off | A number above 0 is on, `off` or 0 is off, `!default` follows `settings.audio.vox`. The export writes `1` or `off` |
| `openGD77.scanZoneSkip`, `scanAllSkip` | Zone skip, all-channel skip | |
| `openGD77.beep` | Not "no beep" | |
| `openGD77.powerSave` | Not "no economy" | |
| `openGD77.location` | Use location, latitude, longitude | Locator to the center of its square. The export writes a 6-character locator, about 5 km |
| `openGD77.talkerAliasTS1`, `TS2`: `None`, `APRS`, `Text`, `Both` | Talker alias TX on TS1, TS2: 0-3 | |
| `aprs` | APRS configuration | Only an APRS system the file defines |
| `scanList` | Dropped | |
| `admit` | Dropped | Written as `Always` |

DMR channels:

| qdmr | Editor | Notes |
|---|---|---|
| `colorCode` | Color code | 0-15 |
| `timeSlot`: `TS1`, `TS2` | Timeslot | |
| `contact` | TX contact | |
| `groupList` | RX group list | |
| `radioId` | Channel DMR ID | `!default` or the default ID: none |
| `roaming` | Roaming flag | Any value sets it; the export writes `!default ""` |
| `extended.talkaround` | DMO flag | |
| other `extended` keys | Dropped | |

FM channels:

| qdmr | Editor | Notes |
|---|---|---|
| `bandwidth`: `Narrow`, `Wide` | 12.5 or 25 kHz | |
| `rxTone`, `txTone` | Tones | |
| `squelch` | Squelch level | qdmr's 1-10 is doubled onto the record's 1-21; `0` and `!default` are the radio's default. The export halves it |
| `extended.reverseBurst` | Dropped | |

The SFR settings (flag, bytes A and B) have no qdmr counterpart; the export leaves them out and says so.

### 3.5 Zones

`name` and `A` map to a zone and its channels. qdmr's zones have two lists, A and B, while an open-firmware zone has
one. A zone with an empty B list imports as one zone. A zone with channels in both imports as two, `<name> A` and
`<name> B`, which is also what `dmrconf encode` writes for this radio. The export writes each zone as an A list with an
empty B list. A zone keeps its first 80 channels.

### 3.6 APRS

An `aprs` element under `positioning` becomes an APRS configuration (open-firmware-cps.md 7.3.8):

| qdmr | Editor | Notes |
|---|---|---|
| `name` | Name | 8 characters |
| `source` | SSID | The call sign part is dropped; the radio sends its own |
| `path` | Path 1 and 2 | Entries after the second are dropped |
| `icon` | Symbol table `/` and symbol | Table below; an unknown name imports as the car `/>` |
| `message` | Comment | 23 characters |
| `revert` | TX frequency | The TX frequency of the FM channel it names. The export names the first FM channel on that frequency |
| `opengd77.baud`: 300, 1200 | 300 baud flag | |
| `opengd77.transmitQSY` | QSY flag | |
| `period` | Dropped | The radio sets the beacon interval in its menu |
| `destination` | Dropped | Written as `APN000-0` |

The export builds `source` from the radio name (letters and digits, up to 6) and the SSID, or `N0CALL` and the SSID
when the radio name has none. The silent and ambiguity settings have no qdmr counterpart.

`dmr` elements under `positioning` (position reports over DMR) have no counterpart and are dropped.

qdmr's icon names, each a symbol of the primary table (`/`), as `dmrconf encode` writes them:

| Symbol | Names |
|---|---|
| `!` - `/` | PoliceStation, None, Digipeater, Phone, DXCluster, HFGateway, SmallPlane, MobileSatelliteStation, WheelChair, Snowmobile, RedCross, BoyScout, Home, X, RedDot |
| `0` - `9` | Circle0 - Circle9 |
| `:` - `@` | Fire, Campground, Motorcycle, RailEngine, Car, FileServer, HCFuture |
| `A` - `Z` | AidStation, BBS, Canoe, (D has none), Eyeball, Tractor, GridSquare, Hotel, TCPIP, (J has none), School, Logon, MacOS, NTSStation, Balloon, PoliceCar, TBD, RV, Shuttle, SSTV, Bus, ATV, WXService, Helo, Yacht, Windows |
| `[` - `` ` `` | Jogger, Triangle, PBBS, LargePlane, WXStation, DishAntenna |
| `a` - `z` | Ambulance, Bike, ICP, FireStation, Horse, FireTruck, Glider, Hospital, IOTA, Jeep, SmallTruck, Laptop, MicE, Node, EOC, Rover, Grid, Antenna, PowerBoat, TruckStop, TruckLarge, Van, Water, XAPRS, Yagi, Shelter |

A symbol from the alternate table, or `/D` and `/J`, exports as `None` with a note.

## 4. What each direction reports

The import returns the record counts and a list of notes, each a code and a count, for what it changed or left out:
names shortened, scan lists, roaming, encryption keys, DMR position reporting, APRS periods, extra path entries,
unknown icons, lists and zones over their limits, records over the table limits, unknown element types, channels
without a valid frequency, sub-tones that aren't CTCSS or DCS. The editor lists them after the import.

The export returns notes for SFR settings, power levels qdmr has no name for, channel locations rounded to a locator,
APRS frequencies with no FM channel to name, APRS symbols with no icon, and the silent and ambiguity flags. The VFOs
and the DMR ID database are never part of a qdmr file.

An import replaces the channel, zone, contact, DTMF contact, RX group list and APRS tables and the settings in 3.1, as
one undoable change. Everything else in the open codeplug (the VFO frequencies, the radio's other settings, the DMR ID
database) stays. The VFOs lose their contact, list and APRS references, since those pointed into the old tables.
Records are numbered in file order.

## 5. qdmr's own binary encoding

`dmrconf encode -R openuv380` on the same file writes the same channels, contacts, lists, zones, DTMF contacts and
boot settings as the editor, with these differences (qdmr 0.15.1):

| Field | qdmr writes | Editor writes |
|---|---|---|
| TX contact of a channel with an RX group list | 0 | The contact |
| Squelch (channel byte 0x37) | 0 | The level (3.4) |
| Group and private hang | 0 (the bytes are left zero) | The settings (3.1) |
| APRS symbol (bytes 0x1D, 0x1E) | Table index (0 primary) and symbol index (character - 0x21) | The characters, as `core/codeplug/codeplug.c` reads them |
| APRS TX frequency | 0 | The `revert` channel's TX frequency |
| APRS record magic | 0x4152 (older layout) | 0x4153 |
| A channel without an `openGD77` block | Beep and power save off | Beep and power save on |
| A contact without an `openGD77` block | Timeslot override TS1 | No override |
| Channel without location, `settings.gnss.fixedPosition` set | The fixed position | No location |
| `radioId: !default` | Flag 1 bit 7 set with ID 0 | Bit 7 clear, 00 16 00 |
| `extended.talkaround`, `roaming` | Not written | DMO and roaming flags |

The APRS symbol difference means the firmware reads no symbol from a codeplug qdmr wrote (it takes a byte below `!`
as none). A codeplug written by the editor carries the symbol characters.

## 6. Tests

`site/tests/qdmr.test.mjs` covers the YAML subset, the value formats, an import of a file written in qdmr's form, the
round trips YAML to model to YAML and cpsSim's codeplug to YAML to model, and the Tools page converters. When
`dmrconf` is installed, a test exports a codeplug, runs `dmrconf encode -R openuv380` on it and compares the decoded
binary with the editor's encoding, field by field, apart from the differences in section 5. Without `dmrconf` that
test is skipped.
