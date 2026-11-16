<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# P25 in the codeplug

The stored records of P25 channels and systems, on the generic layer of [channel-modes.md](channel-modes.md). Little
endian throughout. The firmware's codec is `core/p25/p25_codeplug.c` (`CONFIG_P25`, `CONFIG_P25_TRUNK`); the radio core
does not run P25 yet, so a P25 channel is shown as a mode not in this build in every build ([channel-modes.md](channel-modes.md) 6).

| Id | Kind | System type |
|---|---|---|
| 2 | P25 conventional | reserved: a conventional system that holds identity and keys, laid out with encryption |
| 3 | P25 trunked | P25 trunked system |

## 1. Radio-wide record (kind 2)

Key 0x0A00 + 2 ([channel-modes.md](channel-modes.md) 5.5), read by both kinds:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Unit ID, 1 to 0xFFFFFE; 0, absent or out of range = the radio's DMR ID |

A channel's own unit ID is the record's per-channel DMR ID; a trunked system's is the system's own ID (7.1).

## 2. Conventional channel (kind 2)

The tag ([channel-modes.md](channel-modes.md) 2): no system, flag bit 0 transmit allowed. The parameter block:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Bits 0-1 squelch: 0 normal (every call of the RX NAC), 1 selective (the talk group's calls and unit calls to this radio); 2 and 3 read as normal. Bits 2-7: 0 |
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
| Direct mode | Talkaround on a channel whose RX and TX differ |
| Mode, receive only | DMR, receive only, always (the safe stand-in) |

## 3. Trunked system (type 3)

No firmware reads these records yet: the layout is defined ahead of the trunking engine, for following a control
channel without transmitting.

The generic header ([channel-modes.md](channel-modes.md) 7.1), with identity length 5 and flag bit 1 (transmit side)
clear by default. The own ID is the radio's unit ID on the system, 0 = the radio-wide one (1). The type's block:

| Offset | Size | Field |
|---|---|---|
| 0 | 3 | WACN, bits 19-0; 0xFFFFFF = any |
| 3 | 2 | System ID, bits 11-0; 0xFFFF = any |
| 5 | 2 | Control channel NAC: bit 15 set = bits 11-0 hold it; clear = any |
| 7 | 1 | Options: bit 0 skip encrypted calls; bit 1 use only the programmed channel plan (don't learn it from the control channel). Bits 2-7: 0 |
| 8 | 1 | Hold time after a call in seconds, 0 = the radio's |
| 9 | 1 | Change site below this dBm (signed; 0 = the radio's roaming threshold) |

Registration, affiliation, authentication and traffic keys are appended later; keys use the traffic key family
([channel-modes.md](channel-modes.md) 5.4).

### 3.1 Sites

Generic entries ([channel-modes.md](channel-modes.md) 7.2) of class 1, one per site; the group is the site's number
in the system (0-254). Body: RFSS (1), site (1), 0xFF = not known. Both are for display and roaming only.

### 3.2 Control channels

Entries of class 0, flag bit 0 set, group = the site's. Entry order is the hunt order: the first is the primary.
Body: the channel number (2: identifier in bits 15-12, channel in bits 11-0; 0 = given by frequency only), RX Hz (4),
TX Hz (4, 0 = none; kept for transmit later).

### 3.3 Channel identifier plan

Entries of class 2, group = the identifier (0-15), at most one each. An identifier not programmed is learned from the
control channel. Body:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Base frequency Hz |
| 4 | 4 | Channel spacing Hz |
| 8 | 4 | Transmit offset Hz, signed |
| 12 | 4 | Bandwidth Hz |
| 16 | 1 | Channel type: 0 FDMA, 1 TDMA |
| 17 | 1 | TDMA: slots per carrier (0 = 2) |

A grant on a TDMA identifier is recognized and reported; its voice is not decoded.

## 4. Trunked channel (kind 3)

The tag names the system; flag bit 0 (transmit allowed) clear by default; bit 2 with the fallback fields: the
channel's own fallback, else the system's; kind bit 4: no fallback on this channel. The parameter block:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Bit 0: follow every group of the system; clear: the talk group and the RX group list only. Bits 1-7: 0 |

The talk group is the record's contact, the groups followed its RX group list (group IDs 1-65535). Following every
group is the channel's choice, so one system can have a channel that follows all and others that follow a few.
