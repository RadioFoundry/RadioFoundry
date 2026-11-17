<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DMR Tier III in the codeplug

The stored records of a DMR Tier III trunked system and its channels, on the generic layer of
[channel-modes.md](channel-modes.md): kind 1 and system type 1. A build with `CONFIG_DMR_TRUNK` reads them
(`core/radio/radio_trunk_cp.c`); what the radio does with them is in [dmr-trunking.md](dmr-trunking.md) 12. The
Codeplug Editor writes them and zone sharing carries them. Little endian throughout. Host test:
`tests/host/t3CodeplugTest.c`.

The radio runs the Tier III systems numbered 0 to `RADIO_T3_SYSTEMS` - 1 (0 to 7). A Tier III system record at a
higher number is kept and not run: its channels are unavailable.

## 1. System (type 1)

The generic header ([channel-modes.md](channel-modes.md) 7.1), with:

| Header field | Tier III use |
|---|---|
| Flag bit 0 (needs its secret) | The system authenticates: usable only with a key (3) |
| Flag bit 1 (transmit side) | Set: the radio registers, affiliates and transmits. Clear: listen only, the radio follows the control channel and never transmits there |
| Identity length | 4: network model, NET, category |
| Own ID | The radio's DMR ID on the system, 0 = the radio's |
| Fallback | The system's fallback channel and its name check |

The type's block:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Network model: 0 tiny, 1 small, 2 large, 3 huge |
| 1 | 2 | NET |
| 3 | 1 | Category: 1 A, 2 B |
| 4 | 1 | Color code, 0-15 |
| 5 | 2 | Options: 0x0002 follow all-calls, 0x0004 follow emergency calls on any talkgroup, 0x0020 follow private calls, 0x0040 register with the talkgroups, 0x0080 accept Ambient Listening, 0x0100 accept a kill, 0x0200 no roaming, 0x0400 accept a stun, 0x1000 off-net IP through the system's IP gateway. 0x0001 and 0x0800 are reserved (written 0, kept): listen only and authentication are the header's flags |
| 7 | 1 | Fallback hold time in seconds, 0 = 30 |
| 8 | 4 | Fixed plan: MS transmit Hz of the first channel, 0 = no fixed plan |
| 12 | 2 | Fixed plan: the first channel's LCN |
| 14 | 1 | Fixed plan: separation code |
| 15 | 1 | Plan flags: bit 0 MS transmit below MS receive; bit 1 don't learn channels from the control channel |
| 16 | 4 | Fixed plan: duplex split Hz |
| 20 | 1 | Weak below this dBm (signed; 0 = the radio's roaming threshold) |
| 21 | 1 | Power save asked for at registration: 0 off, 1-7 = 1:2 to 1:128 |
| 22 | 1 | Sites the radio may use: how many, 0 = any |
| 23 | 16 | Those site values, u16 each, 8 at most |

A system is **usable** when its block holds at least the 23 octets up to the site count, it has a control channel
and, if flag bit 0 is set, a key. A block that ends inside the site values names the sites it holds. Option bits a
codeplug may not set (the debug link's, and the reserved ones) are read as 0.

## 2. System channels

Generic entries ([channel-modes.md](channel-modes.md) 7.2) of class 0, group 0xFF. Flag bit 0 marks a control
channel, hunted in entry order. Body: LCN (2), MS receive Hz (4), MS transmit Hz (4), then one octet: the color code,
0xFF = the system's. An entry shorter than this body is skipped. A system takes its first `T3_CTRL_MAX` control
channels among its first `RADIO_T3_CHANS` (8) channels in entry order; the rest are skipped. A system whose only control
channels come after them has none: it is not usable.

## 3. Key

Key 0x0300 + n: K (16 octets, the first the most significant), then the PSN (3 octets, most significant first; absent
or 0: none). Shorter than 16 octets: no key.

## 4. Channels (kind 1)

The channel tag ([channel-modes.md](channel-modes.md) 2) names the system. Tag flag bit 0 is transmit allowed
(a site grants every transmission; set by default). Bit 2 with the fallback fields: the channel's own fallback;
kind bit 4: no fallback on this channel. No parameter block.

| Tag | The channel |
|---|---|
| Transmit allowed, on a usable system with its transmit side | registers, affiliates and transmits as the site grants |
| Transmit flag clear (a zeroed tag too) | follows the system and its calls; never registers, affiliates or transmits, and PTT is refused |
| On a listen-only system (1) | the same, whatever the tag says |
| No system (0xFF), a system that is absent, of another type, not usable or past the systems the radio runs, or no ID to register with (the record's own ID and the radio's DMR ID both 0) | **unavailable**: nothing starts, the channel is receive only on its record's frequency and PTT is refused |

The base record carries the talk target (contact: a talkgroup, a private contact or the all-call contact) and the RX
group list (the talkgroups followed), the name, power, TX time-out and talker alias. Its frequencies are the system's
first control channel's. An all-call contact in the RX group list follows all-calls on that channel only; the system's
option (1) follows them on all its channels. The talk target's talkgroup is always followed: when the RX group list
fills the radio's 32 talkgroups, it takes the last place. Another channel of the system running takes its talk target
and talkgroups without leaving the call or control channel the radio is on.

## 5. Short data poll and supplementary data

Key 0x0800 + n: system n's short data poll setting: UDT format (1), blocks to ask for (1, 1-4), the reply's octets
(0-46). Key 0x0900 + n: the supplementary user data offered with each call on system n: UDT format (1), the octets
(1-46).

## 6. Sharing

A system's sharing level ([channel-modes.md](channel-modes.md) 7.1, [zone-share.md](zone-share.md)): identity only
carries the type, flags and identity; the whole system carries the block, the control channels and the key, the key
only when the sender turns keys on for that share (a radio has no such switch yet: [zone-share.md](zone-share.md)).
Never carried: the own ID, the poll setting and the supplementary data.
