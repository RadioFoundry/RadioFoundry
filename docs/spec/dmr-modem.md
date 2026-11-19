<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DMR modem behavior and the `dmr_modem` HAL contract

This page describes what the DMR Tier II stack does on top of a baseband modem (the HR_C6000, HR_C7000, or HR_C5000),
and the interface a modem driver offers the stack (`hal/hal_dmr_modem.h`). Protocol facts come from ETSI TS 102 361-1
(air interface), -2 (voice and generic services), and -3 (packet data); clause numbers are given where they help.
Chip facts are on the chip pages: [HR_C6000 modem](modem-hrc6000.md) and [HR_C7000 modem](modem-hrc7000.md). For
which radios carry which modem, see the [feature matrix](../feature-matrix.md).

This page uses these terms:

| Term | Meaning |
|---|---|
| Slot | One 30 ms TDMA timeslot |
| Frame | Two slots (60 ms) |
| Superframe | Six voice bursts A-F on one slot (360 ms) |
| Inbound | MS to BS (the repeater input) |
| Outbound | BS to MS |
| RMO | Repeater mode: through a repeater |
| DMO | Direct mode: no repeater |

## 1. Air interface facts the stack relies on

### 1.1 Physical layer (TS 102 361-1 clause 10)

- 4FSK, 4800 symbols/s, 9600 bit/s gross, 12.5 kHz channel. Deviation is +-648 Hz and +-1944 Hz; dibits 01, 00, 10,
  and 11 map to +3, +1, -1, and -3.
- One slot carries one burst: 264 bits (27.5 ms), plus 2.5 ms that is either the CACH (BS outbound, 24 bits) or guard
  time for the PA ramp (MS inbound, and direct mode).
- The modem does symbol timing, sync search, burst framing, and FEC. The stack never handles symbols.

### 1.2 Burst layout (clause 4.2, 6)

| Burst | First half | Middle 48 bits | Second half |
|---|---|---|---|
| Voice with sync (burst A) | 108 bits AMBE | voice sync | 108 bits AMBE |
| Voice with embedded signaling (B-F) | 108 bits AMBE | 8 EMB + 32 embedded signaling + 8 EMB | 108 bits AMBE |
| Data / control | 98 bits info | 10 slot type + 48 data sync + 10 slot type | 98 bits info |

- Voice payload: 216 bits, three 72-bit AMBE+2 frames (20 ms each, with their FEC). The modem drivers exchange it as
  27 bytes.
- Slot type: color code (4 bits), data type (4 bits), and Golay(20,8) parity.
- EMB: color code (4 bits), PI (1), LCSS (2), and QR(16,7) parity.
- Data info: 196 bits. BPTC(196,96) carries 96-bit payloads (rate 1/2, headers, CSBK, LC), trellis rate 3/4 carries
  144-bit payloads, and rate 1 carries 192 bits with no FEC. Rate 1 fails on any single bit error.

### 1.3 Sync patterns (clause 9.1.1)

| Source | Voice | Data |
|---|---|---|
| BS | 755FD7DF75F7 | DFF57D75DF5D |
| MS | 7F7D5DD57DFD | D5D7F77FD757 |
| TDMA direct mode TS1 | 5D577F7757FF | F7FDD5DDFD55 |
| TDMA direct mode TS2 | 7DFFD5F55D5F | D7557F5FF7F5 |
| RC (MS) | 77D55F7DFD77 | |

The receiver accepts the MS, BS, and both direct mode sync families, and reports which one it heard:

- BS sync is a repeater's outbound.
- MS sync is used inbound to a repeater and, by most radios, in simplex.
- TS1 and TS2 sync are TDMA direct mode: dual capacity direct mode (DCDM) and some SFR framings.

### 1.4 Data types (slot type, clause 9.3.6)

| DT | Burst | Payload |
|---|---|---|
| 0 | PI header | 96 bits (privacy) |
| 1 | voice LC header | 72-bit LC + RS(12,9) with mask 0x969696 |
| 2 | terminator with LC | 72-bit LC + RS(12,9) with mask 0x999999 |
| 3 | CSBK | 80 bits + CRC-16 mask 0xA5A5 |
| 4 / 5 | MBC header / continuation | 80 bits + CRC-16 mask 0xAAAA (last block: CRC over the whole MBC) |
| 6 | data header | 80 bits + CRC-16 mask 0xCCCC |
| 7 | rate 1/2 data | 96 bits |
| 8 | rate 3/4 data | 144 bits |
| 9 | idle | 96 bits |
| 10 | rate 1 data | 192 bits |
| 11 | unified single block data | 96 bits |
| 12-15 | reserved | |

The PI header CRC mask is 0x6969.

### 1.5 Full LC (TS 102 361-2 clause 7.1.1)

The full LC is nine bytes:

| Bytes | Field |
|---|---|
| 0 | Protect flag (bit 7), FLCO (bits 5-0) |
| 1 | FID |
| 2 | Service options: bit 7 emergency, bit 6 privacy, bit 3 broadcast, bit 2 open voice call mode, bits 1-0 priority |
| 3-5 | Destination |
| 6-8 | Source |

FLCO 0 is group voice and 3 is unit to unit (private). Embedded data uses other FLCOs: 4 is the talker alias header,
5-7 are talker alias blocks 1-3, and 8 is GPS info (TS 102 361-2 7.1.1.3-7.1.1.4).

### 1.6 Timing structure

- **Slot and frame.** A slot is 30 ms and a frame is 60 ms (TS1, then TS2). A talker on one slot sends one burst
  every 60 ms.
- **Superframe.** Bursts A (sync) and B-F (EMB) take 360 ms. The embedded LC (72 bits + 5-bit checksum, BPTC(128,77))
  is spread over bursts B-E as 4 x 32 bits (LCSS 1 first, 3 continuation, 2 last). Burst F carries null embedded
  data, the reverse channel, or privacy information. Every EMB carries the color code and PI.
- **CACH** (BS outbound only). Each slot has 24 bits: the TACT (AT (1), TC (1), LCSS (2), and Hamming(7,4) parity) and
  17 bits of a short LC fragment. TC names the timeslot that follows (0 = TS1, 1 = TS2); it's the only way an MS learns
  the slot number. AT (access type) says whether the inbound slot that starts one slot later is busy (1) or idle (0)
  (-1 4.5 figure 4.8). Repeaters run aligned timing (-1 5.1.1, table 5.1: a subscriber on TSx transmits under
  outbound TSx), so the AT before outbound TSx belongs to the other channel. An MS on TSx reads its busy state from the
  CACH before the other outbound slot (`split-duplex.md` 5.2 and 7).
- **Voice call on air.** One or more voice LC headers (DT 1), an optional PI header (DT 0) for privacy, superframes,
  and one or more terminators with LC (DT 2). A call ends at a superframe boundary (after burst F).
- **Repeater outbound.** The outbound runs continuously while active. After a call, the repeater keeps it up for the
  hang time (idle bursts or terminators with the call's LC), then drops it.

### 1.7 Channel access (TS 102 361-1 clause 5.2)

| Policy | Rule before transmitting |
|---|---|
| impolite | transmit regardless |
| polite to own color code | don't transmit while the channel carries our CC (other CCs are ignored) |
| polite to all | don't transmit while the channel carries any DMR or RF activity |

Through a repeater, "busy" is the AT bit for our channel (in the CACH before the other outbound slot, as in 1.6), plus
decoded activity on our slot. In direct mode, "busy" is decoded activity with our CC, or carrier for polite to all. A
data or voice transmission that waits for the channel times out after a few seconds (configurable); it never waits
forever.

## 2. Mobile station states

```
                    +--------- carrier / sync lost (timeout) ----------+
                    v                                                  |
  IDLE (listen) --sync+CC--> RX_SYNCING --slot locked--> RX_VOICE / RX_DATA --terminator / timeout--> IDLE
     |                                                       |
     | PTT / data queued                                     | PTT while hearing repeater hang time
     v                                                       v
  [RMO, repeater not heard] --> BS_ACTIVATION --repeater heard--> TX_ALIGN --> TX_HEADER --> TX_VOICE --> TX_TERMINATOR
  [RMO, repeater heard / DMO] -------------------------------------^             \--> TX_DATA ---------------^
                                                                                               |
                                                                      HANG (listen on own slot) <---------+
```

The diagram shows behavior, not a structure to copy. The following sections give what each state requires.

### 2.1 Idle / listen

- The modem receives every slot (no slot lock yet), with sync detection for all four families.
- The color code is either fixed (only bursts with the channel's CC are deframed) or "follow". In follow mode the
  receiver takes a new CC once it's seen repeatedly. On the HR_C6000 that is 8 consecutive matching reports, under
  200 ms apart, with an SNR margin of 5 dB or more; the CC is held while audio is heard and released after 5 s idle.
- A burst with a matching CC moves to RX.

### 2.2 Receiving

- **Mode detection.** BS sync means RMO reception, with the slot number from the CACH TC. MS, TS1, or TS2 sync means
  DMO reception, unless the channel forces DMO or the radio is an SFR user or repeater, which stays DMO.
- **Slot lock (RMO).** The receiver tracks TC. More than 4 agreements in a row give the slot phase; it then receives
  only the wanted slot. More than 3 disagreements cause a resync. With the slot filter off, the "captured" slot is the
  first one that carries a call; after the capture timeout of silence on it, the receiver tries the other slot.
- **Slot lock (DMO).** The receiver locks to the slot of the first valid burst and receives one slot in two.
- **Filters.** Every burst passes these filters:
  - Color code.
  - Timeslot (RMO, when the slot filter is on).
  - Destination: talkgroup, receive group, a private call to us, or all call 0xFFFFFF, which passes everything.
  - Privacy indicator: bursts with PI set play only when privacy is configured for that call.
- **Start of call.** A voice LC header with a good CRC, or late entry (2.4), starts a call. The caller isn't announced
  from the LC until the LC is known to be this call's; a modem can hold the previous call's LC (2.4).
- **During voice.** Every burst A-F goes to the vocoder in order, and silence replaces a missing burst. The embedded
  LC is checked again each superframe, because the talker can change between overs on a repeater without a header.
- **End of call.** A call ends on any of these:
  - A terminator with LC, in DMO only. Through a repeater the MS hears its own terminators echoed, so it doesn't end
    its next reception on them; it uses the timeout.
  - No valid burst on the slot for about 390 ms (13 slot ticks).
  - Loss of slot timing.
- **Data.** CSBK, MBC, data headers, and blocks on our slot go to the data service. Blocks are taken even with a CRC
  error, because the packet CRC-32 decides; headers and CSBKs with a bad CRC are dropped.

### 2.3 Repeater mode and direct mode timing

| | RMO | DMO (simplex, MS sync) | DMO (TDMA direct, DCDM) |
|---|---|---|---|
| Who sets the slot clock | the repeater (CACH, outbound bursts) | the first talker; the receiver follows its bursts | the first talker; both slots usable |
| Slot number known from | CACH TC | not known (only "the slot heard") | sync family (TS1 / TS2 sync) |
| Our TX slot | the channel's TS, aligned to the outbound | the slot phase we start in, or the one heard (reply aligned to the talker) | the channel's TS, aligned to the other slot's traffic if heard |
| Busy from | AT bit + decoded traffic | decoded traffic | decoded traffic on our slot |
| End of call | timeout (echoes) | terminator or timeout | terminator or timeout |
| Modem timing mode | passive (follow received) | active (own) when starting, passive when receiving | passive when the other slot is busy, otherwise active |

A single-frequency repeater (SFR) user is DMO framed, but transmits in the slot opposite to the one it hears from the
repeater, aligned to it ([SFR](sfr.md)). The modem therefore supports "start transmitting in the slot after the next
one, in phase with the bursts being received".

### 2.4 Late entry

- A receiver that missed the header acquires the call from the embedded LC. That takes bursts B-E of one superframe:
  360-720 ms after the first burst heard.
- Some modems announce a late entry (the HR_C6000 has a post-access interrupt); others only report voice bursts with
  the embedded LC. The stack handles both: voice bursts with no header before them start a call once the embedded LC
  is complete.
- **Stale LC.** The HR_C6000 keeps the previous call's LC in its RX LC buffer until it decodes a new one. The new call
  must not be attributed to that LC. The driver handles this once, because it knows its chip's buffer, and a voice
  burst's embedded LC marked valid belongs to this reception (6.1 rule 4):
  - The HR_C6000 driver captures the buffer when a reception starts, and accepts an unchanged LC after more than one
    superframe of voice.
  - The HR_C7000 driver hands over only the LCs that its late-LC interrupt reports with a good checksum.

  The stack adds no stale check of its own on top, for example against the previous call's LC. When the same talker
  calls again, a second check costs one more superframe for each layer.
- The stack skips the first two bursts A of a late entry before it plays audio, unless the source is already known and
  isn't us.
- A call that faded (no terminator) and comes back within 1.5 s (`DMR_CALL_RESUME_MS`), with the same LC on the same
  slot, is one call with a hole in it. The sink sees it end and begin, `rx_calls` counts it once, and
  `rx_calls_resumed` counts the return. Proving that the LC reaching the stack belongs to this call is still the
  driver's job: an LC in a sticky buffer doesn't show that the same call is on air
  ([modem-hrc6000.md](modem-hrc6000.md) 5.6).
- A lone voice burst (noise, a stray) doesn't start a call: a burst B must follow a burst A by 60 ms (as in SFR). An LC
  that comes with the first burst is held with that burst until the next burst completes the pair; then both play.
  The HR_C7000 reports voice only once it has the LC, so its first burst carries the LC; dropping that LC costs a
  superframe of audio.
- When the PI header is missed, the privacy parameters come from the embedded or burst F privacy information. This is
  a risk for privacy schemes.

### 2.5 Transmitting voice

The call stack configures the modem with the channel's transmit color code, even when the receive filter accepts any
CC (any-CC listening, polite-to-all access, or a dual-CC SFR user). `set_cc_filter` changes reception only. TS 102
361-1 9.1.2 (EMB), 9.1.3 (slot type), and 9.3.1 (CC) define the CC fields.

A voice transmission follows these steps:

1. **Channel access** follows the policy (1.7). When the channel is busy, the stack waits (with a timeout) or refuses.
2. **BS activation** (2.7) runs in RMO when no repeater is heard.
3. **Alignment.** In RMO, the stack waits until the slot tracking is stable, then transmits in the slot after the next
   one that carries our TS. In DMO, it starts on the modem's own timing, or aligns to a call it hears (a reply).
4. **Headers.** Three voice LC headers go out on our slot, one per 60 ms: 180 ms of lead-in. A PI header follows for
   privacy schemes that need one.
5. **Voice.** Superframes A-F follow. Burst A has the voice sync; B-E carry the embedded LC, or talker alias or GPS
   embedded data in alternate superframes; F carries null embedded data or privacy data. When the encoder has no
   frame ready, the stack sends AMBE silence (B9 E8 81 52 61 73 00 2A 6B per 20 ms frame).
6. **End.** PTT release takes effect at the next superframe boundary, and the terminator with LC goes out on our slot.
7. **Hang.** After transmitting, the receiver holds the slot on our TS (capture timeout), so that it hears the reply
   there.
8. **TX timeout.** The TX timeout timer (a user setting) forces the end.

### 2.6 Transmitting data and CSBK

- A list of bursts (CSBK preambles, data header, and blocks, or single CSBKs) goes out one per frame on our slot, with
  no voice header and no terminator. Channel access and BS activation work as for voice. A transmission carries up to
  64 bursts (`DMR_DATA_MAX_BURSTS`); its timeout of 10 s plus 60 ms per burst covers a wake-up.
- The data service has a "send this payload verbatim, the CRC is supplied" option (`dmr_tx_burst_t.host_crc`), for raw
  passthrough of CSBKs and data headers from a host. The HR_C7000 and HR_C5000 always compute the CSBK and data header
  CRC, so they can't pass through a burst with a deliberately wrong CRC. On those modems the stack gives the payload
  without its CRC, and the modem adds it.

### 2.7 BS activation (repeater wake-up)

- The stack sends a BS_Dwn_Act CSBK: CSBKO 0x38, LB=1, FID 0, BS address 0 (any), and our own ID as the source. The
  octets are B8 00 00 00 00 00 00 followed by the source (3). The HR_C7000 sends it in two consecutive slots with the
  PA keyed; the HR_C6000 sends one burst.
- The stack then listens. The repeater is up when its outbound is heard with the slot number toggling correctly more
  than 3 times in a row.
- With no answer within 600 ms, the stack sends again, up to the configured number of attempts, and then reports the
  failure to the user.
- The CSBK goes out on our own timing (`DMR_TIMING_OWN`): no repeater is heard, so there is nothing to align to. On the
  timing the HR_C6000 last had, from a repeater that had just dropped, it sends nothing, because its passive timing has
  no repeater left to follow. A CSBK whose transmission ends without going on air (`DMR_EV_TX_DONE` with no
  `DMR_EV_TX_SLOT_STARTED`, and the modem no longer sending) is sent again at once as the next attempt
  (`call.tx_wake_lost`).
- While a key-up waits for its first slot (the wake-up queued, or the aligned voice start not yet on air), the modem's
  slot numbers don't count as the repeater heard. The modem labels slots from the timing it last had, which would keep
  a repeater that dropped "up", and a repeater that dropped as the hang ended would get no wake-up.
- The modem can give up a voice start through a repeater before its first burst (`DMR_EV_TX_REJECTED`, or
  `DMR_EV_TX_DONE` with nothing sent: no slot timing to align to). The stack then wakes the repeater again instead of
  refusing the call: once per key-up, only while PTT is held, with the whole wake-up's attempts. A second such start is
  refused. The refused start's own `DMR_EV_TX_DONE` comes after its rejection, so a wake-up waits for its CSBK's
  `DMR_EV_TX_SLOT_STARTED` before it takes a `DMR_EV_TX_DONE` as its own.
- Where the modem reports the repeater's bursts (received, or heard between ours), only those bursts show that the
  repeater is up; its slot numbers don't. The slot numbers run on from the timing the modem last had, so a repeater
  that dropped would count as up for a key-up 0.6 s later, and a second wake-up would count as answered.
- Our own ID as the source during a key-up is our echo: once a wake-up starts the repeater, it replays an over it heard
  while down. The echo never makes the channel busy.
- A start through a repeater that never gets a slot (no timing at all, so the modem's own wait never runs out) is cut
  by the stall timeout. It wakes the repeater again, as above, instead of being refused.
- A short PTT press still runs the whole wake-up: the user only starts the call. On release, the stack keeps listening
  and retrying. If the repeater answers after release, nothing is sent, the call view's `wake_admits` counter goes up,
  and the radio plays its talk permit tone. If no repeater is heard in the last attempt, the call ends with
  `DMR_TX_ERR_NO_REPEATER`, and the radio plays its deny tone.

### 2.8 Hang time and TX hold-off

**Repeater drop under our over.** The repeater can drop under our over: its hang ended as we keyed, and it took our
voice as nothing. Learning each repeater's hang to predict this doesn't generalize (hangs differ per repeater and per
call type), so the stack reacts instead:

- Where the radio's RF can receive the repeater's outbound between our bursts on a split channel (the modem's
  `tx_listen_split`; `DMR_CALL_TX_LISTEN`, `dmr_call_set_tx_listen`), a voice over through a repeater listens there.
- A repeater burst is BS sync on our color code. A neighbor's burst on another color code (heard where the modem
  doesn't filter the color code) isn't ours.
- No repeater burst for `DMR_CALL_TX_DROP_MS` (240 ms) once our bursts are on air means the repeater dropped
  (`call.tx_rpt_drops`). The over ends with a terminator, not an abort: an abort mid-voice leaves the next CSBK
  undecodable on the HR_C5000. The stack wakes the repeater again (2.7, once per key-up) and starts the voice again.
  After that wake-up, only the repeater's own bursts count as its answer.
- The same holds before our first burst. An aligned start waits for the repeater's slot, and a repeater that dropped as
  we keyed (its last bursts taken as up, or as the answer to our wake-up) gives none. With no repeater burst for
  `DMR_CALL_TX_START_DROP_MS` (540 ms) since the start, the stack stops the start and wakes the repeater, instead of
  waiting for the modem's own 1.5 s timeout, which is longer than a short key-up. This limit is longer than the one
  under the over: after our wake-up, the HR_C5000 at times hears nothing of a repeater that is up until the modem
  searches again (every 240 ms, [modem-hrc6000.md](modem-hrc6000.md) 5), so the modem gets two searches first.
- `call.rx_in_tx` and `call.rx_bs_in_tx` count what was heard while sending.
- A radio whose RF can't listen (the modem's `tx_listen_split` is NULL or returns false) sends as before and doesn't
  see the drop. Neither does a modem that hears the repeater in only some of the listen slots, at times in none for
  seconds (`tx_listen_gaps`): it listens for the reverse channel only, and silence there isn't a drop.

**Reverse channel** (ETSI TS 102 361-1 5.1.5, 6.4.2; TS 102 361-4 6.4.14, 6.4.15.2). A radio that listens between its
bursts also hears a reverse channel (RC) that the BS sends on the other slot: either a dedicated RC burst, or one in
voice burst F (EMB LCSS 00, PI 1).

- The modem reports the 11 RC info bits (`DMR_EV_RC`, with `DMR_RC_EMBEDDED` for burst F), and the stack checks the
  CRC-7 (`dmrFecRcInfoDecode`).
- A cease command (0100) or cease request (0101) heard twice within `DMR_CALL_RC_CONFIRM_MS` (1 s) while our voice goes
  out ends the over after burst F with a terminator, like the TX timeout: `tx_error` is ceased, and PTT must be
  released before the next over. Twice, because burst F's null fragment passes the CRC one time in 128, and the BS
  repeats an RC every 360 ms. A request is always accepted.
- Power commands (0000-0011) are only counted; the power stays as set.
- The diagnostics and the `dmr_call` view section show `call.rc_good`, `call.rc_bad`, `call.rc_ceased`, and
  `call.rc_last`.
- The HR_C6000 engine reads the RC info from 0x53 and 0x54 at a class 3 report, or at a burst F report with LCSS 00
  ([hrc6000-registers.md](hrc6000-registers.md) 2.2).
- A dedicated RC burst has no sync, and the chips report nothing for it. After each listen slot, the engine reads the
  chip's receive tap instead (on the HR_C5000 and HR_C6000, `hrc_chip_t` `tap_addr` and `tap_field`; on the HR_C7000,
  [modem-hrc7000.md](modem-hrc7000.md) 5.4). The tap holds the burst received, with a voice-layout burst's 48-bit
  sync, EMB, or RC field first. The engine decodes the field (`dmrFecRcFromField`) when its EMB has our color code,
  PI 1, and LCSS 00. It decodes only an area that changed since its last read, and none that the chip reported itself
  in that listen slot, so one burst counts once. `rct` in the modem diagnostics shows RCs found and areas changed.

**Hang time and keying rules.**

- The repeater keeps the outbound up for its call hang time after a call. An MS that keys in the hang time needs no BS
  activation, and replies on the same slot (and talkgroup, where group hang applies).
- After hearing a call on our slot, a polite MS waits for the terminator or timeout before keying; an impolite one keys
  at the next aligned slot.
- **Talk permit before keying** (`dmr_call_config_t.permit_hold_ms`). On a radio whose beeps can't sound on air, an
  admitted voice call (the channel free, the repeater up) waits in `TX_PERMIT`, still receiving, while the radio plays
  its talk permit tone. It keys after at least `permit_hold_ms`, once `permit_busy()` is false, and at most
  `DMR_CALL_PERMIT_MAX_MS` (400 ms) after the hold began. The call view counts each hold in `permit_seq` and shows it
  as its own phase (`DMR_CALL_PHASE_TX_PERMIT`; the screen says ADMITTED). Releasing PTT in the hold ends the call, as
  in TX access. A call heard in the hold sends the call back to TX access, as if the heard call had been there at PTT:
  it's refused, or waits out `access_wait_ms` and holds again with a new permit. Through a repeater the key-up is
  impolite, so without the hold it would go over the call.
- After our own transmission, the receiver keeps the slot lock and the call's destination for the capture timeout.

## 3. Privacy hooks

- **Basic privacy** (the common 16-bit key scheme). The voice LC carries the privacy bit (service options bit 6) and a
  manufacturer FID. The AMBE bits of each burst are XORed with a key-derived pattern that depends on the burst position
  (A-F). The modem gives the stack the raw 27-byte AMBE before decoding and takes raw AMBE for transmission, with the
  burst position. The EMB PI bit is set on the voice bursts of an encrypted call.
- **Receivers without the key** drop bursts with PI set and don't play them.
- **Enhanced and AES schemes** need a PI header (DT 0) before the voice, and the per-superframe privacy information in
  burst F. The modem must let the stack send a PI header burst and set the burst F embedded content, or at least pass
  them through. Late entry needs the IV from burst F; the HR_C6000 path doesn't support it.
- The HR_C6000 has its own "end-to-end encryption" interrupts (TX-start sub-status bit 5). The firmware doesn't use
  them.

## 4. Talker alias and embedded data

- **TX.** In alternate superframes, the stack replaces the embedded LC with one talker alias block or a GPS embedded
  LC. The talker alias header (FLCO 4) carries the format, the length, and the first 6 characters; blocks 1-3 (FLCO
  5-7) carry 7 characters each. The GPS embedded LC (FLCO 8) carries a 25-bit longitude scaled 2^25/360 and a 24-bit
  latitude scaled 2^24/180. The modem accepts a new embedded LC each superframe.
- **RX.** The modem reports each assembled embedded LC (FLCO 0, 3, 4-8) once per superframe. The stack assembles the
  alias and decodes the GPS position.

## 5. What the modems provide

| | HR_C6000 | HR_C7000 | HR_C5000 |
|---|---|---|---|
| Host interface | external chip; config SPI (page/register) + voice SPI + I2S | on-chip peripheral of the CK803S SoC, memory-mapped 32-bit registers | external; like the HR_C6000 but config page 0 |
| Interrupts | timeslot, system, RF TX (RF RX not routed on the `stm32f4` boards) | system (vector 61), TX timeslot (62), RX timeslot (63), RF TX (64) | timeslot, system, RF |
| RF TX lead | | 3.1 ms before the burst with its RF control register at 0x41F1A (1.0 ms with 0x40A0A) | |
| PA off | MCU, at the next timeslot interrupt (>20 ms after on) or a 29 ms one-shot | one-shot timer, 28 ms default (user setting) | 30 ms timer |
| Burst status | one byte: data type / voice A-F (1-6 + voice bit), sync class, CRC flag, PI; separate CC/CACH and sync type registers | one 32-bit word: data type bits 11-8, voice type bits 14-12, sync class bits 1-0, error flags bits 30-24 | similar to the HR_C6000 |
| Reports per voice burst | two (EMB, then burst) | the manual also lists an EMB report | |
| CRC of CSBK / data header | chip or MCU (CRC_MCU_Control) | chip only | chip only |
| Vocoder | AMBE on the MCU (binary blob), PCM over I2S | AMBE blob run in place from flash, tables in modem RAM | AMBE on the MCU |
| Timing modes | passive / active per register; "send next slot" / "receive next slot" / "resync" per slot | same model (LAYER2 control / TX-RX control / slot-on registers) | same model |
| RX restart | TX+RX enable passive, then sync-fail, clear, sync-fail, receive-next | slot-on off, sync-fail, receive-next, slot-on (RX+TX) | |

All three chips share one model: at each slot boundary, the host decides what the slot after next does (send a burst
of a given type, receive, or nothing), and the chip frames, codes, and times it. The HAL keeps that model.

## 6. The `dmr_modem` HAL contract

The driver owns the chip, its interrupts, and the per-slot register work. The DMR stack owns calls, filters, policy,
and the data service. They meet through a TX queue and RX events, so the stack runs at task level and never inside
the slot interrupt. The contract is `hal_dmr_modem_t` in `hal/hal_dmr_modem.h`.

### 6.1 Principles

1. **The driver keeps the slot clock.** It numbers slots with a free-running counter and, when known, with the TDMA
   slot (TS1/TS2) and whether it's in step with a received signal.
2. **Lookahead TX queue.** The stack queues bursts ahead of time. The driver takes one at each of our slots and sets it
   up from its own interrupt. A burst for slot N must be queued before the boundary of slot N-1 (30-60 ms ahead). An
   empty queue in the middle of a voice call means "send AMBE silence"; in data, it means "end of list".
3. **The driver copies RX bursts out** in the interrupt, because the modem buffers are overwritten every slot, and
   delivers them as events. The stack has no hard deadline beyond the queue depth (at least 8 bursts).
4. **The driver hides modem quirks.** It gives one report per burst (it drops the HR_C6000's EMB double report), voice
   burst index 0-5, a normalized CRC flag, and flags a stale LC.
5. **"Transmitting" is the driver's slot state**, from the first burst set up to the end of the last burst's slot. It's
   never the PA state, because the PA switches per burst.

### 6.2 Events (driver to stack)

The driver calls the stack's `dmr_event_cb_t` from task level (its own deferred handler), never from the slot
interrupt.

| Event | Contents | Timing |
|---|---|---|
| `DMR_EV_SLOT_TICK` | slot counter, TS if known (from CACH TC or sync family); flags: the AT bit of the last CACH that named this slot, when known and fresh (`DMR_TICK_AT_KNOWN`, `DMR_TICK_AT_BUSY`) | every 30 ms; delivered within 2 ms of the boundary; informative (the stack never has to answer it in time) |
| `DMR_EV_BURST_RX` | `dmr_burst_t`: kind (voice A-F index / data type), sync family (MS/BS/TS1/TS2), color code, CRC ok, PI, TS if known, in-step flag (report 2-10 ms after the boundary), RSSI if available, payload (LC 9+3 bytes, data 12/18/24 bytes, voice 27 bytes) and the LC when complete; arg = the slot counter when heard (0 if the driver keeps none) | within 10 ms after the burst's slot ends; one event per burst on air, in air order |
| `DMR_EV_VOICE_LOST` | voice sync lost on a slot (the next voice frame is silence) | |
| `DMR_EV_TX_SLOT_STARTED` / `DMR_EV_TX_SLOT_ENDED` | per transmitted burst (RF keyed / unkeyed), for diagnostics and RF switching on boards where the driver doesn't own it | at the RF TX interrupt / PA off |
| `DMR_EV_TX_REJECTED` | the modem refused to send (channel busy under a polite policy) | |
| `DMR_EV_TX_DONE` | the last queued burst's slot has ended (terminator included); the driver is back in receive | after the last burst's slot, not before |
| `DMR_EV_SYNC_LOST` | no slot tick for 200 ms while not idle, or the timing was found out of step; the driver has restarted the receiver | |
| `DMR_EV_SHORT_LC` | a BS's CACH short LC (ETSI TS 102 361-1 7.1.4) as the chip assembled it: 28 bits and the CRC-8 received. The stack checks the CRC (`dmr_call_short_lc`). Optional: only while both slots of a BS signal are received (a trunked control channel), from chips whose short LC location is known | when it changes; a new one every 120 ms |
| `DMR_EV_RC` | a reverse channel heard between our bursts: the 11 RC info bits, CRC unchecked; `DMR_RC_EMBEDDED` when it was in a voice burst F (2.8). Optional | |

Events can reach the stack in batches, so the stack orders and de-duplicates bursts by voice index and slot counter,
never by arrival time. A late entry needs no event of its own: voice bursts with a complete embedded LC start the call
(2.4).

### 6.3 Commands (stack to driver)

| Operation | Effect |
|---|---|
| `start` / `stop` | chip on and in DMR mode (after the board's RF side is in DMR mode), events start flowing / back to the chip's FM or idle state |
| `configure` | own DMR ID, the channel's transmit color code and initial receive filter (-1 = follow), default access policy (impolite / polite to CC / polite to all), DMO / RMO framing |
| `rx_restart` | drop sync and search again, every slot, passive timing. The radio core restarts at every new DMR frequency with the stack running (a scan or channel step): a chip can keep the last transmitter's slot timing otherwise ([gotchas](gotchas.md) 17.2) |
| `rx_lock_slot(ts)` | receive only TS 1 or 2, or both (0) |
| `tx_begin(timing, ts, access)` | start a transmission: own timing (DMO), aligned to the received signal on the next slot carrying a given TS (RMO, SFR user), or as soon as possible; with the access policy for this transmission |
| `tx_queue` / `tx_queue_space` | one burst (`dmr_tx_burst_t`): kind (voice header, voice A-F with 27-byte AMBE, terminator, PI header, CSBK, data header / block, idle, raw DT, or an empty slot), payload, host-supplied CRC flag, sync family override (SFR outbound); false when the queue (depth 8 or more) is full |
| `tx_end` | no more bursts: send the queued ones (a voice call ends at the superframe boundary with its terminator, which the stack has queued), then `DMR_EV_TX_DONE` |
| `tx_abort` | stop at the next slot (emergency, timeout) |
| `set_embedded_lc` | the LC (or talker alias / GPS embedded LC) the modem spreads over B-E from the next superframe |
| `set_cc_filter(cc)` | receive color code filter (-1 = any); keeps the configured transmit color code |
| `is_transmitting` | slot-state based (6.1 point 5) |
| `tx_listen`, `tx_listen_split` | optional: receive in the slots between our bursts, on the same channel or, retuned, on a split channel (2.8, [SFR](sfr.md) 3) |
| `tx_fill` | optional: put a burst in place of the oldest queued empty slot (an SFR's repeat) |
| `health` | optional: lost modem interrupts found and recovered from |

The split duplex operations (`now_us`, `slot_clock`, `bs_*`, under `CONFIG_HOTSPOT_SPLIT`) serve the TX and RX halves
of [split-duplex.md](split-duplex.md) 5-7.

### 6.4 Timing requirements on a driver

- Slot ticks every 30.000 ms +- 1.25 ms when following a signal (the chip's real-time sync correction window), with no
  missed tick while receiving or sending.
- Bursts set up from the driver's own interrupt for the slot after next, with no stack round trip.
- RF keyed before the burst (HR_C7000: 3.1 ms lead) and unkeyed after it, within the 2.5 ms guard time. A burst is
  about 27.5-31 ms on air, with sub-millisecond ramps.
- A `DMR_EV_BURST_RX` event for every burst heard on a slot being received, within 10 ms of its slot.
- `DMR_EV_TX_DONE` only once the final burst (terminator, last data block) has left the antenna.
- Receiving again within a few milliseconds of the last burst. A confirmed data response comes in the next frame, 30 ms
  after our terminator ([dmr-data.md](dmr-data.md) 12.9).
  - The glue keeps a back-to-receive flag. The flag is set when the last burst switch put the receiver back, or when
    `DMR_EV_TX_DONE` did on a board that keeps the synthesizer on TX between bursts. The next burst, a new channel,
    and entering or leaving DMR clear it.
  - The core ends the transmission through `hal_radio_t.dmr_tx_end` (`hrc_dmr_tx_end`, `stm32_hrc_dmr_tx_end`). While
    the flag is set, the board's `tx_end` hook ends it and leaves the receiver as it is on the same channel; otherwise
    the core calls `tx_off()`. `rx()` is always a full set-up ([gotchas.md](gotchas.md) 3.14).
- Recovery: the driver re-initializes a stalled modem (no tick for 200 ms) and reports `DMR_EV_SYNC_LOST`.

### 6.5 What the stack must not assume

- That the modem announces a late entry: it might only report voice bursts.
- That the LC buffer is fresh at the start of a call.
- That reports about received bursts are reliable while it transmits. On the HR_C6000, the CRC, CC, sync type, and
  header info bits are corrupt while sending.
- That the PA state means "transmitting".
- That the modem can send a CSBK or data header with a wrong CRC (HR_C7000, HR_C5000).
- That voice burst numbering is 0-5 or 1-6: the driver normalizes it to 0-5.
