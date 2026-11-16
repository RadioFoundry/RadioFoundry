<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# P25 trunking, receive only

A listener for a P25 Phase 1 trunked system: it decodes the control channel's outbound signaling, hunts for the
control channel and holds it, and follows voice channel grants for chosen talk groups to the conventional call layer
([p25.md](p25.md)), coming back when the call ends. Switch: `CONFIG_P25_TRUNK`, which needs `CONFIG_P25`
([build-config.md](../build-config.md)). Code: `core/p25/p25_tsbk.c` (blocks and messages), `core/p25/p25_follow.c`
(the follower), the trunking hooks of `core/p25/p25_rx.c` and `core/p25/p25_engine.c`.

It never transmits. It does not register, affiliate, request, acknowledge or answer anything; the transmit path of a
trunked channel stays off.

State:

| Part | State |
|---|---|
| Trunking signaling units (TSDU) and the header of a multi-block trunking packet: receive, FEC, CRC | written, host tested |
| Messages a listener needs (2) | written, host tested |
| The follower: hunt, hold, follow, return (4) | written, host tested, also through the soft modem with noise |
| The radio core: a trunked channel, its retunes, the UI | written, host tested (6) |
| On air | not run on a radio |
| Transmit, registration, affiliation; TDMA (Phase 2) voice channels; roaming by signal level; multi-block message bodies | not written |

Sources: published descriptions of TIA-102.AABB (trunking control channel formats), TIA-102.AABC (trunking messages)
and TIA-102.BAAA-A (the common air interface): test equipment application notes and public opcode references. Facts
no source here confirmed are listed in 7; the code treats them conservatively.

## 1. Units

A control channel sends trunking signaling data units (TSDU, data unit ID `0x7`) back to back. A unit is the frame
sync, the NID and one to three blocks. A multi-block trunking packet is a packet data unit (PDU, `0xC`) whose header
block names the trunking formats.

| Item | Value |
|---|---|
| Block | 12 octets: 80 message bits and a CRC-CCITT over them (octets 10, 11) |
| On air | each block as 196 bits of the rate 1/2 trellis code (the same code and interleave as a packet's), after the NID |
| Unit length | the blocks, filled with null bits to the next status symbol: 1 block 180 symbols, 2 blocks 288, 3 blocks 360 |
| Last block | bit 7 of octet 0 (LB). A TSDU holds at most three blocks |
| Status symbols | as in every unit, one after each 70 information bits |

The NID does not give a TSDU's length. The receiver (`p25_rx.c`, `rx_head`) decodes the first block as soon as its
symbols are in and asks the framer for the next one until a block says it is the last, or the third is in. A block
whose CRC fails still gives its best guess of the flag. For a packet, the header block gives the count of blocks that
follow; a header whose CRC fails, or a packet longer than the unit buffer (eight blocks), is not taken.

TSDU and PDU are taken only on a trunked channel (`p25_engine_cfg_t.trunk`); a conventional channel refuses them as
before. They go to the engine's `rx_unit` hook and never to the call layer.

## 2. Messages

Octet 0: LB (bit 7), protected (P, bit 6), opcode (bits 5-0). Octet 1: the manufacturer's ID (MFID). Octets 2-9: the
arguments. Bit positions below count from the first argument bit.

| Opcode | Message | Fields read | Use |
|---|---|---|---|
| `0x00` | group voice channel grant | service options 8, channel 16, group 16, source 24 | follow |
| `0x02` | group voice channel grant update | channel 16, group 16, channel 16, group 16 | follow (either pair) |
| `0x03` | group voice channel grant update, explicit | service options 8, reserved 8, channel (T) 16, channel (R) 16, group 16 | follow channel (T) |
| `0x04` | unit to unit voice channel grant | channel 16, target 24, source 24 | follow, calls to or from `unit_id` |
| `0x06` | unit to unit voice channel grant update | as `0x04` | as `0x04` |
| `0x33` | identifier update, TDMA | identifier 4, channel type 4, transmit offset sign 1 + 13, spacing 10, base 32 | plan |
| `0x34` | identifier update, VHF/UHF | identifier 4, bandwidth code 4, transmit offset sign 1 + 13, spacing 10, base 32 | plan |
| `0x3D` | identifier update | identifier 4, bandwidth 9, transmit offset sign 1 + 8, spacing 10, base 32 | plan |
| `0x3B` | network status broadcast | LRA 8, WACN 20, system 12, channel 16, service class 8 | identity |
| `0x3A` | RFSS status broadcast | LRA 8, flags 4, system 12, RFSS 8, site 8, channel 16, service class 8 | identity, view |
| `0x3C` | adjacent site status broadcast | as `0x3A` | learn a control channel |
| `0x39` | secondary control channel broadcast | RFSS 8, site 8, channel 16, class 8, channel 16, class 8 | learn a control channel |
| `0x29` | secondary control channel broadcast, explicit | RFSS 8, site 8, channel (T) 16 | learn a control channel |
| `0x38` | system service broadcast | services available 24, supported 24, priority 8 | view only |

Service options: bit 7 emergency, bit 6 protected (encrypted), bit 5 duplex, bit 4 packet mode, bits 2-0 priority.
In the explicit forms the channel (T) is where the site transmits, which is where a listener tunes.

Not decoded, counted by opcode in the view and skipped: every other opcode of the standard table; any block with a
MFID other than `0x00` or `0x01` (a manufacturer's own message, whatever its opcode); any protected block (its
arguments are encrypted). A multi-block trunking packet's header is recognized (format `0x15` or `0x17`, service
access point 61 or 63) and its opcode kept in the view; its data blocks are not decoded.

## 3. Channels to frequencies

A channel number is an identifier (bits 15-12) and a channel (bits 11-0). The identifier's update gives a base
frequency (units of 5 Hz) and a channel spacing (units of 125 Hz):

    frequency = base + channel x spacing

On a TDMA identifier the channels share carriers: the carrier is channel / slots, its frequency base + carrier x
spacing. Worked examples (`tests/host/p25TrunkTest.c`):

| Identifier | Channel | Frequency |
|---|---|---|
| base 851.00625 MHz, spacing 6.25 kHz | `0x1001` | 851.0125 MHz |
| the same | `0x1FFF` | 876.6 MHz |
| base 136 MHz, spacing 2.5 kHz | `0x0010` | 136.04 MHz |
| base 769.00625 MHz, spacing 12.5 kHz, 2 slots | `0x2006`, `0x2007` | 769.04375 MHz |

A frequency past 32 bits, or an identifier with no base or spacing, gives none: the channel is not followed.
Identifiers come from the configuration (programmed) and from the air (learned). A programmed one is never replaced
off air; with `P25_FOLLOW_PLAN_ONLY` nothing is learned.

## 4. The follower

`p25_follow_t`, 332 bytes on an Arm target, placed by the caller. Flash: about 3.7 KB for the follower and the
messages, 0.8 KB for the receive side's trunking units (the trellis decoder among them). Pure: time comes in as a
millisecond tick on every call, retunes go out through `p25_follow_hooks_t.tune`. All calls come from one task (or
one lock).

| State | What it does | Leaves |
|---|---|---|
| HUNT | tunes the control channel candidates in turn: the programmed ones (`cfg.cc`, first the primary), then the ones learned off air (adjacent sites, secondary channels of this system), from the last one that answered | a candidate decodes and is the system: CONTROL |
| CONTROL | reads every block; learns identifiers and control channels; follows grants it wants | a grant followed: VOICE. Nothing decoded for 1 s, or the network status names another system: HUNT |
| VOICE | the conventional call layer plays the call | a terminator, no voice for 1 s, the longest call, another call on the channel, an encrypted call skipped: CONTROL |

Hunting:

| Step | Time | Then |
|---|---|---|
| A candidate with nothing decoded | 600 ms | the next |
| A candidate with signaling but not yet the system's identity (WACN and system ID, when configured) | 4 s | the next |
| A block of another NAC or another system | at once | the next |
| A whole pass without an answer | wait on the last good candidate 2 s, doubled each pass up to 30 s | a new pass |
| No control channel for `fallback_ms` | once per loss | the `fallback` hook (the caller may move to a fallback channel) |

A candidate without a frequency (a channel number whose identifier is not known yet) is passed over. With WACN and
system ID set to any, the first block decoded on a candidate of the right NAC holds it. RFSS status names the
system but not the WACN: it settles a candidate only when the WACN is any.

Following a grant (group: `talk_group`, the `groups` list, or every group with `P25_FOLLOW_ALL_GROUPS`; unit to unit:
to or from `unit_id`, unless `P25_FOLLOW_NO_UNIT`):

| Grant | Result |
|---|---|
| Service options say protected, and `P25_FOLLOW_SKIP_ENC` | not followed (view: refused ENC). Its repeated grants and its updates are skipped while they keep coming (1.5 s apart at most) |
| Its identifier unknown | not followed (view: refused NO_IDEN); followed once the identifier arrives |
| A TDMA identifier other than single-slot full rate | not followed (view: refused TDMA): the voice path plays Phase 1 only |
| A call left on its terminator | its updates are not followed for 1.5 s; a new grant is |
| `hold_ms` after a followed group call | only that group is followed |
| Otherwise | `tune(hz, GROUP or UNIT, group or target)`, state VOICE |

On the voice channel the follower watches the call layer's fields (`rx_call`): a group call to another group sends
it back (the channel was given to another call), an ALGID other than clear marks the call encrypted and, with
`P25_FOLLOW_SKIP_ENC`, sends it back and skips that call's grants and updates as above. The engine never plays
encrypted voice in any case.

Timers (`p25_follow.h`): dwell 600 ms, identity 4 s, control channel lost 1 s, backoff 2 s to 30 s, rejoin 1.5 s,
voice lost 1 s (`cfg.voice_lost_ms`), longest call 120 s (`cfg.call_max_ms`).

The view (`p25_follow_view`, a consistent copy for another task): state, the control channel tried or held, its NAC,
WACN, system, RFSS, site, the call followed (group, source, target, encrypted), identifiers known, control channels
learned, the last grant refused and why, and counters: blocks, CRC failures, manufacturer's blocks, packets, grants,
follows, refusals by reason, wrong system, hunts, control channel losses, voice losses, and blocks by opcode.

## 5. Configuration

`p25_follow_cfg_t`, from a trunked channel's codeplug record:

| Follower | Codeplug (`p25_trunk_cfg_t`) |
|---|---|
| `wacn`, `sysid`, `nac` | `sys.wacn`, `sys.sysid`, `sys.cc_nac` (their "any" values are any here too) |
| `flags`: `SKIP_ENC`, `PLAN_ONLY`, `ALL_GROUPS` | `sys.options`, `follow_all` |
| `unit_id`, `talk_group` | `unit_id` (the P25 settings' `unit_id` first, as on a conventional channel), `talk_group` |
| `cc[]`: `hz`, `chan`, `site` | `ctl[]`: `rx_hz`, `chan`; `ctl_site[]` |
| `iden_set`, `iden[]`: base, spacing, slots, TDMA | `iden_set`, `iden[]` (bandwidth and transmit offset are not needed to listen) |
| `hold_ms` | `sys.hold_s` x 1000 (0: no hold) |
| `fallback_ms` | none yet: 0, never (the fallback channel is read, not used) |
| `groups[]`, `voice_lost_ms`, `call_max_ms`, `P25_FOLLOW_NO_UNIT` | none yet |

Not used by a listener: `sys.roam_dbm`, `listen_only` (it is always), the control channels' transmit frequencies.

## 6. The radio core

`core/radio/radio_p25.c` runs the follower on a P25 trunked channel (kind 3, [channel-modes.md](channel-modes.md) 4)
where the board's air interface receives P25. It is the conventional channel's path ([p25.md](p25.md) 7) with these
differences:

| Step | What happens |
|---|---|
| Read | `radio_p25_take` makes the channel P25; the radio core tunes it as any P25 channel (the record's receive frequency, 12.5 kHz, the DMR stack stopped) |
| Begin | `radio_p25_begin` reads the system (`p25_trunk_load`) into the part of the P25 memory the voice path takes next, and makes the follower's configuration (5) and the engine's: `cfg.trunk` set, both NACs the control channel's (or any), our unit ID, the channel's talk group. Then the voice path, the follower (its first tune), the engine. A system that doesn't load: no engine (`p25.begin_fails`) |
| Units | The engine's `rx_unit` and `rx_call` hooks hand every unit and every call's fields to the follower, in the provider's task |
| Timers | The radio task ticks the follower at its own tick (10 ms), so its timers run whether the provider delivers or not (a provider that receives only at a sync delivers nothing on a dead channel) |
| Retune | The `tune` hook only records the request. The radio task carries it out at its next tick: the receiver to the frequency (12.5 kHz, no tone), the engine's selective squelch (`rx_tgid`) to the group for a group call and 0 otherwise (`p25_engine_set`), `p25_engine_retuned`, then `p25_follow_retuned`. A frequency outside the board's receive bands, or in a listen-only band, is not tuned (`p25t.untuned`): the follower hears nothing there and goes back |
| PTT | Refused with "RX only" whatever the record says (`TXDENY_RX_ONLY`); `radio_p25_tx_prepare` refuses as well. Nothing in this path keys the transmitter |
| Leave | Another channel, the scan, the bench or anything else that takes the modem ends the engine, then the follower |

One task at a time runs the follower, without a lock: each task sets its own flag, then looks at the other's. The task
that finds the other's flag set clears its own and leaves. On one core the task that runs second always sees the
first one's flag, so the two are never inside together. The provider's task then drops that unit or call
(`p25t.skips`; the control channel repeats itself), the radio task tries again at its next tick.

Memory: the follower, its configuration and its control channel and identifier tables sit beside the engine in the
memory the board places for P25 (the vocoder's, where it lends it); the system read from the codeplug shares the voice
path's part, which it needs only until the voice path starts. The radio task keeps a copy of the follower's view for
other tasks (about 150 bytes).

What the user sees: `p25.trunk` in the radio view (`RADIO_P25_TRUNK_*`: finding the system, on the control channel,
following a call). Home's talk line says "Finding system", "Control channel" or "Following TG 201" (the grant's group
until the call's own fields are in) instead of what PTT calls. The debug link's `p25_trunk` section has the follower's
view and the retune counters ([debuglink.md](debuglink.md) 3.7).

Not used yet: the fallback channel, an RX group list (`groups`), the system's `roam_dbm`.

Tests: `radioCoreTestP25T` (host) runs the radio core against a trunked site on the fake air interface: it finds the
system on the primary control channel, refuses PTT, follows a grant for its talk group to the voice channel and plays
the call there, comes back, leaves another group's grant and an encrypted one (skip on), does not tune a channel
outside the radio's bands, finds the system on a second control channel programmed by number, stops with the channel
and hunts again on return. `tests/hil/test_p25.py`, `test_p25_trunk_follow` (opt-in, receive only): a stored trunked
channel on a live system; not yet run on a radio.

## 7. Not confirmed

These come from no source available here, or from one only in part. The code treats each conservatively; a capture
of a live control channel settles them.

| Fact | What the code does |
|---|---|
| The block CRC: register starting at 0, the remainder inverted | as stated. A wrong guess fails every block, which a capture shows at once |
| A TSDU or packet is padded with null bits to the next status symbol | as stated (180, 288, 360 symbols) |
| MFID `0x01` uses the standard table | taken as the standard's |
| The TDMA identifier's channel types: slots per carrier (types 0-2 one, 3 two, 4 four, others two) | only type 1 counts as FDMA; every other type is refused as TDMA |
| The transmit offset's unit in the VHF/UHF and TDMA forms, and the sense of its sign bit | kept as sent; a listener does not use it |
| The explicit secondary control channel form's layout | only its channel (T) is read |
| The system service broadcast's layout | view only |
| The four flag bits ahead of the system ID in RFSS and adjacent site status | kept as sent, not used |
| The multi-block packet header: format in octet 0, SAP in octet 1, MFID in octet 2, blocks to follow in octet 6, opcode in octet 7 | used to recognize the header and count it only |
