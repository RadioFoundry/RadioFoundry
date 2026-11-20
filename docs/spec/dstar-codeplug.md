<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# D-STAR in the codeplug

The stored records of D-STAR DV channels, on the generic layer of [channel-modes.md](channel-modes.md). The radio
header the fields go into is [dstar-modem.md](dstar-modem.md) 3.1, the slow data message 3.5. The firmware's codec is
`core/dstar/dstar_codeplug.c` (`CONFIG_DSTAR`); the Codeplug Editor's is `site/static/js/codeplug/mode_dstar.js`.

| Id | Kind | System type |
|---|---|---|
| 4 | D-STAR DV (short name `DV`) | none: a D-STAR channel names no system |

## 1. Text fields

Every text field below is stored as it goes on air: ASCII, padded with spaces to its full length, not terminated.
Every reader cleans a field the same way, so a field is safe whatever its bytes:

| Stored byte | Read as |
|---|---|
| 0x00, 0xFF (a zeroed, erased or short record) | a space |
| `a`-`z` | upper case, in a callsign field |
| Outside the field's characters | a space |

| Field | Characters |
|---|---|
| Callsign fields (UR, RPT1, RPT2, MY, suffix) | `A`-`Z`, `0`-`9`, space and `/` |
| Message | printable ASCII, 0x20-0x7E, both cases |

The callsign characters are the ones callsigns and the gateway commands of 2.3 use; the firmware keeps every other
character out of a header it sends. A writer cleans a field the same way before storing it, and a field shorter than
its length is padded with spaces.

## 2. Channel (kind 5)

The tag ([channel-modes.md](channel-modes.md) 2): no system (0xFF), no fallback. Flag bit 0, transmit allowed, is set
on a new channel: D-STAR is an amateur mode the user transmits on. The base record rule still applies: the base record
is the safe stand-in (DMR, receive only, the channel's frequencies), written before the tag. Flag bits 4-7: 0.

### 2.1 Parameter block

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | UR: the station called. Blank (all spaces once cleaned) reads as `CQCQCQ  ` |
| 8 | 8 | RPT1: the departure repeater. Blank on a direct channel |
| 16 | 8 | RPT2: the destination repeater (the gateway, to go beyond the repeater). Blank on a direct channel |
| 24 | 1 | Flags: bit 0 via a repeater (clear: direct between radios). Bits 1-7 reserved: written 0, kept as found. 0xFF (an erased byte) reads as 0 |

A zeroed block, or none, is a direct channel calling CQ: UR `CQCQCQ  `, RPT1 and RPT2 blank, flags 0. Fields are
appended after offset 24.

The base record carries:

| Record field | Use |
|---|---|
| Name, RX and TX frequency, power, TX time-out, scan skip | As any channel. A repeater's split is the record's RX and TX |
| Contact, RX group list, color code, time slot, per-channel DMR ID | Not used |
| Mode, receive only | DMR, receive only, always (the safe stand-in) |

### 2.2 The header the radio sends

At PTT the radio builds the radio header ([dstar-modem.md](dstar-modem.md) 3.1) from the channel and the radio-wide
record (3):

| Header field | From |
|---|---|
| Flag 1 | b6 (via a repeater) from the channel's flag bit 0; every other bit 0 (voice, no control, no emergency) |
| Flags 2, 3 | 0 |
| RPT2 | The channel's RPT2 |
| RPT1 | The channel's RPT1 |
| UR | The channel's UR, or for one over the gateway command the user picked (2.3) |
| MY | The radio-wide MY call; blank = the radio's DMR callsign, cut to 8 characters |
| MY suffix | The radio-wide suffix |

The slow data carries the radio-wide message when it isn't blank.

### 2.3 Gateway commands

Amateur D-STAR gateways commonly take commands in the UR field. The firmware's helpers send them by setting UR for a
single over, without changing the stored channel; they are common gateway practice, not part of the D-STAR standard,
and a gateway may answer some, all or none of them:

| UR | Command |
|---|---|
| Reflector callsign padded to 6 characters, module letter in position 7, `L` in position 8 (`REF030CL`) | Link the repeater to that reflector's module |
| `       U` (seven spaces, `U`) | Unlink |
| `       E` | Echo test: the gateway plays the over back |
| `       I` | Information: the gateway says what it is linked to |

A channel may also store such a UR, which then goes with every over on it.

## 3. Radio-wide record (kind 5)

Key 0x0A00 + 4 ([channel-modes.md](channel-modes.md) 5.5):

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | MY: this radio's callsign. Blank = the radio's DMR callsign, which the radio core supplies |
| 8 | 4 | MY suffix: blank = none (four spaces on air) |
| 12 | 20 | The slow data message (message characters, 1); blank = no message sent |

Absent, zeroed or short: every field missing or zeroed reads blank, so a radio with no record sends its DMR callsign,
no suffix and no message. Fields are appended after offset 31.
