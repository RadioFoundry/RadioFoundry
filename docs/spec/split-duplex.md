<!-- SPDX-License-Identifier: MIT -->

# Split duplex repeater (two radios as one duplex MMDVM modem)

Two OpenDigitalRadio radios, each connected to the same host as an MMDVM modem, act together as one full-duplex DMR
repeater: the **TX half** transmits the repeater output (BS sourced, continuous, both slots, with the CACH), the
**RX half** receives the repeater input (MS sourced bursts on both slots) and labels each burst TS1 or TS2 from the TX
half's slot timing. The host (`tools/split_duplex_host.py`, or any duplex host that implements section 8) coordinates the pair and can present it to
MMDVMHost as one ordinary duplex modem (section 8).

Switch: `CONFIG_HOTSPOT_SPLIT`, on wherever `CONFIG_HOTSPOT` is (not on the MD-380), a child of `CONFIG_HOTSPOT` (`docs/build-config.md`). Built in, the
radio still behaves exactly as a simplex hotspot (and as the SFR of `sfr.md`) until a host that knows this extension
enables a role with the new vendor command (section 3). No existing MMDVM implementation splits TX and RX across two
modems, so this document defines the small extension; everything else uses the standard MMDVM commands with their
standard meanings (`mmdvm-protocol.md`).

Standards cited: ETSI TS 102 361-1 V2.7.1 ("-1"), clause numbers given as "-1 4.5".

## 1. Why two radios, and what each half does

A duplex DMR repeater receives on f_in and transmits on f_out at the same time. A handheld can't: its single synthesizer
and T/R switch do one or the other, and a radio keyed continuously never hears the inbound. Two radios can, if the TX
half's slot clock is known to the RX half: the repeater's inbound slots are defined by its outbound timing (-1 4.4.1,
4.5), so a burst heard on f_in can only be called TS1 or TS2 by comparing its time with the outbound slot clock. The
radios are separate machines with separate crystals, joined only through the host, so the host relays the TX half's
slot clock to the RX half (time sync, section 6).

| Half | On air | Never |
|---|---|---|
| TX half | continuous BS-sourced outbound on f_out while keyed: both slots every 60 ms frame, BS voice / data sync, a CACH before every burst (TC slot numbering, AT inbound busy, short LC), idle bursts in empty slots (-1 4.6.1) | receives anything for the host |
| RX half | receive only on f_in, both slots, MS-sourced bursts, each burst sent up as DMR_DATA1 / DMR_DATA2 as a duplex MMDVM board does | keys its transmitter |
| normal | the simplex hotspot of `mmdvm-protocol.md` (or the SFR of `sfr.md`) | |

## 2. Compatibility rules (the owner's hard requirements)

1. **Standard hosts see no change.** MMDVMHost, Pi-Star, WPSD, DMRGateway, BlueDV and the SDR bench tools
   never send command 0xD6; to them a split build is the simplex hotspot (or SFR) it was. All of `mmdvm-protocol.md`
   section 7 holds unchanged; the host tests run the whole MMDVM suite (`tests/host/mmdvmTest.c`) and the SFR suite a
   second time with this extension built in and registered (`mmdvmTestSplit`, `hotspotSfrTestSplit`).
2. **Visible differences when built in, for every host:** the GET_VERSION description gains the suffix ` SPLIT1`
   (after the SFR suffix, if any), and command 0xD6 is answered (a build without the extension NAKs it with reason 1,
   as any unknown command). No status byte, no other command and no timing changes.
3. **Roles only by explicit request.** A role starts only with 0xD6 SET_ROLE. It ends, back to normal, on SET_ROLE 0,
   the end of the host session (port closed / DTR dropped, the host link's idle timeout, a radio reset: `mmdvm_end`),
   or the **role lease**: no 0xD6 command for the lease time (default 10 s; TIME requests every second keep it). A
   standard host that connects after a split host died therefore finds the normal hotspot within the lease time even
   if the radio never saw the port close.
4. **Unknown things stay unknown.** Unknown commands keep their NAK 1 / NAK 2 exactly as in `mmdvm-protocol.md` 2.1.
   An unknown 0xD6 sub-command gets the 0xD6 reply with result 1. While in a role the standard commands keep their
   standard meanings (SET_FREQ, SET_CONFIG, SET_MODE, GET_STATUS, DMR_DATA1/2, DMR_START, DMR_SHORTLC, DMR_ABORT): a
   split host speaks plain MMDVM duplex to each half, plus the few vendor sub-commands that carry what MMDVM has no
   field for (the role, the clocks, the AT bits).
5. **Only what MMDVM can't carry is new**: the role (no MMDVM field), time sync and the slot epoch (no MMDVM concept),
   the AT bits fed from the other radio (a duplex board sets them from its own receiver), and a status readout of the
   extension's counters. Short LC, keying, abort, per-slot data, per-slot buffer space, RX overflow and the TX flag
   use the standard commands and status fields.

## 3. The vendor command 0xD6 (ODR_SPLIT)

MMDVM leaves 0xD0-0xDF unused; 0xD5 is our SFR debug command (`mmdvm-protocol.md` 6.4), 0xD6 is this one.

Request: `E0 <len> D6 <sub> [<data>]`. Every request gets exactly one reply frame with the same command:
`E0 <len> D6 <sub> <result> [<data>]`, result 0 = done, otherwise a reason with the numbering of `mmdvm-protocol.md`
2.2: 1 unknown sub-command, 2 wrong state (not in the role that takes it, the hotspot hasn't taken the modem yet, the
modem driver can't do it), 4 invalid (too short, value out of range). Numbers are little endian. Every 0xD6 request
refreshes the role lease. A firmware without the extension answers `E0 05 7F D6 01` (NAK 1).

| Sub | Name | To | Request data | Reply data |
|---|---|---|---|---|
| 00 | INFO | either | none | the INFO block (3.1) |
| 01 | SET_ROLE | either | role u8 [lease_s u8] [power_cap u8] [prebuffer u8] [max_key_10s u8] [cooldown_s u8] [stream u8] [options u8] | the INFO block |
| 02 | TIME | either | token, 8 bytes (echoed) | token 8 bytes, t_rx u32, t_tx u32 |
| 03 | GET_EPOCH | TX half | none | counter u32, t_us u32, ts u8, flags u8 |
| 04 | SET_EPOCH | RX half | t_us u32 [period_ns u32] | none |
| 05 | SET_AT | TX half | busy mask u8 [mode u8] | AT mask on air u8 |
| 06 | STATUS | either | none | the STATUS block (3.6) |
| 07 | ALIGN | TX half | t_us u32, period_ns u32, counter u32 [max_slew_us u16] [mode u8] | phase_us s32, flags u8 (3.8) |
| 08 | TX_SCHED | TX half | counter u32, slot u8, control u8, burst 33 bytes | counter u32, outcome u8 (3.9) |
| 89 | RX_META | from the RX half | (an event, never a request) | result 0, the metadata block (3.10) |

Sub-commands 80 and up are events: frames the radio sends without a request, in the reply format. A host never sends
them; a request with one gets result 1.

Examples: `E0 04 D6 00` (INFO), `E0 05 D6 01 01` (TX half, defaults), `E0 05 D6 01 00` (back to normal),
`E0 0C D6 02 01 02 03 04 05 06 07 08` (TIME with token 0102..08), `E0 05 D6 05 03` (both inbound slots busy).

### 3.1 INFO (00) and the INFO block

| Offset | Content |
|---|---|
| 0 | extension version: 1 |
| 1 | roles the modem driver offers: bit 0 TX half (it has the continuous BS operations), bit 1 RX half (it has a slot clock) |
| 2 | role now: 0 normal, 1 TX half, 2 RX half |
| 3 | TX buffer per slot, bursts (16) |
| 4 | CACH on air: 0 none (BS bursts without a CACH: section 5.4), 1 the chip sends our CACH bits, 2 the chip makes its own CACH (TC at least; our AT / short LC unknown), 3 unknown (experiment) |
| 5-8 | the modem clock now, µs (u32, wraps every 71.6 minutes) |
| 9 | flags: 01 RX half has a valid epoch, 02 TX half keyed, 04 TX half in its cool-down, 08 TX half slot clock running |
| 10-11 | capabilities (u16): bit 0 scheduled TX (3.9), bit 1 ALIGN with the counter base and the snap (3.8), bit 2 ALIGN slewing (3.8), bit 3 RX metadata (3.10), bit 4 carrier trim (reserved, not built), bit 5 PPS input (reserved: no radio wires a pulse, `modem-hrc7000.md` 2.9a). A bit is set only where the radio's modem driver can do it |
| 12 | scheduled TX guard, ms (u8): a TX_SCHED burst must arrive at least this long before its slot goes on air |
| 13 | scheduled TX horizon, slots (u8, 32): a TX_SCHED burst may arrive at most this many slots ahead |
| 14-15 | TX half slot-to-air delay, µs (s16): the time from a slot boundary as GET_EPOCH and ALIGN give it to the start of that slot on air, a constant of the modem driver (0 unknown) |

The block grew from 10 to 16 bytes without a new version: hosts read the bytes they know and test the capability bits.

### 3.2 SET_ROLE (01)

| Data offset | Field | Default | Range |
|---|---|---|---|
| 0 | role: 0 normal, 1 TX half, 2 RX half | (required) | 0-2 |
| 1 | lease, seconds; 0 = default | 10 | 1-255 |
| 2 | TX half power cap, MMDVM RF level (SET_FREQ byte 9 scale); 0 = default | 64 (25 %) | 1-255 |
| 3 | TX half prebuffer per slot, bursts (the target repeat delay); 0 = default | 3 (180 ms) | 1-12 |
| 4 | TX half longest continuous key-up, 10 s units; 0 = default | 18 (180 s) | 1-60 (600 s) |
| 5 | TX half cool-down after that, seconds; 0 = default | 20 | 1-255 |
| 6 | TX half stream: 0 the DMR_DATA1 / DMR_DATA2 buffers (section 5.5), 1 scheduled (TX_SCHED, 3.9) | 0 | 0-1 |
| 7 | options: bit 0 RX_META events (RX half, 3.10). Other bits must be 0 | 0 | 0-1 |

Result 2 when the hotspot hasn't taken the modem yet (send GET_VERSION or SET_FREQ first) or the driver lacks the
operations of that role (INFO byte 1); 4 for a role above 2 or an unknown option bit. Setting the same role again only updates the parameters.
Changing role drops whatever the previous role had (buffers, epoch, calls going up get their DMR_LOST). Role 0 is
always accepted.

### 3.3 TIME (02)

The host sends its own 8-byte token (for example its send time t1 in µs); the reply echoes it with **t_rx**, the modem
clock (µs) when the request frame was taken from the port, and **t_tx**, the modem clock when the reply was built (the
reply is written at once after). The modem clock is the one the slot clock is stamped in (`hal_dmr_modem_t.now_us`).
The host measures t4 when the reply arrives and estimates the offset radio minus host as
((t_rx - t1) + (t_tx - t4)) / 2 with round trip (t4 - t1) - (t_tx - t_rx) (section 6).

### 3.4 GET_EPOCH (03), TX half

**counter** and **t_us**: the TX half's latest outbound slot boundary (the start of a 30 ms outbound slot: the CACH
before burst `ts`), its count and its time in the TX half's modem clock; **ts**: the outbound slot that started there
(1 or 2); flags: 01 keyed, 02 the slot clock runs. Result 2 when not TX half or no slot clock runs yet. The TX half runs
its slot clock from SET_ROLE 1 until the role ends, keyed or not, so one epoch holds for the whole role (re-read it
after a retune, which restarts the clock: SET_FREQ). Successive outbound slots are period_ns apart on the TX half's
crystal; the host measures the ratio of the two crystals (section 6).

### 3.5 SET_EPOCH (04), RX half

**t_us**: the start of some outbound TS1 slot, in the RX half's modem clock (the host translates it, section 6);
**period_ns**: the outbound slot length in RX half clock units (ns per slot; 0 or absent = 30 000 000). The RX half
labels from it at once (section 7) and moves the anchor forward itself. Result 2 when not RX half; 4 when period_ns is
outside 29 990 000-30 010 000 (more than 333 ppm off).

### 3.6 SET_AT (05), TX half, and the STATUS block (06)

SET_AT: busy mask bit 0 = inbound TS1 busy, bit 1 = inbound TS2 busy; mode 0 (default): the AT bit on air is the mask
OR the automatic rule (an outbound slot carrying a call or data, not idle, marks its inbound slot busy: the repeat of
an inbound call keeps its own inbound busy, -1 4.5 "may also set the AT bit to Busy during the call hang"); mode 1:
the mask alone. The mask holds until the next SET_AT, and falls back to idle after 2 s without one (a host that died
must not leave the channel marked busy). The reply carries the mask that is on air now.

STATUS block (all counters wrap):

| Offset | Content |
|---|---|
| 0 | role |
| 1 | flags as INFO byte 9 |
| 2 | RX half: inbound busy mask (bit 0 TS1, bit 1 TS2: a call or data heard on that slot within the last 360 ms, or an uplink call open) |
| 3 | TX half: AT mask on air |
| 4, 5 | TX half: bursts buffered for slot 1, slot 2 |
| 6-7, 8-9 | TX half: underruns slot 1, slot 2 (u16): a slot that had started a stream found its buffer empty |
| 10-11 | TX half: idle bursts sent (u16) |
| 12-13 | TX half: CACH bursts sent (u16) |
| 14-15 | TX half: short LCs taken from DMR_SHORTLC (u16) |
| 16-17, 18-19 | RX half: bursts labeled TS1, TS2 (u16) |
| 20-21 | RX half: bursts heard with no valid epoch (u16; sent up as DMR_DATA2) |
| 22-23 | RX half: last phase residual, µs (i16): where the burst's slot started relative to the epoch's prediction |
| 24-25 | RX half: seconds since the last SET_EPOCH (u16, 65535 = never) |
| 26-27 | TX half: seconds keyed in this key-up (u16) |
| 28-29 | TX half: DMR_SHORTLC frames that failed the check (u16) |
| 30-31 | lease expiries (u16, since boot) |
| 32-33 | TX half: scheduled bursts sent (u16) |
| 34-35 | TX half: scheduled bursts late (u16): outcome 1, or committed too late for the driver to place |
| 36-37 | TX half: scheduled bursts refused, too early or full (u16) |
| 38-39 | TX half: scheduled bursts replaced by a later copy (u16) |
| 40-41 | TX half: scheduled bursts refused in the cool-down (u16) |
| 42-45 | TX half: phase now, µs (s32): the slot clock against the last ALIGN's grid (3.8), 0 without one |
| 46 | TX half: ALIGN flags now (3.8): 01 aligned, 04 snapping, 20 GET_EPOCH numbers slots from the last ALIGN (counter base) |
| 47 | reserved, 0 |

The block grew from 32 to 48 bytes; a host that reads 32 is unaffected.

### 3.7 The description suffix

` SPLIT1` is appended to the GET_VERSION description whenever the extension is built in, role or not (the number is
the extension version). Find it with `\bSPLIT(\d+)\b`; send 0xD6 only to a modem that announces it. It never contains
`MMDVM`, `SFR` or a slot pattern, so no host's modem type detection or SFR regular expression changes.

### 3.8 ALIGN (07), TX half

ALIGN puts the TX half's slot clock on the host's grid, for instance UTC slots (slot n starts at n x 30 ms after a
UTC epoch, outbound TS1 on even n). **t_us** is a slot boundary as GET_EPOCH gives it, in the TX half's modem clock (the
host translates it with TIME, as for SET_EPOCH; it subtracts INFO bytes 14-15 to turn an on-air time into a
boundary). **counter** is that slot's number (for UTC slots, n mod 2^32); an even counter is an outbound TS1 slot, an
odd one TS2. **period_ns**: the slot length in TX half clock units, 0 = 30 000 000 (range as SET_EPOCH). The grid is
t_us + (c - counter) x period_ns / 1000 for slot c, either side of t_us. **max_slew_us**: the average slot length
change while slewing, µs per slot (0 = default 20; above 20 is invalid). **mode**: 0 (default) snap while unkeyed,
slew while keyed; 1 always slew, never restart the clock (needs capability bit 2).

The reply's **phase_us** is the slot clock's boundary minus the grid's, folded to ±15 000 µs, measured before anything
is changed. Flags: 01 aligned (|phase| < 1000 µs after this request), 02 slewing, 04 snapped, 08 period applied to the
slot clock (else the period is stored for the phase only), 10 not applied (keyed).

- **Not keyed**: the slot clock snaps: it restarts so that a boundary falls on the grid, and from then on GET_EPOCH's
  counter is the grid's number (counter base) and even counters are outbound TS1. The TX half checks the new clock
  against the grid at its first boundaries and snaps again (up to three times) if it is off by 1000 µs or more or on
  the wrong slot, with the chip's restart delay learned from the first boundary (kept for the role). STATUS 42-46 show
  the result: a clock that stays off keeps the counter base but not the aligned flag. Allowed in either stream mode.
- **Keyed, or mode 1**: if the counter's parity matches the slot clock's and either |phase| < 1000 µs or the driver can
  slew (bit 2), the numbering changes at once (counter base, flag 01 when |phase| < 1000 µs) and, with bit 2, the
  clock slews onto the grid (flag 02 while the phase is over half a driver step). Otherwise nothing changes and the
  reply carries the phase with flag 10, result 0: the host learns how far a long key-up has drifted (20 ppm over
  180 s is 3.6 ms) and re-aligns at the next unkey.
- **Slewing** moves the boundaries without a restart, so the transmitter stays keyed and no slot is lost. The driver
  makes one slot longer or shorter by whole steps (`bs_slew`, `bs_slew_step_us` in `hal_dmr_modem.h`), at least 3
  slots apart, never more than max_slew_us times the slots since the last step, and never past the grid. The default
  20 µs per slot is 667 ppm, 30 times the 20 ppm of two crystals, so a host that re-aligns every few seconds keeps the
  phase within a step; a 3 ms drift takes about 4.5 s. Slewing ends when the phase is within half a step, at a snap
  (an unkeyed mode 0 ALIGN, a retune), or when the clock is lost or on the other slot (ALIGN again). Each step counts
  in `split.slew_steps` / `split.slew_us`.
- A retune (SET_FREQ) restarts the slot clock. The TX half snaps back onto the last ALIGN's grid by itself at the
  next poll while unkeyed (STATUS 46 shows 04 until it is done); the host may send ALIGN again as well. The TX half
  keeps no period of its own: with flag 08 clear the host re-aligns while unkeyed (every 10 s is enough: 20 ppm is
  0.2 ms) and watches STATUS 42-45 while keyed.
- Result 2: not TX half, or the slot clock is not running. Result 4: too short, period out of range, or the modem can't
  move its slot clock (capability bit 1 clear).

### 3.9 TX_SCHED (08), TX half, stream 1

One burst for one numbered outbound slot (`E0 2B D6 08 ...`). **counter**: the slot, in ALIGN's numbering; **slot**: 1
or 2, which must be the counter's parity (TS1 even) or the request is invalid (result 4); **control** and **burst**: as
DMR_DATA1/2 (`mmdvm-protocol.md` 2.8). Counters compare with wrap-around (signed 32-bit difference). Every request gets
its reply: counter echoed and an outcome:

| Outcome | Meaning | Counted (STATUS) |
|---|---|---|
| 0 | queued | 32-33 when sent |
| 1 | late: the slot is already committed to the driver (about INFO 12 before air), dropped, never moved | 34-35 |
| 2 | early: more than INFO 13 slots ahead | 36-37 |
| 3 | replaced the burst already queued for that slot (a resend, or a better copy from a voter) | 38-39 |
| 4 | full (another slot's burst holds the place), or in the key-up cool-down | 36-37, 40-41 |

Result 2 when not TX half, stream 0, or no counter base (no ALIGN yet, or the slot clock restarted and has not snapped
back). An idle burst (or a repeated terminator, as in 4.1) for a slot is taken as "nothing": the place is cleared and
the stream rules fill the slot. A slot with nothing queued sends
what the stream rules give (section 5.1: an idle burst, a silence voice burst inside a voice stream, a terminator for a
stream the host left), and the CACH runs as usual: AT, TC and the short LC are the same as in stream 0.

**Keying** is the same as stream 0: DMR_START 01 keys and 00 unkeys, the 2 s host-silence rule and the duty guard
apply. A queued burst keys the transmitter too, early enough for its slot (key-up starts with a TS1 slot, so up to two
slots of idle before it); a key-up that came from a queued burst (not DMR_START 01) ends 1 s after the last scheduled
burst went out. Bursts are committed to the driver one slot ahead, each tagged with its counter: the driver plays a
burst only in that slot and drops it if the slot has passed, so a late commit is lost, never shifted.

### 3.10 RX_META (89), RX half event

With SET_ROLE option bit 0 (capability bit 3), the RX half sends `E0 15 D6 89 00 <16 bytes>` for every burst it hears,
just before that burst's DMR_DATA / DMR_LOST frames, so a host (a voter, a logger) gets what the standard frames
leave out. The block:

| Offset | Content |
|---|---|
| 0-3 | the start of the burst's slot, modem clock µs (valid with flag 40) |
| 4-5 | residual against the epoch, µs (s16, clamped; as STATUS 22-23) |
| 6 | label: 1 TS1, 2 TS2, 0 none (no epoch or the slot clock didn't place it) |
| 7 | kind: the slot type (`hal_dmr_modem.h` burst kinds), voice 0x10 + burst index (A = 0) |
| 8 | color code heard |
| 9 | flags: 01 CRC good, 02 in step, 04 PI, 08 LC complete, 10 BS sync, 20 labeled by the epoch, 40 slot time valid |
| 10-11 | RSSI, dBm (s16, 0 unknown) |
| 12-13 | sequence number (u16, counts every event; a gap means a lost frame) |
| 14-15 | reserved, 0 |

A burst that fails its CRC still gets its event (flag 01 clear), though no uplink frame may follow. The events cost
21 bytes per 30 ms slot heard, well inside the port's budget.

## 4. Standard commands in a role

### 4.1 TX half

| Command | Meaning in the TX half (a duplex MMDVM board's) |
|---|---|
| SET_FREQ | the repeater pair: RX = f_in (not used by this half), TX = f_out; RF level capped by the power cap. A retune restarts the slot clock (new epoch) and drops the buffers |
| SET_CONFIG | duplex flag clear (simplex flag set is accepted too); color code for the outbound; the rest as `mmdvm-protocol.md` 2.5. The SFR trigger never fires in a role |
| SET_MODE | 02 DMR: normal; 00 idle: buffers dropped, keying ends after the burst on air |
| DMR_START `1D 01` / `1D 00` | key the continuous outbound / unkey once what is buffered has gone out (at most 1 s). ACKed |
| DMR_DATA1 / DMR_DATA2 | one 33-byte burst for outbound slot 1 / 2, as MMDVMHost builds it (BS sync, control byte 00 allowed: classified from the burst as in 2.8). Queued per slot. The first frame while unkeyed keys up too. Silent on success (as MMDVM boards); NAK 5 when that slot's buffer is full, NAK 2 in the cool-down, NAK 4 wrong length |
| DMR_SHORTLC `1C <9 bytes>` | the CACH short LC, sent from the next 4-CACH boundary and repeated every 120 ms until replaced (section 5.3). ACKed |
| DMR_ABORT `1E <slot>` | drop what is buffered for that slot (not yet on air). ACKed |
| GET_STATUS | byte 5 flag 01 TX while keyed (and while buffered bursts wait); bytes 7 / 8 the free space of slot 1 / slot 2 less a margin of 2 (the hosts write while above 1); flag 08 TX overflow once after a NAK 5; nothing else changes |

### 4.2 RX half

| Command | Meaning |
|---|---|
| SET_FREQ | the repeater pair: RX = f_in, TX = f_out (not used: this half never transmits) |
| SET_CONFIG | color code of the inbound |
| DMR_DATA1 / DMR_DATA2 | NAK 2 (receive only) |
| DMR_START, DMR_SHORTLC, DMR_ABORT | ACKed, no effect |
| GET_STATUS | flag 40 carrier detect while bursts are heard; flag 04 RX overflow when uplink frames were lost; space bytes 0 |
| Uplink | every burst heard as DMR_DATA1 (labeled TS1) or DMR_DATA2 (TS2), framed as the simplex uplink frames them (`mmdvm-protocol.md` 2.8: headers, voice A-F with EMB and the call's embedded LC, terminators, data; late entry with a header made from the embedded LC; silence fill), RSSI bytes; DMR_LOST1 / DMR_LOST2 500 ms after the last burst of a call without a terminator, per slot |

## 5. TX half: the continuous BS outbound

### 5.1 Framing (-1 4.5, 4.6.1, 5.1.4.5)

While keyed the TX half sends a burst in **every** slot, alternately outbound TS1 and TS2, each preceded by a 24-bit
CACH, with BS-sourced sync (voice 755FD7DF75F7, data DFF57D75DF5D, -1 9.1.1): a "traffic channel with CACH",
transmitted without gaps. A slot with nothing buffered gets the idle message (-1 7.3); a slot whose stream underruns
mid voice superframe gets a silence voice burst at the next position (so the superframe stays whole), mid data an idle
burst. The chip makes the FEC, slot type and EMB (with its color code from SET_CONFIG); the host's sync and EMB bits are
not sent as given (the HR_C6000 / HR_C7000 build bursts from information bits: `modem-hrc6000.md` 6.1). So voice keeps
its AMBE and position, headers / terminators / CSBK / data keep their 96 (or 144 / 196) information bits including the
host's CRC (sent verbatim, CRC_MCU_Control), and the embedded LC of B-E is taken per slot from the host's own B-E
fragments once they decode (talker alias and GPS pass through), else from the slot's voice header.

### 5.2 The CACH (-1 6.3, 9.1.4, B.3.5, B.4.1)

Each CACH is 7 TACT bits (AT, TC, LCSS(2), then Hamming(7,4,3) parity H(2..0), generator x^3 + x + 1, -1 table B.17)
and 17 payload bits, interleaved over the 24 bits as -1 figure B.9:

```
TX(23..0) = AT  P16 P15 P14  TC  P13 P12 P11  LS1 P10 P9 P8  LS0 P7  H2 P6 P5 P4  H1 P3 P2 P1  H0 P0
```

- **TC**: the outbound slot that follows the CACH (0 = TS1, 1 = TS2), -1 4.5 figure 4.9.
- **AT**: inbound busy for the inbound slot with the same number, which starts one slot later (-1 4.5 figure 4.8:
  "the CACH burst preceding outbound time slot 2 indicates the status of the burst in inbound time slot 2"). The TX half
  can't hear the inbound; the host feeds it (SET_AT) from the RX half, plus the automatic rule of 3.6. With neither,
  AT is 0 (idle). (-1 table 9.5's note, AT = 1 "in continuous transmission mode", is about MS-to-MS continuous mode,
  not a repeater outbound.)
- **LCSS / payload**: the short LC (-1 7.1.4, B.2.3, figure B.6): 28 bits (SLCO + 24 data bits) plus CRC-8 (x^8 +
  x^2 + x + 1, init 0, -1 B.3.7) = 36 bits in a 3 x 12 matrix, each row Hamming(17,12,3) (generator x^5 + x^2 + 1, -1
  table B.13), a fourth row of column parity (even), read out column by column into four 17-bit rows, one per CACH,
  LCSS first fragment (01), continuation (11), continuation (11), last (10). One short LC per 4 CACH = 120 ms.
  Without a short LC from the host the TX half sends the Null message (SLCO 0000, -1 4.6.1, TS 102 361-2 7.1.3).

The encoders are `core/dmr/fec/dmrCach.c` (no hardware), shared with the Tier III plan (`dmr-tier3-plan.md`, a TSCC
outbound needs the same CACH with C_SYS_Parms).

### 5.3 DMR_SHORTLC

`E0 0C 1C <9 bytes>`: the 68 bits of the four CACH payloads (the transmit matrix of -1 figure B.6, row 1 first, P16
first, most significant bit of byte 0 first, 4 zero pad bits at the end of byte 8). The TX half checks it (column
parity, Hamming rows, CRC-8); a frame that fails is counted (STATUS 28) and re-read as the 28-bit short LC in its first
4 bytes (bits 27-0 right aligned, as -1 B.3.7 numbers them) if that form's CRC matches the 5th byte, else dropped (the
previous short LC stays). It goes on air from the next CACH whose slot counter is a multiple of 4. The exact byte layout
MMDVMHost sends is to be confirmed on the bench (section 11, open question 1): both forms are taken.

### 5.4 Making BS framing on the chips

The core hands the modem driver, for each slot, the burst and the CACH (fields and the 24 interleaved bits:
`dmr_cach_t`). What the driver can do with the CACH depends on the chip:

- **HR_C6000**: the chip's layer 2 BS framing sends an all-zero CACH whatever is written to its inputs, so the driver
  runs the chip in the physical layer mode, where it only modulates ([modem-hrc6000.md](modem-hrc6000.md) 9.1, 9.2).
  The core builds each slot's 288 bits (`bsOutboundRawSlot`: the CACH, then the burst with BS sync, slot type and
  coded payload, or for voice the AMBE around the sync or EMB and embedded LC) and the driver writes them to TX RAM.
  INFO byte 4 says 1 (ours).
- **HR_C7000**: the chip's own BS mode (LAYER2_BS_MODE, 0x428-0x438) sends Idle frames with BS sync and a CACH of
  valid parity, but their AT, TC and short LC are not under our control (`modem-hrc7000.md` 2.9, bench 2026-10-20). So
  the driver (`hrc7000_bs.c`) uses the chip's physical layer mode, as on the HR_C6000: the core's 288 bits per slot go
  to TX RAM and the chip only modulates them (`modem-hrc7000.md` 2.10). INFO byte 4 says 1 (ours). The glue
  (`platform/hrc7000/dmr_glue.c`) wires `now_us` and `slot_clock` (the slot engine's tick and its time; a received
  burst's arg is the tick of the slot after its own, as on the HR_C6000), so the HR_C7000 radios offer both halves.
- **Fallback when the chip can't make a CACH**: BS-synced continuous bursts without valid TACT bits. MMDVMHost-style
  receivers and most radios still decode the bursts, but a subscriber radio in repeater mode takes its slot numbering
  from TC and its channel access from AT, so it may not tell TS1 from TS2 and talks on the wrong slot. That is not a
  usable repeater output; use a radio whose chip makes the CACH (HR_C7000 once specified) or an MMDVM board as the TX
  half (the RX half still works with any of them, section 8.4).

### 5.5 Per-slot buffer and target delay

Each slot has a buffer of 16 bursts (960 ms). A stream starts playing out on its slot once `prebuffer` bursts are in
(default 3 = 180 ms, the target repeat delay), or at once for a stream that ends (terminator) or after 240 ms; until
then the slot gets idle bursts. Host and USB jitter up to the prebuffer never reach the air. The buffer is fed into the
driver two bursts ahead per slot (the driver's set-up happens one slot before the burst, `modem-hrc6000.md` 4.1).
GET_STATUS bytes 7 / 8 report each slot's free places less 2. An underrun (STATUS 6-9) re-arms the prebuffer for that
slot. Network hosts pace at one burst per 60 ms per slot (`mmdvm-protocol.md` 3.5), which the clock difference between
host and radio (a few ppm) turns into a slow drift of the buffer level: 20 ppm is one burst per 50 minutes, absorbed by
the next gap between calls (a stream that ends drains, a new one prebuffers again).

### 5.6 Keying and the power / duty guard

- Keyed by DMR_START 01 or the first DMR_DATA; unkeyed by DMR_START 00 (after the buffers drain, at most 1 s), SET_MODE
  idle, DMR_ABORT of both slots while idle, the end of the role, or the guard.
- **Power**: the RF level from SET_FREQ is capped by the role's power cap (default 64, about 25 %: the lowest step on
  most boards). A handheld's PA and battery aren't made for continuous transmission; use mains power and a low step.
- **Duty**: one key-up lasts at most max_key (default 180 s), then the TX half unkeys for the cool-down (default 20 s),
  during which DMR_DATA is NAKed with 2, DMR_START 01 is ignored and the INFO / STATUS flag 04 is set. A host that keeps
  a repeater up for long hangs should drop the key between calls (MMDVMHost duplex does after its mode hang).
- The TX half also unkeys when the host goes silent for 2 s while keyed (no frame of any kind).

## 6. Time sync

The host sees three clocks: its own, the TX half's modem clock (C_tx) and the RX half's (C_rx), each a free-running
µs counter. With offsets o_tx = C_tx - C_host and o_rx = C_rx - C_host (both drifting at up to about 2 x 20 ppm
relative to each other and to the host):

1. **Offsets**: every 1 s the host sends TIME to each radio (on Linux USB CDC a round trip is 1-4 ms). It keeps the last
   16 samples per radio and uses the one with the smallest round trip (NTP's clock filter: the smallest round trip has
   the smallest asymmetry), offset = ((t_rx - t1) + (t_tx - t4)) / 2. A linear fit over the last 30 s of filtered
   samples gives the rate (drift) of each radio relative to the host.
2. **Epoch**: the host reads GET_EPOCH from the TX half (after SET_ROLE 1, after every retune, and every 10 s), picks
   any TS1 boundary T_tx = t_us (+ 30 000 µs if ts was 2), and translates it into the RX clock:
   T_rx = T_tx - o_tx + o_rx, with both offsets evaluated at the same host time (extrapolated with the fitted rates).
   period_ns for the RX half = 30 000 000 x (1 + r_rx - r_tx) where r are the fitted rates (ppm / 1e6).
3. **SET_EPOCH** to the RX half, again every 10 s (2 s while the rates are still being learned, the first 30 s), only
   when the epoch can be trusted. A sample's offset is wrong by at most half its round trip, so each clock fit has an
   error bound (half the largest round trip it rests on), and the epoch's is the sum of the two. The host relays the
   first epoch only when that bound is under 10 ms, and a later one only when the bound is under 10 ms and the epoch
   lands within 7.5 ms of the last one relayed, carried forward by whole frames. One that disagrees is held back; three
   in a row that agree with each other replace the one relayed (it was the wrong one). An epoch off by more than half a
   slot labels every inbound burst with the other slot while the residual (step 4) still looks small, because the RX
   half tracks to the nearest boundary; these rules keep such an epoch from being relayed. `split_duplex_host.py`'s
   `EpochRelay` keeps a log of every decision.
4. **RX half tracking**: in a call each in-step burst gives the phase residual (where its slot started minus the
   prediction, folded to ±15 ms); the RX half averages it (1/8 per burst) and moves its anchor by the average, by at
   most ±5 ms in all since the last SET_EPOCH. That absorbs constant chip delays (the TX chip's slot boundary to air
   and the RX chip's air to report) and drift between resyncs. The label itself (which of the two slots) only ever
   comes from the epoch: MS-sourced bursts carry no slot number (-1 9.1.1: the MS sync is the same on both slots).

**Accuracy needed**: a burst is labeled from which half of the 60 ms frame its slot falls in, so the label is right
while the total error is under half a slot, 15 ms. Budget: the offset estimate with the round-trip filter, 1-2 ms on
USB full speed (each direction has 1 ms USB frame quantization plus task latency of the host link loop, under 5 ms on
the radio); the HR_C6000 report time relative to its slot (1-12 ms after the timeslot interrupt for an in-step burst,
`modem-hrc6000.md` 4.2: we use the timeslot interrupt time, not the report time, so this does not count); constant
chip delays, estimated under 3 ms and tracked away; drift between resyncs, 40 ppm x 10 s = 0.4 ms. The target is
**|error| ≤ 5 ms**, a 10 ms margin. The STATUS phase residual (22-23) shows the real error on the bench (section 10).

The host must reach both radios directly (USB, or a serial port on the host they're plugged into). A serial link
carried over a network adds a one-way delay that stays put for a session: the round trip still looks plausible, but
both offsets come out skewed the same way. On the bench, through such a link, 9 of 12 subscriber calls were labeled
with the wrong slot; with the same radios on the host's own ports, 12 of 12 were right.

## 7. RX half: labeling

The RX half's modem receives on both slots (`rx_lock_slot(0)`, color code from SET_CONFIG) and reports each burst
with the slot counter of its report (`DMR_EV_BURST_RX` arg); `hal_dmr_modem_t.slot_clock` gives the latest slot
boundary's counter and µs time, so the boundary at which burst k's slot started is t(k-1) = t_last - (c_last - k + 1)
x 30 ms (the report comes in the slot after the one the burst was in, `hrc6000_dmr.h`). With the epoch E (start of an
outbound TS1) and period P: n = round((t(k-1) - E) / P). Outbound slot n is TS1 when n is even; the inbound slot that
runs at the same time has the other number (inbound TSx starts one slot after outbound TSx, -1 4.5 figure 4.8 / 4.9;
-1 table 5.1: aligned logical channel 1 = inbound TS2 + outbound TS1). So **label = TS2 for even n, TS1 for odd n.**

Each slot has its own uplink tracker (`hotspotUplink_t`: headers, late entry, silence fill, one terminator, DMR_LOST
after 500 ms) and its own embedded LC fragments, so two calls on the two slots go up interleaved and whole. Without a
valid epoch (none yet, or older than 120 s) bursts go up as DMR_DATA2 and are counted (STATUS 20-21).

Inbound busy (STATUS 2, for the host's SET_AT): a slot is busy from its first burst until its call ends (terminator
or DMR_LOST), and 360 ms after any data burst.

## 8. The virtual duplex modem: host-side guide

This section is the contract for a host: with it the pair can be presented to MMDVMHost (or another duplex host) as
one standard duplex MMDVM modem without further radio changes. "Upstream" is the duplex host (MMDVMHost), "TX" and "RX"
the two radios.

### 8.1 Start-up

1. Open both ports. On each: GET_VERSION; require ` SPLIT1` in the description. SET_FREQ with the repeater pair (RX =
   f_in, TX = f_out, RF level from upstream) and SET_CONFIG (duplex flag clear, upstream's color code) to **both**.
2. INFO on each; require bit 0 of byte 1 on TX, bit 1 on RX. SET_ROLE 1 on TX, SET_ROLE 2 on RX (lease 10 s; the power
   cap and prebuffer you want).
3. Time sync (section 6): TIME to each, at least 8 samples 20 ms apart, then every 1 s. GET_EPOCH from TX, SET_EPOCH to
   RX. Only then report the virtual modem ready upstream.
4. Keep both leases: TIME every 1 s does it. If either radio fails (no TIME reply for 3 s, a role lost in INFO), report
   the virtual modem dead upstream (stop answering status, so MMDVMHost resets it) and start again from 1.

### 8.2 Upstream commands to the halves

| Upstream sends | Goes to | Notes |
|---|---|---|
| GET_VERSION | answered by the host itself | protocol 1 (or 2 with capabilities DMR), a description of your choice; never forward |
| GET_STATUS | answered by the host from both halves' last status (8.3) | poll each radio every 60-250 ms yourself |
| SET_FREQ | both, as is | |
| SET_CONFIG | both, as is | |
| SET_MODE | both | idle on TX ends the keying; on RX it stops nothing (the uplink keeps running, as a duplex board's idle receiver) |
| DMR_DATA1 / DMR_DATA2 | TX | pace on TX's space bytes (they are the virtual modem's) |
| DMR_START | TX | |
| DMR_SHORTLC | TX | |
| DMR_ABORT | TX | |
| SEND_CWID | TX only when TX is not keyed (it is a normal FM ID: SET_ROLE 0 first, then back to 1, or skip CW ID) | the halves only key CW in role normal |
| DMR_ALOHA, other modes | NAK 2 from the host | |

### 8.3 The virtual status

| Status byte | From |
|---|---|
| 3 modes | 02 |
| 4 state | upstream's last SET_MODE |
| 5 flags | 01 TX from TX's; 04 RX overflow from RX's; 08 TX overflow from TX's; 40 carrier detect from RX's; never 10 |
| 7, 8 space | TX's bytes 7 and 8 |

### 8.4 Uplink and AT

- Forward every DMR_DATA1 / DMR_DATA2 / DMR_LOST1 / DMR_LOST2 from RX upstream unchanged: they are already a duplex
  board's uplink frames (slot labeled, control byte, RSSI).
- AT: whenever RX's inbound busy mask changes (from STATUS byte 2, or from the uplink frames themselves: a call open on
  a slot is busy until its 42 terminator or DMR_LOST), send SET_AT with the mask to TX. Re-send at least every 1 s
  (TX drops a mask older than 2 s).
- A duplex host repeats RF calls itself: MMDVMHost puts an RF call heard on slot N back out on slot N as DMR_DATA
  frames with DMR_START; nothing in the radios repeats on its own.

### 8.5 Timing the host must keep

- Status: the virtual modem must answer upstream's GET_STATUS within tens of ms; answer from cached radio status.
- Epoch: re-read GET_EPOCH after every SET_FREQ (the TX half's slot clock restarts) and send SET_EPOCH before
  forwarding anything else from RX (bursts heard in between may carry the wrong label).
- Clock rates: refit continuously; resend SET_EPOCH every 10 s.

### 8.6 Using an MMDVM board as the TX half

If only the RX half is an OpenDigitalRadio (for example the TX chip can't make a CACH, 5.4): the epoch must then come
from the MMDVM board's slot timing, which MMDVM doesn't expose. Not supported by this extension; noted for completeness.

## 9. Frequency accuracy, calibration and antennas

- **TX half frequency**: the continuous outbound keys once with the board's per-burst RX-to-TX sequence and then
  stays in TX, so it inherits that sequence's carrier error; no offset constant is added (the sequence and its
  measured error are on each radio's hardware page; gotcha 3.5). A continuous carrier warms the TCXO: measure on the
  SDR bench after 5 minutes keyed (`sdrmon.py analyze` prints the frequency offset per burst) and, if it is outside
  about ±300 Hz, correct the reference oscillator offset in the radio's calibration. An ADF7021-based receiver
  (MMDVM_HS) decodes nothing at -3 kHz, so check every TX half once with the bench.
- **RX half**: uses the radio's receive calibration as is; the modem tolerates the ±1.5 kHz of a typical
  subscriber.
- **Antennas**: the two halves are a duplex repeater without a duplexer. With 5 MHz UHF spacing, a 1 W TX half next to
  the RX half desensitizes it unless the antennas are isolated by about 60-70 dB: a duplexer on one antenna, or
  separate antennas several meters apart vertically (about 40-50 dB at UHF for 3 m), plus the lowest power step. Check
  with the RX half's RSSI of a weak station with the TX half keyed and unkeyed.

## 10. Bench tests (the lead's HIL)

### 10.1 BS framing variants

Debug link command `SPLIT` (0x15) op 1 sets the framing variant the next TX half key-up uses; op 2 reads the split
view. Variants: 0 ours (the default: physical layer mode, the core's 288 bits per slot), 1 the chip's layer 2
framing (0x10 = 0x6E; AT / TC written to 0x52 and the short LC to TX RAM 0x24 for every burst), 255 ours with up to
4 raw (page 4 register, value) pairs written at key-up (0x10 restored at unkey; 0x11 and 0x40 too when pairs were
given). `tools/split_duplex_host.py --exp <variant>` or the HIL test (`ODR_SPLIT_VARIANT`, `ODR_SPLIT_REGS="REG=VAL
..."`) keys the outbound with idle bursts and a short LC for 10 s; `sdrmon.py analyze --cach` on the SDR bench
decodes per burst the family, the CACH (`tact ok AT TC LCSS`) and the short LC.

The results that led to the physical layer mode are in [modem-hrc6000.md](modem-hrc6000.md) 9.2. On the HR_C7000
the variant is a bit field of physical layer options instead (`modem-hrc7000.md` 2.10: continuous mode, the autotest
bit, the TX RAM halves); 0 is the default there too.

### 10.2 TX half keyed with idle CACH, RX half labeling, time sync readout

`tests/hil/test_split_duplex.py` (one radio flashed with `CONFIG_HOTSPOT_SPLIT=y`; the TX and RX tests opt-in with
`--allow-rf --allow-service-stop`):

- `test_split_capability` (no RF): the capability gate of the `split_duplex` checklist feature
  (`tests/hil/features.toml`). The build has the extension and INFO offers both halves (offered bits 0 and 1). On a
  radio that offers fewer it skips `no_hw`, which a bring-up run counts as a failure.
- `test_split_protocol` (no RF): the suffix, INFO, SET_ROLE 2 and 0, STATUS, unknown sub-command (result 1) and
  unknown command (NAK 1), the debug link's SPLIT view.
- `test_split_tx_half_off_air` (marker `radio_tx`): the TX half keyed 10 s at the lowest power with idle bursts, a
  short LC and SET_AT toggling; asserted off air: BS sync only, a burst every 30 ms (same-slot spacing 60 ms), one
  key-up edge, the color code, and with our framing (variant 0) the CACH: TC alternating, AT following SET_AT, the
  short LC decoding; asserted on the radio: no underruns, short LC taken, a CACH for every burst (`bs.cach`).
  `ODR_SPLIT_VARIANT` picks the variant, `ODR_SPLIT_REGS` the raw writes of variant 255.
- `test_split_rx_half_labels` (marker `rf`): the RX half with an epoch made from its own clock; a first bench call
  measures the phase residual and the epoch is centered on the bench's slots; a second call must go up under one
  label, whole (header, voice, terminator), with a residual under 5 ms; a third call with the epoch moved by 30 ms
  must go up under the other label.

`tests/hil/test_split_pair.py` runs a real pair (two radios built with the switch, `--split-rx-port`; the host runs
`split_duplex_host.Pair`, which also repeats every burst the RX half hears on its own slot of the TX half, as a duplex
host would):

- `test_split_pair_repeat` (markers `rf`, `radio_tx`): a bench call on the inbound goes up under its label and comes
  back on the outbound, whole, in under a second.
- `test_split_pair_handheld` (adds `codeplug_write`; `--split-ms-port`, `--split-ms-ts`, `--split-out-mhz`): a third
  ODR radio is put on a repeater channel (its codeplug edited and restored) and keys up on each inbound slot the way a
  subscriber does, synced to the outbound's CACH. Asserted: the call goes up under its own slot only, with no burst
  unlabeled (an unlabeled burst is sent up as TS2, so a TS2 call could pass without an epoch), comes back on the
  outbound on the right slot, the header's source is the subscriber, no underruns, residual under 5 ms.

Run the pair from the host the radios are plugged into (section 6). On the bench (2026-10-21, host on the radios' own
ports): the TX half's epoch matched the outbound's slots on air in 12 of 12 sessions, with the RX half on the outbound;
subscriber calls were labeled right in 29 of 31 sessions with real epochs (bound 5-9 ms). The two wrong sessions are
not explained yet (open question 7).

`tools/split_duplex_host.py --rx PORT --rx-only --flip-at 20` does the RX half part by hand, and `--tx A --rx B`
runs a real pair (bring-up and timing readout, no upstream; `--repeat` repeats as above).

### 10.3 ALIGN and the scheduled stream

- `test_split_align` (no RF): the TX half, never keyed, snapped three times onto a grid of the host's clock (slot n at
  host n x 30 ms). Each time: STATUS aligned with the counter base, GET_EPOCH's boundary on the grid within the clock
  sync's bound plus 1 ms, TS1 on even counters. 2026-10-21, run on the host the radios are plugged into, each from a
  fresh boot: DM-1801 Pro 3 of 3 runs, boundaries 9-120 us from the grid (sync bound 3 ms), learned restart delay
  -1167 us (`modem-hrc7000.md` 2.10 finding 12); DM-1701 4 of 4, 2-317 us (sync bound 0.5 ms), delay +1.2 ms
  (`modem-hrc6000.md` 9.1). The first snap of a boot lands off by the chip's delay and the second, with it learned,
  on the grid. DM-32UV 3 of 3, -2 to +227 us, delay -1141 us; DB25-D 3 of 3, -77 to +84 us, delay -1137 us (both
  over a USB-serial cable, sync bound 5.4-5.6 ms).
- `test_split_tx_sched` (radio_tx): the voice test's call as TX_SCHED for numbered TS2 slots, one resend, one late
  burst, the key-up from the queued bursts; checked off air (one slot, TC 1, the call whole, one key-up, 60 ms
  spacing) and in STATUS. DM-1801 Pro, 2026-10-21: passed (22 queued, 1 replaced, 1 late; the call decoded without
  sync errors in its TS2 slots); DM-32UV passed. DB25-D: the radio restarted during the call (no crash record); open.
- `test_split_slew` (no RF): the TX half snapped onto the host's grid, then ALIGN mode 1 to a grid 1.5 ms later; the
  clock must reach it by slot trims within 5 s (aligned, not slewing, |phase| within 100 µs, `split.slew_us` about the
  shift). Skips without capability bit 2, which no driver sets yet: the HR_C7000's LAYER2_MAX_CNT trim was tried with
  this test and does not move the physical layer's slot timer (`modem-hrc7000.md` 2.10 finding 13), and the HR_C6000
  has no trim (`modem-hrc6000.md` 9.1).
- `test_split_rx_half_labels` (10.2) turns RX_META on when INFO offers it and checks an event for every burst sent up,
  the sequence numbers and both labels.

### 10.4 What runs without hardware

`make -C tests/host`: `splitDuplexTest` (CACH and short LC against tables B.13 and B.17 and figures B.6 and B.9, the
idle message of table D.2, the outbound buffer, the epoch arithmetic, both roles and the guards through mmdvm.c and
hotspot.c on a simulated modem, ALIGN's snap with a chip delay learned, one beyond half a slot and one that can't
be, the counter base after a retune, keyed ALIGN, slewing (keyed and in mode 1: its rate, the steps, the end within half
a step, the grid kept, no restart; a snap, a retune or a parity mismatch during it), RX_META (its fields, the order
before the uplink frames, a bad CRC, the option off), TX_SCHED's outcomes, slots and keying, the debug link command), `mmdvmTestSplit` and `hotspotSfrTestSplit` (the standard
host suites with the extension built in), `hrc6000BsTest` and `hrc6000DmrTestSplit` (the HR_C6000 BS outbound on a
fake chip, and the slot engine's suite with the hand-over hooks built in), `hrc7000BsTest` and `hrc7000DmrTestSplit`
(the same for the HR_C7000, both with the numbered queue and the restart of 3.8 and 3.9), and `sim/splitPairTest.py`: two radios'
cores in lockstep virtual time, with crystals 18-35 ppm apart, one modem clock wrapping through 2^32, USB jitter
with spikes, and the host logic of `split_duplex_host.py`: two overlapping mobile calls are heard by the RX half,
labeled, repeated into the TX half and checked on its outbound (whole, in order, own slot, TC on every CACH, AT busy,
no underrun, repeat delay), then host silence and lease expiry. `tools/bench/dmr_cach.py selftest` checks the Python
CACH coding, which is byte-identical with `core/dmr/fec/dmrCach.c`.

## 11. Open questions

1. The 9-byte layout of DMR_SHORTLC as MMDVMHost sends it (5.3): both forms are accepted; the bench decode settles it.
2. HR_C6000 CACH (5.4, 10.1): AT / TC (0x52) and the short LC (TX RAM 0x24) are now written per the manual; open
   until the bench shows them on air: the 0x11 value, and where the chip takes the CACH LCSS from.
3. HR_C7000 TX half: settled on the DM-32UV (`modem-hrc7000.md` 2.10, bench 2026-10-21): the physical layer's
   timeslot mode with the slot in both TX RAM halves gives the whole outbound with our CACH.
4. The constant delay between the TX half's slot boundary and the RX half's (chip pipelines, air): measured on the
   bench with the phase residual; if it is more than 5 ms, add a fixed correction (the tracker limit). Measured
   (2026-10-21, `modem-hrc7000.md` 2.10): outbound TS1 goes on air 4.6 ms after the HR_C7000 TX half's epoch and
   2.2 ms after the HR_C6000's; subscriber call residuals were 0.1-2.4 ms. No correction needed.
5. Whether AT should default to busy during the host's hang time (mode 0 does: the outbound slot is not idle) or only
   from the RX half (mode 1).
6. The HR_C6000 driver's own counters (`hrc6000_bs_stats_t`) are on the board's diagnostics line `bs` (slots,
   bursts, underruns, key-ups, CACH written, PA failures, stalls; `odr_debug.py` parses them as `bs.*`). Not reachable
   from the core itself (no `hal_dmr_modem_t` stats operation).
7. Two subscriber calls in 31 sessions went up under the other slot with a good epoch and a small residual. The
   SDR bench can tell whether the subscriber sent it on that slot (an ODR radio syncing to the wrong CACH phase) or
   the RX half filed it one slot off: capture the inbound and the outbound together during such a session.
8. The RX half could find the slot numbers itself: after the outbound keys up and before a subscriber transmits, a
   short retune to the outbound reads its CACH (TC), with no host clock in the chain. Not built.
