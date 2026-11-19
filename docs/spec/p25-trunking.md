<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# P25 trunking

A subscriber for a P25 Phase 1 trunked system. It decodes the control channel's outbound signaling, hunts for the
control channel and holds it, and follows voice channel grants for chosen talk groups to the conventional call layer
([p25.md](p25.md)), coming back when the call ends. With the transmit side on, it also registers, affiliates to its
talk group, asks for a voice channel on PTT and transmits on the one granted. Switch: `CONFIG_P25_TRUNK`, which needs
`CONFIG_P25` ([build-config.md](../build-config.md)). Code: `core/p25/p25_tsbk.c` (blocks and messages),
`core/p25/p25_follow.c` (the follower and its transmit side), the trunking hooks of `core/p25/p25_rx.c` and
`core/p25/p25_engine.c`, `core/radio/radio_p25.c` and `core/radio/radio_sm.c` (the radio core).

State:

| Part | Written | Host tested | Run on a radio |
|---|---|---|---|
| Trunking signaling units (TSDU) and the header of a multi-block trunking packet: receive, FEC, CRC | yes | yes | the bench's control channel (8): 152 blocks with 1 CRC failure while only listening; one or two blocks lost around each of its own bursts |
| Outbound messages a listener needs (2) | yes | yes | the bench's: network and RFSS status, the identifier, grants |
| The follower: hunt, hold, follow, return (4) | yes | yes, also through the soft modem with noise | `test_p25_trunk_rx`: a grant to another unit's call followed to a second bench transmitter, its voice received and played, back on the control channel after the terminator |
| The radio core: a trunked channel, its retunes, the UI (7) | yes | yes | yes, a channel written over CPS |
| Inbound requests and their answers (2.1) | yes | yes | the three requests decoded off air by `p25tool.py`; the bench's answers taken |
| Registration, affiliation, voice request, transmit on the grant, return (5) | yes | yes (`radioCoreTestP25T`, a fake site) | `test_p25_trunk_tx` |
| Bench control channel and inbound decode (`tools/bench/p25tool.py`, 8) | yes | `p25tool.py selftest` | yes |
| Unit to unit calls out, emergency, deregistration, location registration, acknowledgements, roaming by signal level, TDMA (Phase 2) voice channels, multi-block message bodies | no | | |

The radio runs are against a control channel `p25tool.py` writes from the same descriptions as the firmware, so they
check the radio's timing, retunes and transmissions, not the layouts: no live system has been heard or answered.
Every message layout of the transmit side comes from published descriptions, not a capture: 9 lists them.

Sources: published descriptions of TIA-102.AABB (trunking control channel formats), TIA-102.AABC (trunking messages)
and TIA-102.BAAA-A (the common air interface): test equipment application notes and public opcode references. Facts
no source here confirmed are listed in 9; the code treats them conservatively.

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

Answers to one unit, read when the transmit side is on (5):

| Opcode | Message | Fields read (bit offsets in the arguments) |
|---|---|---|
| `0x2C` | unit registration response | reserved 2, RV 2 @2, system 12 @4, unit ID 24 @16, address 24 @40 |
| `0x28` | group affiliation response | LG 1 @0, reserved 5, GAV 2 @6, announcement group 16 @8, group 16 @24, target 24 @40 |
| `0x27` | deny response | AIV 1 @0, EX 1 @1, service type 6 @2, reason 8 @8, additional information 24 @16, target 24 @40 |
| `0x21` | queued response | as `0x27` |

RV and GAV: 0 accepted, 1 failed (try again), 2 denied, 3 refused. The service type of a deny or queued response is the
opcode of the request it answers.

Not decoded, counted by opcode in the view and skipped: every other opcode of the standard table; any block with a
MFID other than `0x00` or `0x01` (a manufacturer's own message, whatever its opcode); any protected block (its
arguments are encrypted). A multi-block trunking packet's header is recognized (format `0x15` or `0x17`, service
access point 61 or 63) and its opcode kept in the view; its data blocks are not decoded.

### 2.1 Inbound requests

A radio sends one block at a time, in a TSDU of one block (180 symbols), its status symbols "unknown"
(`p25_isp_unit`). Inbound opcodes share numbers with outbound ones of another meaning; `p25_isp_parse` reads them.
MFID `0x00`, LB set.

| Opcode | Request | Fields sent (bit offsets in the arguments) |
|---|---|---|
| `0x2C` | unit registration request | reserved 8, WACN 20 @8, system 12 @28, unit ID 24 @40 |
| `0x28` | group affiliation request | reserved 12, system 12 @12, group 16 @24, unit ID 24 @40 |
| `0x00` | group voice service request | service options 8 @0, reserved 16, group 16 @24, unit ID 24 @40 |

The registration request carries the WACN and system ID the control channel names (or the configured ones). Service
options are 0 (no emergency, clear, half duplex, priority 0). The affiliation request's reserved bits are 0; it
carries no emergency flag. Its group and unit ID places were confirmed on a live control channel, which read them
one octet early when they were packed at bits 16 and 32.

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

Where the radio transmits (the inbound side of a channel) is its receive frequency plus the identifier's transmit
offset. The `0x3D` form gives the offset in 250 kHz steps; the VHF/UHF (`0x34`) and TDMA (`0x33`) forms are taken in
channel spacings (9). A programmed identifier's offset (`tx_offset_hz`) is kept in 250 kHz steps where it divides,
else in spacings; a programmed identifier with no offset takes the one the air gives. A control channel's inbound
frequency is the configured `ctl[].tx_hz`, else its channel number (programmed, or from the network and RFSS status
broadcasts) through the identifier table. A channel with no inbound frequency is not transmitted on.

## 4. The follower

`p25_follow_t`, 452 bytes on an Arm target (332 without the transmit side's state, which is always compiled in with
the switch), placed by the caller. Flash: about 3.7 KB for the follower and the
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
| A group grant whose source is our own unit | not followed: not a call to listen to. As the answer to our voice request it is taken by the transmit side (5) |
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

## 5. The transmit side

On with `P25_FOLLOW_TX` (the channel's tag allows transmit, the system is not listen only, a unit ID and a talk group
are set). The follower builds every inbound block and hands it to the `send` hook with the inbound frequency; the
caller keys for it (7). It keeps one request in hand at a time.

Registration (`view.reg`, `P25_REG_*`):

| State | What it does | Leaves |
|---|---|---|
| OFF | the transmit side is off, or `P25_FOLLOW_NO_REG` | |
| IDLE | waits for a control channel held and its RFSS status heard (or 2 s on it) | sends the registration request: WAIT |
| WAIT | registration sent | accepted: sends the affiliation request, AFF. Failed: RETRY. Denied or refused: DENIED. No answer after 3 sends: RETRY |
| AFF | affiliation sent | accepted for our group: READY. Failed: RETRY. Denied or refused: DENIED |
| READY | registered and affiliated | another site (system, RFSS or site differs from the one registered): IDLE |
| RETRY | waits 10 s, doubled each round up to 60 s | registers again: WAIT |
| DENIED | final: PTT says why | a channel restart or another site |
| NO_TX | the control channel has no inbound frequency | |

Losing the control channel and finding the same site again keeps the registration. Hunting resets the site to unknown.

Sending (`acc_tick`): a block goes out only after a block of the control channel was heard since the tune, so its
status symbols are known; while the last status symbol of the last unit said the inbound channel is busy, it waits
20 ms plus up to 60 ms at random and looks again (`view.inb_busy`). An unanswered block is sent again after 1 s plus
100-400 ms at random, three sends in all. A deny or queued response must name the request's opcode and our unit.
An answer counts only for a request that went out at least once: a registration or affiliation response, a deny, a
queued response or a grant heard while the request still waits for its turn is not ours and changes nothing.

PTT (`p25_follow_ptt`, `view.req`, `P25_REQ_*`):

| Step | What happens |
|---|---|
| Can it ask? | `p25_follow_can_talk`: a control channel held, and READY (or `NO_REG`). Else PTT is refused with the reason (7) |
| Request | the group voice service request for `talk_group`: WAIT. On a voice channel (following a call) the follower first goes back to the control channel; there is no keying on a channel left over from another call |
| Queued | a queued response: QUEUED, waiting up to 10 s for the grant |
| Grant | a group voice grant (`0x00`) whose source is our unit and whose group is ours: TALK. The follower tunes the channel (`P25_FOLLOW_TUNE_TALK`) and gives the transmit frequency (`view.talk_hz`). A channel without an inbound frequency, or on a TDMA identifier: NO_CHAN |
| Other grants | while the request waits, no other call is followed |
| Talk | on our channel the follower ignores voice loss and terminators (the radio is the one talking); after the longest call plus 10 s it goes back regardless |
| End | `p25_follow_talk_end` (the terminator went out): back to the control channel. Grants of our own call that the site keeps repeating are skipped for 1.5 s |
| Released early | before the grant the request is dropped |
| Failure | deny: DENIED (`deny_svc`, `deny_reason`); no answer after 3 sends, or queued too long: TIMEOUT |

Timers (`p25_follow.h`, `P25_ACC_*`): site 2 s, answer 1 s, three sends, resend gap 100 ms plus up to 300 ms, busy
20 ms plus up to 60 ms, registration retry 10 s doubled up to 60 s, queue 10 s, talk the longest call plus 10 s.

View additions: `reg`, `req`, `deny_svc`, `deny_reason`, `aff_tg`, `talk_hz`, and counters of registrations sent and
accepted, affiliations sent and accepted, requests sent and granted, denials, queued responses, timeouts, sends the
caller refused, and waits on a busy inbound channel.

## 6. Configuration

`p25_follow_cfg_t`, from a trunked channel's codeplug record:

| Follower | Codeplug (`p25_trunk_cfg_t`) |
|---|---|
| `wacn`, `sysid`, `nac` | `sys.wacn`, `sys.sysid`, `sys.cc_nac` (their "any" values are any here too) |
| `flags`: `SKIP_ENC`, `PLAN_ONLY`, `ALL_GROUPS`, `NO_REG` | `sys.options` (bits 0, 1, 2), `follow_all` |
| `flags`: `TX` | the tag allows transmit, not `listen_only`, `unit_id` and `talk_group` set |
| `unit_id`, `talk_group` | `unit_id` (the P25 settings' `unit_id` first, as on a conventional channel), `talk_group` |
| `cc[]`: `hz`, `tx_hz`, `chan`, `site` | `ctl[]`: `rx_hz`, `tx_hz`, `chan`; `ctl_site[]` |
| `iden_set`, `iden[]`: base, spacing, slots, TDMA, transmit offset | `iden_set`, `iden[]` (bandwidth is not used) |
| `hold_ms` | `sys.hold_s` x 1000 (0: no hold) |
| `fallback_ms` | none yet: 0, never (the fallback channel is read, not used) |
| `groups[]`, `voice_lost_ms`, `call_max_ms`, `P25_FOLLOW_NO_UNIT` | none yet |

Not used: `sys.roam_dbm`.

## 7. The radio core

`core/radio/radio_p25.c` runs the follower on a P25 trunked channel (kind 3, [channel-modes.md](channel-modes.md) 4)
where the board's air interface receives P25. It is the conventional channel's path ([p25.md](p25.md) 7) with these
differences:

| Step | What happens |
|---|---|
| Read | `radio_p25_take` makes the channel P25; the radio core tunes it as any P25 channel (the record's receive frequency, 12.5 kHz, the DMR stack stopped) |
| Begin | `radio_p25_begin` reads the system (`p25_trunk_load`) into the part of the P25 memory the voice path takes next, and makes the follower's configuration (6) and the engine's: `cfg.trunk` set, both NACs the control channel's (or any), our unit ID, the channel's talk group. Then the voice path, the follower (its first tune), the engine. A system that doesn't load: no engine (`p25.begin_fails`) |
| Units | The engine's `rx_unit` and `rx_call` hooks hand every unit and every call's fields to the follower, in the provider's task |
| Timers | The radio task ticks the follower at its own tick (10 ms), so its timers run whether the provider delivers or not (a provider that receives only at a sync delivers nothing on a dead channel) |
| Retune | The `tune` hook only records the request. The radio task carries it out at its next tick: the receiver to the frequency (12.5 kHz, no tone), the engine's selective squelch (`rx_tgid`) to the group for a group call and 0 otherwise (`p25_engine_set`), `p25_engine_retuned`, then `p25_follow_retuned`. A frequency outside the board's receive bands, or in a listen-only band, is not tuned (`p25t.untuned`): the follower hears nothing there and goes back |
| PTT, transmit side off | Refused with "RX only" (`TXDENY_RX_ONLY`): the tag is receive only, the system listen only, or no unit ID or talk group |
| PTT, transmit side on | `tx_check` as for any channel except the frequency checks, which wait for the grant. `radio_p25_trunk_ptt` asks the follower; while it waits, home says "Wait" or "Queued" and nothing keys. On the grant (`radio_p25_trunk_talk`: GO) the radio core checks the granted transmit frequency against the board's bands and listen-only bands, then keys as a conventional P25 call there (`p25_key`): the channel's power, the TX timer, the engine's call with our unit ID and talk group and the control channel's NAC (the one heard when the system's NAC is any). A refusal says why (`TXDENY_TRUNK`, the view's `p25.twhy`): no control channel, not registered, denied, no answer, voice channel unusable |
| Release | the terminator goes out (`AIR_TX_DONE`), then `radio_p25_trunk_talk_end` sends the follower back. Released before the grant: the request is dropped |
| Inbound bursts | between calls, a block the follower asked to send is keyed alone on the inbound frequency (`p25t_burst`): the same checks as PTT (inhibit, battery, the band), lowest of nothing, the channel's power; `p25_engine_tx_burst` sends the one TSDU and the transmitter drops at its end. The view stays idle (no red LED, no TX timer). A refused burst counts `p25t.burst_refused` and the follower's `send_fail` |
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
until the call's own fields are in) instead of what PTT calls. With the transmit side on it says "Registering" until
registered, "TG 201" when PTT can ask, "Wait" or "Queued" while a request waits, "Denied" or "Reg. denied" after a
refusal. The debug link's `p25_trunk` section has the follower's
view and the retune counters ([debuglink.md](debuglink.md) 3.7).

Not used yet: the fallback channel, an RX group list (`groups`), the system's `roam_dbm`.

Memory: the transmit side adds 120 bytes to the follower (lent with it) and 28 bytes to the radio task's copy of the
view, plus the radio core's request state (`p25_trunk_t`: the burst in hand, 12 bytes, and a few flags).

Tests: `radioCoreTestP25T` (host) runs the radio core against a trunked site on the fake air interface: it finds the
system on the primary control channel, refuses PTT on a listen-only system, follows a grant for its talk group to the voice channel and plays
the call there, comes back, leaves another group's grant and an encrypted one (skip on), does not tune a channel
outside the radio's bands, finds the system on a second control channel programmed by number, stops with the channel
and hunts again on return. With transmit on, the fake site reads the radio's inbound bursts and answers: registration
and affiliation, PTT refused before them, a request granted (the radio keys on the voice channel's inbound frequency
with its unit and group, then comes back), a denied and a queued request, no answer (three sends, then a timeout), a
grant to a channel outside the bands. `p25TrunkTest` checks the inbound layouts and the follower's access timers.
`tests/hil/test_p25.py`: `test_p25_trunk_follow` (opt-in, receive only) on a live system; `test_p25_trunk_tx` (opt-in,
the bench and the radio transmit) against a control channel the bench plays (8); and `test_p25_trunk_rx` (opt-in, two
bench transmitters, the radio only receives on a listen-only system): the control channel grants another unit's call
while a second transmitter plays that call on the voice channel. The last two have run on a radio; the first needs a
live system.

## 8. Bench tools

`tools/bench/p25tool.py` (the firmware's own code built for the host) writes a control channel for the bench to play
and reads what a radio sends. Dibit files go to the bench's fsk4 transmitter (`tools/bench/fsk4_bench.py tx`);
captures come from its FM discriminator (`fsk4_bench.py disc`).

| Command | Does |
|---|---|
| `p25tool.py cc site.dibits --expect site.json --nac 293 --wacn BEE00 --sysid 2F3 --ch 1004 --base 851006250 --spacing 6250 --tx-offset -45000000 --units 27` | broadcasts only: network and RFSS status, the identifier with its inbound offset, an adjacent site, a secondary control channel |
| `... --reg 1234567 --reg-at 0` | a registration response accepting unit 1234567 in every unit (`--reg 1234567:2`: denied) |
| `... --aff 1234567:201 --aff-at 0` | an affiliation response for group 201 |
| `... --grant 201:100a:1234567 --grant-at 0` | a group voice grant of channel `0x100A` to that unit and group |
| `... --deny 1234567 --deny-at 5:3`, `--queue 1234567:0x2f --queue-at 5:3` | a deny or queued response to the voice request, in units 5 to 7 |
| `p25tool.py decode in.dibits --inbound`, `p25tool.py disc in.raw --meta in.json --inbound` | the radio's inbound bursts: each request and its fields |
| `p25tool.py disc voice.raw --meta voice.json` | the radio's call on the granted channel (a conventional call) |
| `p25tool.py inbound in.dibits --expect in.json --src 1234567 --tg 201` | the three requests a radio sends, for checking the decode side |

`--X-at FIRST[:COUNT[:EVERY]]` names the units (from 0) that carry an answer: COUNT units from FIRST (0: to the
end), each EVERYth of them. A unit holds three blocks at most, so three answers in every unit leave no broadcast and
a receiver never learns the system's identity: a file that answers all through gives each answer its own unit
(`--reg-at 0:0:3 --aff-at 1:0:3 --grant-at 2:0:3`). The expect file gives each unit's symbol position. A file of 8192 symbols or fewer goes in the bench's request and can be repeated (`fsk4_bench.py tx --repeat`).
`p25tool.py selftest` covers the answers and the inbound decode, as symbols and through the C4FM model.

## 9. Not confirmed

These come from no source available here, or from one only in part. The code treats each conservatively; a capture
of a live control channel settles them.

| Fact | What the code does |
|---|---|
| The block CRC: register starting at 0, the remainder inverted | as stated. A wrong guess fails every block, which a capture shows at once |
| A TSDU or packet is padded with null bits to the next status symbol | as stated (180, 288, 360 symbols) |
| MFID `0x01` uses the standard table | taken as the standard's |
| The TDMA identifier's channel types: slots per carrier (types 0-2 one, 3 two, 4 four, others two) | only type 1 counts as FDMA; every other type is refused as TDMA |
| The transmit offset's unit in the VHF/UHF and TDMA forms | taken as channel spacings; the `0x3D` form as 250 kHz. A programmed identifier's offset is used where given |
| The sense of the transmit offset's sign bit (1: above) | as stated |
| The inbound request layouts (2.1): field order and reserved bits of `0x2C`, `0x28`, `0x00` | as stated; a capture of another radio, or the site's answers, settles them |
| The answer layouts (2): registration, affiliation, deny and queued responses | as stated; an answer that doesn't name our unit is ignored, so a wrong guess shows as no answer (`timeouts`) |
| A grant answering our request is `0x00` with our unit as the source | as stated; the explicit form or another source is not taken as ours |
| When a subscriber may send on the inbound channel: after an idle status symbol, how long after it, and the random wait | after a block heard since the tune, not while the last status symbol says busy; waits as in 5 |
| The response timer, resend count and registration backoff a site expects | 1 s, three sends, 10 s to 60 s |
| A one-block inbound TSDU's status symbols: "unknown" | as stated |
| Whether a site needs the affiliation's announcement group, or a registration's address, kept for later requests | not kept |
| The explicit secondary control channel form's layout | only its channel (T) is read |
| The system service broadcast's layout | view only |
| The four flag bits ahead of the system ID in RFSS and adjacent site status | kept as sent, not used |
| The multi-block packet header: format in octet 0, SAP in octet 1, MFID in octet 2, blocks to follow in octet 6, opcode in octet 7 | used to recognize the header and count it only |
