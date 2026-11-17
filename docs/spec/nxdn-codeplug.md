<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# NXDN in the codeplug

The stored records of NXDN conventional channels, on the generic layer of [channel-modes.md](channel-modes.md).
Little endian throughout. The firmware's codec is `core/nxdn/nxdn_codeplug.c` (`CONFIG_NXDN`). The radio core runs a
channel of kind 4 where the build has `CONFIG_NXDN` and the board's air interface carries NXDN at either rate
([nxdn.md](nxdn.md) 6); elsewhere it is parked ([channel-modes.md](channel-modes.md) 6).

| Id | Kind | System type |
|---|---|---|
| 4 | NXDN conventional | none |

## 1. Radio-wide record (kind 4)

Key 0x0A00 + 4 ([channel-modes.md](channel-modes.md) 5.5):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Unit ID, 1 to 65519; 0, absent or out of range = the radio's DMR ID where it is 1 to 65519, else 1 |

A channel's own unit ID is the record's per-channel DMR ID, where it is 1 to 65519; the narrowest one set counts. IDs
above 65519 are special (TS 1-B) and never a radio's. The VFO keeps its own unit ID ([nxdn.md](nxdn.md) 6.2).

## 2. Conventional channel (kind 4)

The tag ([channel-modes.md](channel-modes.md) 2): no system, flag bit 0 transmit allowed. The parameter block:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Bits 0-1 squelch: 0 RAN (every call of the RX RAN), 1 selective (2.1); 2 and 3 read as RAN. Bit 2: 4800 bit/s; clear: 9600 bit/s. Bits 3-7: 0 |
| 1 | 1 | RX RAN, 0 to 63; 0 = every RAN (TS 1-B 4.1.7.1). Above 63 reads as 0 |
| 2 | 1 | TX RAN: bit 7 set = bits 5-0 hold it (0 to 63); clear = the RX RAN, 1 when the RX RAN is 0. Bit 6: 0, set reads as clear |

A zeroed block, or none, is 9600 bit/s, RAN squelch, every RAN received and RAN 1 sent; with the tag's flag bit 0
clear, receive only. Both rates use 12.5 kHz channel settings ([nxdn.md](nxdn.md) 9). Encryption fields are appended
later.

The base record carries:

| Record field | Use |
|---|---|
| Name, RX and TX frequency, power, TX time-out, scan skip | As any channel |
| Contact | The talk target: a group contact = group (1-65535; 65535 calls all), a private contact = a unit call (1-65519), the all-call contact = group 65535, none or an ID out of range = group 1 |
| Per-channel DMR ID | This radio's unit ID on the channel where 1 to 65519; else the radio-wide one (1) |
| RX group list | Kept, not read |
| Mode, receive only | DMR, receive only, always (the safe stand-in) |

There is no talkaround setting: NXDN frames are inbound either way ([nxdn.md](nxdn.md) 6.2). The record's direct-mode
bit is kept and not read.

### 2.1 Selective squelch

Of the calls on the RX RAN, selective plays:

| Call | Played |
|---|---|
| A group call to the channel's group | yes |
| A call to 65535 (all) | yes |
| A unit call to this radio's unit ID | yes |
| Anything else | no: shown as another group, not played |

On a channel whose contact is a private one or the all-call contact, selective plays calls to all and unit calls to
this radio. RAN squelch plays every call of the RAN.

### 2.2 Writing from the radio

An NXDN channel the radio saves (the VFO in NXDN stored as a channel, a mode changed for now made permanent) is
written as [channel-modes.md](channel-modes.md) 2.4 gives: the base record as the safe stand-in, then a new tag of
kind 4 with no system, transmit allowed from the channel and a full parameter block from the NXDN settings: the rate,
the RX and TX RAN, and the squelch selective where the settings name a group to hear, else RAN. Its contact is the
stored contact of the talk target, if any (else none: group 1). The settings' unit ID and emergency bit are not
stored. Editing a channel that is NXDN already rewrites its record and lets the tag follow, the block kept byte for
byte; a copy at another number takes the source's tag. When the channel editor changed the RAN or the rate
([ui.md](ui.md) 2.1), the record is written as the safe stand-in and the tag follows it, then the edited block (its
squelch as read) is written over the front of the tag's ([channel-modes.md](channel-modes.md) 2.4): the system, the
fallback, the other flags and any later bytes of the block stay, and under a tag with the base-record flag the
record keeps its own mode. A copy takes its source's tag first. A plain channel in NXDN takes the edited block in place of the settings'.

## 3. Sharing

A zone share carries an NXDN channel's tag as it is ([zone-share.md](zone-share.md) 4.2): the block is 3 octets, inside
the share's limit of 64.
