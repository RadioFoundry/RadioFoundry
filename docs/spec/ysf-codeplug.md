<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# YSF in the codeplug

The stored records of YSF (C4FM) channels, on the generic layer of [channel-modes.md](channel-modes.md).
The firmware's codec is `core/ysf/ysf_codeplug.c` (`CONFIG_YSF`). The radio core runs a channel of kind 103 where the
build has `CONFIG_YSF` and the board's air interface carries YSF ([ysf.md](ysf.md) 5); elsewhere it is parked
([channel-modes.md](channel-modes.md) 6).

| Id | Kind | System type |
|---|---|---|
| 103 (0x67) | YSF | none |

## 1. Radio-wide record (kind 103)

Key 0x0A00 + 103 = 0x0A67 ([channel-modes.md](channel-modes.md) 5.5):

| Offset | Size | Field |
|---|---|---|
| 0 | 10 | Callsign, ASCII: 0-9, A-Z, `-` and `/`, padded with NUL or spaces. Empty, absent or with another character: the radio name's callsign ([ysf.md](ysf.md) 5.2). Written by the editor or by Settings > My radio > YSF callsign |

The record holds no radio ID, and no record ever will: the radio ID is made from the radio's chip and is never stored,
because a codeplug is copied between radios ([ysf.md](ysf.md) 6). A reader that finds bytes past offset 10 keeps them.

## 2. Channel (kind 103)

The tag ([channel-modes.md](channel-modes.md) 2): no system, flag bit 0 transmit allowed. The parameter block:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Transmit DG-ID, 0-99. Above 99 reads as 0 |
| 1 | 1 | Receive DG-ID, 0-99; 0 = every DG-ID. Above 99 reads as 0 |

A zeroed block, or none, sends DG-ID 0 and hears every DG-ID; with the tag's flag bit 0 clear, receive only.

The base record carries:

| Record field | Use |
|---|---|
| Name, RX and TX frequency, power, TX time-out, scan skip | As any channel |
| Contact, RX group list, per-channel DMR ID, color code, timeslot | Kept, not read: YSF names its caller and us by callsign |
| Mode, receive only | DMR, receive only, always (the safe stand-in) |

### 2.1 Writing from the radio

A YSF channel the radio saves (the VFO in YSF stored as a channel, a mode changed for now made permanent) is written
as [channel-modes.md](channel-modes.md) 2.4 gives: the base record as the safe stand-in, then a new tag of kind 103
with no system, transmit allowed from the channel and a block from the YSF settings (the DG-IDs). When the channel
editor changed the DG-ID ([ui.md](ui.md) 2.1) the edited block is written over the front of the tag's, the rest of
the tag kept. A channel the editor's Mode row made YSF starts with DG-ID 0 both ways.

## 3. Sharing

A zone share carries a YSF channel's tag as it is ([zone-share.md](zone-share.md) 4.2): the block is 2 octets, inside
the share's limit of 64. The radio-wide record (the callsign) is never shared, and there is no radio ID to share.
