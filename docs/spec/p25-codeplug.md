<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# P25 in the codeplug

The stored records of P25 channels and systems, on the generic layer of [channel-modes.md](channel-modes.md). Little
endian throughout. The firmware's codec is `core/p25/p25_codeplug.c` (`CONFIG_P25`, `CONFIG_P25_TRUNK`). The radio core
runs a conventional channel (kind 2) where the build has `CONFIG_P25` and the board's air interface receives P25
([p25.md](p25.md) 7.1); elsewhere it is parked ([channel-modes.md](channel-modes.md) 6). With `CONFIG_P25_TRUNK` it
runs a trunked channel (kind 3) the same way ([p25-trunking.md](p25-trunking.md) 7).

| Id | Kind | System type |
|---|---|---|
| 2 | P25 conventional | reserved: a conventional system that holds identity and keys, laid out with encryption |
| 3 | P25 trunked | P25 trunked system |

## 1. Radio-wide record (kind 2)

Key 0x0A00 + 2 ([channel-modes.md](channel-modes.md) 5.5), read by both kinds:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Unit ID, 1 to 0xFFFFFE; 0, absent or out of range = the radio's DMR ID |

A channel's own unit ID is the record's per-channel DMR ID; a trunked system's is the system's own ID (7.1). The
narrowest one set counts. The radio-wide one is also the VFO's.

## 2. Conventional channel (kind 2)

The tag ([channel-modes.md](channel-modes.md) 2): no system, flag bit 0 transmit allowed. The parameter block:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Bits 0-1 squelch: 0 normal (every call of the RX NAC), 1 selective (2.1); 2 and 3 read as normal. Bits 2-7: 0 |
| 1 | 2 | RX NAC: bit 15 set = bits 11-0 hold the NAC, 0xF7E = any NAC; clear = 0x293. Bits 12-14: 0 |
| 3 | 2 | TX NAC: the same encoding; clear = the RX NAC, 0x293 when the RX NAC is any |

A zeroed block, or none, is normal squelch on NAC 0x293 both ways; with the tag's flag bit 0 clear, receive only.
0xF7F is refused as an RX NAC (read as 0x293), and 0xF7E and 0xF7F as a TX NAC (read as the default). Encryption
fields (algorithm and key ID, selecting a traffic key of a system of type 2) are appended later.

The base record carries:

| Record field | Use |
|---|---|
| Name, RX and TX frequency, power, TX time-out, scan skip | As any channel |
| Contact | The talk target: a group contact = talk group (1-65535), a private contact = a unit call, the all-call contact = talk group 65535, none = talk group 1 |
| RX group list | Kept; for several receive talk groups later |
| Per-channel DMR ID | This radio's unit ID on the channel; unset = the radio-wide one (1) |
| Direct mode | Talkaround (2.2) |
| Mode, receive only | DMR, receive only, always (the safe stand-in) |

### 2.1 Selective squelch

Selective plays, of the calls on the RX NAC:

| Call | Played |
|---|---|
| A group call to the channel's talk group | yes |
| A group call to talk group 65535 (everyone) | yes |
| A unit call to this radio's unit ID | yes |
| Anything else | no: tracked and shown as another talk group, not played |

On a channel whose contact is a private one (a unit call) or the all-call contact, selective plays the everyone talk
group and unit calls to this radio. Normal squelch plays every call of the NAC. The record's RX group list is to widen
selective later: its group contacts' talk groups are played too, the talk group first. Until then it is kept and not
read.

### 2.2 Talkaround

Two values: on, or automatic. The record's direct mode set is on (status symbols say the call is direct, as to
another radio). Clear is automatic: on where RX and TX are the same frequency, off otherwise (as to a repeater). There
is no stored "off" for a simplex frequency: a zeroed record is automatic, which is right for both kinds of channel.

### 2.3 Writing from the radio

A P25 channel the radio saves (a VFO in P25 stored as a channel, a mode changed for now made permanent) is written as
[channel-modes.md](channel-modes.md) 2.4 gives: the base record as the safe stand-in, then a new tag of kind 2 with no
system, transmit allowed from the channel and a full parameter block: the RX and TX NAC, and the squelch selective
where the settings name a talk group to hear, else normal. Its contact is the stored contact of the talk target, if
any (else none: talk group 1). Talkaround on sets the direct-mode bit; automatic and off leave it clear (automatic, 2.2).
A session unit ID is not stored. Editing a channel that is P25 already rewrites its record and lets the tag follow,
the block kept byte for byte; a copy at another number takes the source's tag ([channel-modes.md](channel-modes.md)
2.4). When the channel editor changed the NAC ([ui.md](ui.md) 2.1), the record is written as the safe stand-in and
the tag follows it, then the edited NACs and the squelch as read are written over the front of its block
([channel-modes.md](channel-modes.md) 2.4): the system, the fallback, the other flags and any later bytes of the block
stay, and under a tag with the base-record flag the record keeps its own mode. A copy takes its source's tag first. A plain channel in P25 takes the
edited NACs in place of the settings'.

## 3. Trunked system (type 3)

Read by the trunking follower ([p25-trunking.md](p25-trunking.md) 6).

The generic header ([channel-modes.md](channel-modes.md) 7.1), with identity length 5 and flag bit 1 (transmit side)
clear by default: a system without it is listen only, and the radio never registers or transmits on it. The own ID is the radio's unit ID on the system, 0 = the radio-wide one (1). The type's block:

| Offset | Size | Field |
|---|---|---|
| 0 | 3 | WACN, bits 19-0; 0xFFFFFF = any |
| 3 | 2 | System ID, bits 11-0; 0xFFFF = any |
| 5 | 2 | Control channel NAC: bit 15 set = bits 11-0 hold it; clear = any |
| 7 | 1 | Options: bit 0 skip encrypted calls; bit 1 use only the programmed channel plan (don't learn it from the control channel); bit 2 don't register or affiliate (PTT asks for a voice channel at once). Bits 3-7: 0 |
| 8 | 1 | Hold time after a call in seconds, 0 = the radio's |
| 9 | 1 | Change site below this dBm (signed; 0 = the radio's roaming threshold) |

Authentication and traffic keys are appended later; keys use the traffic key family
([channel-modes.md](channel-modes.md) 5.4).

### 3.1 Sites

Generic entries ([channel-modes.md](channel-modes.md) 7.2) of class 1, one per site; the group is the site's number
in the system (0-254). Body: RFSS (1), site (1), 0xFF = not known. Both are for display and roaming only.

### 3.2 Control channels

Entries of class 0, flag bit 0 set, group = the site's. Entry order is the hunt order: the first is the primary.
Body: the channel number (2: identifier in bits 15-12, channel in bits 11-0; 0 = given by frequency only), RX Hz (4),
TX Hz (4: the inbound frequency where the radio sends its requests; 0 = from the channel number and the identifier's
transmit offset).

### 3.3 Channel identifier plan

Entries of class 2, group = the identifier (0-15), at most one each. An identifier not programmed is learned from the
control channel. Body:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Base frequency Hz |
| 4 | 4 | Channel spacing Hz |
| 8 | 4 | Transmit offset Hz, signed (inbound minus outbound; 0 = take it from the control channel) |
| 12 | 4 | Bandwidth Hz |
| 16 | 1 | Channel type: 0 FDMA, 1 TDMA |
| 17 | 1 | TDMA: slots per carrier (0 = 2) |

A grant on a TDMA identifier is recognized and reported; its voice is not decoded.

## 4. Trunked channel (kind 3)

The tag names the system; flag bit 0 (transmit allowed) clear by default, and the transmit side needs it, the
system's transmit side (3), a unit ID and a talk group; bit 2 with the fallback fields: the
channel's own fallback, else the system's; kind bit 4: no fallback on this channel. The parameter block:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Bit 0: follow every group of the system; clear: the talk group and the RX group list only. Bits 1-7: 0 |

The talk group is the record's contact, the groups followed its RX group list (group IDs 1-65535). Following every
group is the channel's choice, so one system can have a channel that follows all and others that follow a few.
