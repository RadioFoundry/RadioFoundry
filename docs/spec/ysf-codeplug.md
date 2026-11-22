<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Yaesu System Fusion in the codeplug

The stored records of Fusion (C4FM) channels, on the generic layer of [channel-modes.md](channel-modes.md). What the
fields become on air is [ysf-modem.md](ysf-modem.md) 3. The firmware's codec is `core/ysf/ysf_codeplug.c`
(`CONFIG_YSF`); the Codeplug Editor's is `site/static/js/codeplug/mode_ysf.js`.

| Id | Kind | System type |
|---|---|---|
| 6 | Yaesu System Fusion (short name `YSF`) | none: a Fusion channel names no system |

## 1. Text fields

Every text field is stored as it goes on air: ASCII, padded with spaces to its full length, not terminated. Every
reader cleans a field the same way, so a field is safe whatever its bytes:

| Stored byte | Read as |
|---|---|
| 0x00, 0xFF (a zeroed, erased or short record) | a space |
| `a`-`z` | upper case |
| Anything but `A`-`Z`, `0`-`9`, space, `/` and `-` | a space |

A writer cleans a field the same way before storing it. The characters are the ones callsigns, portable suffixes and
the radio IDs of Yaesu radios use; the firmware keeps every other character out of what it sends.

## 2. Channel (kind 6)

The tag ([channel-modes.md](channel-modes.md) 2): no system (0xFF), no fallback. Flag bit 0, transmit allowed, is set
on a new channel: Fusion is an amateur mode the user transmits on. The base record is the safe stand-in (DMR, receive
only, the channel's frequencies), written before the tag. Flag bits 4-7: 0.

### 2.1 Parameter block

| Offset | Size | Field |
|---|---|---|
| 0 | 10 | Destination: the station or group called. Blank reads as `ALL` (a group call) |
| 10 | 1 | DG-ID sent, 0-99. 0xFF and anything over 99 read as 0 |
| 11 | 1 | DG-ID heard, 0-99; 0 hears every DG-ID. 0xFF and anything over 99 read as 0 |
| 12 | 1 | Flags: bit 0 narrow deviation (FICH Dev = 1; clear: wide); bit 1 PTT sends Voice FR, VW (clear: V/D mode 2, DN; [ysf-modem.md](ysf-modem.md) 4.1). Bits 2-7 reserved: written 0, kept as found. 0xFF reads as 0 |

A zeroed block, or none, is a group call to `ALL` on DG-ID 0, wide, in DN. Either voice mode is received whatever the
flag says; it chooses only what PTT sends. A firmware built without VW (`CONFIG_YSF_VW`) sends DN and keeps the bit. Fields are appended after offset 12.

The base record carries:

| Record field | Use |
|---|---|
| Name, RX and TX frequency, power, TX time-out, scan skip | As any channel. A repeater's split is the record's RX and TX |
| Contact, RX group list, color code, time slot, per-channel DMR ID | Not used |
| Mode, receive only | DMR, receive only, always (the safe stand-in) |

### 2.2 What the radio sends

| On air ([ysf-modem.md](ysf-modem.md) 3) | From |
|---|---|
| CSD1 destination | The channel's destination; a call to `ALL`: `*****` and the radio ID, spaces when none is set (radio ID mode, as Yaesu radios send it) |
| CSD1 source | The radio-wide callsign (3); blank = the radio name (the open-firmware callsign field) |
| CSD2 downlink, uplink | Spaces: a radio sends direct, a repeater fills them (4.2.1 (1) of the standard) |
| CSD3 Rem1-Rem3 | Spaces |
| CSD3 Rem4 (our radio ID) | The radio-wide radio ID (3) |
| FICH call mode | Radio ID mode for a call to `ALL`; otherwise group / CQ |
| FICH Dev | The channel's narrow flag |
| FICH DT, the voice | V/D mode 2 (DN), or Voice FR (VW) when the channel's VW flag is set: then CSD3 goes in the first communication frame ([ysf-modem.md](ysf-modem.md) 4.1) |
| FICH SQL, SC | SQL type 0 and SC = the DG-ID sent (3.2 of the modem spec: where radios carry the DG-ID) |
| FICH MR, VoIP | Direct, local |

A receiver on a channel whose DG-ID heard is not 0 plays only transmissions whose SC carries that DG-ID; the callsigns
of the others are still shown as heard.

## 3. Radio-wide record (kind 6)

Key 0x0A00 + 5 ([channel-modes.md](channel-modes.md) 5.5):

| Offset | Size | Field |
|---|---|---|
| 0 | 10 | Callsign. Blank = the radio name, which the radio core supplies |
| 10 | 5 | Radio ID: sent in CSD3 Rem4, and in a call to `ALL` in the destination field (radio ID mode). Blank = spaces in both, and calls to `ALL` still go in radio ID mode |

Absent, zeroed or short: every field missing or zeroed reads blank. Fields are appended after offset 14.
