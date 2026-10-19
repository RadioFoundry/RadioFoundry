<!-- SPDX-License-Identifier: MIT -->

# DMR modem behavior and the `dmr_modem` HAL contract

What a fresh DMR Tier II stack must do on top of any baseband modem (Hongrui HR_C6000, HR_C7000, HR_C5000, a
future software modem), and the interface a modem driver offers it. Protocol facts are from ETSI TS 102 361-1 (air
interface), -2 (voice and generic services) and -3 (packet data); clause numbers are given where useful. Modem
facts come from `modem-hrc6000.md` (HR_C6000, on air on the DM-1701) and from the HR_C7000 port (DM-32, built and
checked against disassembly, not yet on air), and are marked as such.

Terms: **slot** = one 30 ms TDMA timeslot; **frame** = two slots (60 ms); **superframe** = six voice bursts A-F on
one slot (360 ms); **inbound** = MS to BS (repeater input); **outbound** = BS to MS; **RMO** = through a repeater,
**DMO** = direct mode (no repeater).

---

## 1. Air interface facts the stack relies on

### 1.1 Physical layer (TS 102 361-1 clause 10)

- 4FSK, 4800 symbols/s, 9600 bit/s gross, 12.5 kHz channel. Deviation +-648 / +-1944 Hz (dibits 01/00/10/11 map to
  +3, +1, -1, -3).
- One slot carries one burst: 264 bits (27.5 ms) plus 2.5 ms that is either the CACH (BS outbound, 24 bits) or guard
  time for the PA ramp (MS inbound, and direct mode).
- The modem does symbol timing, sync search, burst framing and FEC. The stack never handles symbols.

### 1.2 Burst layout (clause 4.2, 6)

| Burst | First half | Middle 48 bits | Second half |
|---|---|---|---|
| Voice with sync (burst A) | 108 bits AMBE | voice sync | 108 bits AMBE |
| Voice with embedded signaling (B-F) | 108 bits AMBE | 8 EMB + 32 embedded signaling + 8 EMB | 108 bits AMBE |
| Data / control | 98 bits info | 10 slot type + 48 data sync + 10 slot type | 98 bits info |

- Voice payload: 216 bits = three 72-bit AMBE+2 frames (20 ms each, with their FEC). The modem drivers exchange it as
  27 bytes.
- Slot type: color code (4) + data type (4) + Golay(20,8) parity.
- EMB: color code (4), PI (1), LCSS (2), QR(16,7) parity.
- Data info: 196 bits = BPTC(196,96) for 96-bit payloads (rate 1/2, headers, CSBK, LC), trellis rate 3/4 for 144-bit
  payloads, no FEC for 192-bit rate 1 payloads (rate 1 fails on any single bit error).

### 1.3 Sync patterns (clause 9.1.1)

| Source | Voice | Data |
|---|---|---|
| BS | 755FD7DF75F7 | DFF57D75DF5D |
| MS | 7F7D5DD57DFD | D5D7F77FD757 |
| TDMA direct mode TS1 | 5D577F7757FF | F7FDD5DDFD55 |
| TDMA direct mode TS2 | 7DFFD5F55D5F | D7557F5FF7F5 |
| RC (MS) | 77D55F7DFD77 | |

The receiver must accept MS, BS and both direct mode sync families (and say which one it heard): BS sync means a
repeater's outbound; MS sync is used both inbound to a repeater and, by most radios, in simplex; TS1/TS2 sync is
TDMA direct mode (dual capacity direct mode, DCDM, and some SFR framings).

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

PI header CRC mask: 0x6969.

### 1.5 Full LC (TS 102 361-2 clause 7.1.1)

Nine bytes: FLCO + PF (byte 0: protect flag bit 7, FLCO bits 5-0), FID (byte 1), service options (byte 2), destination
(bytes 3-5), source (bytes 6-8). FLCO 0 = group voice, 3 = unit to unit (private). Service options: bit 7 emergency,
bit 6 privacy, bit 3 broadcast, bit 2 open voice call mode, bits 1-0 priority. Embedded data uses other FLCOs: 4 talker
alias header, 5-7 talker alias blocks 1-3, 8 GPS info (TS 102 361-2 7.1.1.3-7.1.1.4).

### 1.6 Timing structure

- **Slot** 30 ms; **frame** 60 ms (TS1 then TS2); a talker on one slot sends one burst every 60 ms.
- **Superframe**: bursts A (sync) and B-F (EMB) = 360 ms. The embedded LC (72 bits + 5-bit checksum, BPTC(128,77))
  is spread over bursts B-E as 4 x 32 bits (LCSS 1 first, 3 continuation, 2 last); F carries null embedded data, the
  reverse channel or privacy information. The color code and PI are in every EMB.
- **CACH** (BS outbound only): 24 bits per slot: TACT = AT (1), TC (1), LCSS (2) + Hamming(7,4) parity; 17 bits of a
  short LC fragment. **TC** says which timeslot follows (0 = TS1, 1 = TS2): it is the only way an MS learns the slot
  number. **AT** (access type) says whether the inbound channel of that slot is busy (1) or idle (0).
- **Voice call on air**: one or more voice LC headers (DT 1), optionally a PI header (DT 0) for privacy, then
  superframes, then one or more terminators with LC (DT 2). Calls should end at a superframe boundary (after F).
- **Repeater outbound**: continuous while active; after a call the repeater keeps the outbound up for the hang time
  (idle bursts / terminators with the call's LC), then drops it.

### 1.7 Channel access (TS 102 361-1 clause 5.2)

| Policy | Rule before transmitting |
|---|---|
| impolite | transmit regardless |
| polite to own color code | don't transmit while the channel carries our CC (other CCs are ignored) |
| polite to all | don't transmit while the channel carries any DMR or RF activity |

Through a repeater, "busy" is the AT bit of the CACH for our slot, plus decoded activity on our slot. In direct mode,
"busy" is decoded activity with our CC (or carrier, for polite to all). A data or voice transmission that waits for
the channel must time out (a few seconds, configurable) rather than wait forever.

---

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

This is a behavioral diagram, not a structure to copy. The states and what they require:

### 2.1 Idle / listen

- The modem receives every slot (no slot lock yet), with sync detection for all four families.
- Color code: either fixed (only bursts with the channel's CC are deframed) or "follow": accept a new CC when it is
  seen repeatedly (HR_C6000 practice: 8 consecutive matching reports, under 200 ms apart, with an SNR margin of 5 dB or
  more; held while audio is heard, released after 5 s idle).
- A burst with matching CC moves to RX.

### 2.2 Receiving

- **Mode detection**: BS sync => RMO reception (slot number from CACH TC). MS / TS1 / TS2 sync => DMO reception
  (unless the channel forces DMO, or the radio is an SFR user/repeater, which stays DMO).
- **Slot lock** (RMO): track TC; a stable count of agreements (more than 4) gives the slot phase; then receive only
  the wanted slot. Too many disagreements (more than 3) => resync. If the slot filter is off, the "captured" slot is
  the first one carrying a call; after the capture timeout of silence on it, try the other slot.
- **Slot lock** (DMO): lock to the slot of the first valid burst; receive one slot in two.
- **Filters** applied to every burst: color code; timeslot (RMO, if the slot filter is on); destination
  (talkgroup / receive group / private call to us / all call 0xFFFFFF passes everything); privacy indicator (bursts
  with PI set are only played when privacy is configured for that call).
- **Start of call**: a voice LC header with good CRC, or late entry (2.4). Do not announce the caller from the LC
  until it is known to be this call's (modems may hold the previous call's LC: see 2.4).
- **During voice**: every burst A-F is handed to the vocoder in order; a missing burst is replaced by silence; the
  embedded LC is re-checked each superframe (the talker may change between overs on a repeater without a header).
- **End of call**: terminator with LC (DMO only: through a repeater, the MS hears its own terminators echoed and
  must not end its next reception on them; use the timeout), or no valid burst on the slot for about 390 ms (13 slot
  ticks), or loss of slot timing.
- **Data**: CSBK, MBC, data headers and blocks on our slot are passed to the data service. Blocks are taken even with
  a CRC error (the packet CRC-32 decides); headers and CSBKs with a bad CRC are dropped.

### 2.3 Repeater mode vs direct mode timing

| | RMO | DMO (simplex, MS sync) | DMO (TDMA direct, DCDM) |
|---|---|---|---|
| Who sets the slot clock | the repeater (CACH, outbound bursts) | the first talker; the receiver follows its bursts | the first talker; both slots usable |
| Slot number known from | CACH TC | not known (only "the slot heard") | sync family (TS1 / TS2 sync) |
| Our TX slot | the channel's TS, aligned to the outbound | the slot phase we start in, or the one heard (reply aligned to the talker) | the channel's TS, aligned to the other slot's traffic if heard |
| Busy from | AT bit + decoded traffic | decoded traffic | decoded traffic on our slot |
| End of call | timeout (echoes) | terminator or timeout | terminator or timeout |
| Modem timing mode | passive (follow received) | active (own) when starting, passive when receiving | passive when the other slot is busy, otherwise active |

A single-frequency repeater (SFR, ERDM) user is DMO framed but must transmit in the slot opposite to the one it hears
from the repeater, aligned to it (see the SFR spec). The modem must therefore support "start
transmitting in the slot after the next one, in phase with the bursts being received".

### 2.4 Late entry

- A receiver that missed the header acquires the call from the embedded LC: needs bursts B-E of one superframe, so
  360-720 ms after the first burst heard.
- Some modems announce it (HR_C6000: post-access interrupt), others only report voice bursts with embedded LC.
  The stack must handle both: voice bursts with no preceding header start a call once the embedded LC is complete.
- **Stale LC**: the HR_C6000 (and likely the C7000) keeps the previous call's LC in its RX LC buffer until a new one is
  decoded. The stack must not attribute the new call to that LC: either wait two superframes (two bursts A) after the
  slot lock, or compare with the LC that was present when the reception began and only accept a different one, or
  accept after more than one superframe. This is the driver's job, done once: it knows its chip's buffer, and a
  voice burst's embedded LC marked valid is this reception's (6.1 rule 4). The HR_C6000 driver captures the buffer when
  a reception starts and accepts an unchanged LC after more than one superframe of voice; the HR_C7000 hands over
  only LCs its late-LC interrupt reports with a good checksum. The stack must not add its own stale check on top
  (for example against the previous call's LC): the same talker calling again then costs a superframe more for
  each layer (HIL 2026-10-10: a late entry joined at burst 9 of 36 played 7 bursts instead of 13).
- Skip the first two bursts A of a late entry before playing audio, unless the source is already known and isn't us.
- A lone voice burst (noise, a stray) must not start a call: require a B 60 ms after an A (SFR practice). An LC that
  comes with the first burst is held with that burst until the next burst makes the pair, then both are played. The
  HR_C7000 reports voice only once it has the LC, so its first burst carries it (HIL 2026-10-15: dropping that LC cost a
  superframe, 13 bursts played instead of 18).
- A PI header missed => privacy parameters come from the embedded / F-burst privacy info (risk for privacy schemes).

### 2.5 Transmitting voice

1. **Channel access** per the policy (1.7). Busy => wait (with timeout) or refuse.
2. **RMO, repeater not heard**: BS activation (2.7).
3. **Alignment**: RMO: wait until the slot tracking is stable, then transmit in the slot after the next one that
   carries our TS. DMO: start on the modem's own timing; if a call is being heard (a reply), align to it.
4. **Headers**: three voice LC headers on our slot (one per 60 ms: 180 ms of lead-in). Optionally a PI header for
   privacy schemes that need one.
5. **Voice**: superframes A-F; burst A has the voice sync; B-E carry the embedded LC (or talker alias / GPS embedded
   data in alternating superframes); F carries null embedded or privacy data. If the encoder has no frame ready,
   send AMBE silence (B9 E8 81 52 61 73 00 2A 6B per 20 ms frame).
6. **End**: PTT release takes effect at the next superframe boundary; send the terminator with LC on our slot.
7. **Hang**: after transmitting, hold the slot on our TS (capture timeout) so the reply is heard on it.
8. **TX timeout timer** (user setting) forces the end.

### 2.6 Transmitting data and CSBK

- A list of bursts (CSBK preambles, data header, blocks; or single CSBKs) goes out one per frame on our slot. No
  voice header, no terminator. Same channel access and BS activation as voice. Up to 40 bursts per transmission in
  current practice; a timeout of 10 s + 60 ms per burst covers a wake-up.
- The data service needs a "send this payload verbatim, I supplied the CRC" option (raw passthrough of CSBK / data
  headers from a host). On modems that always compute the CSBK / data header CRC (HR_C7000, HR_C5000), raw
  passthrough of a burst with an intentionally wrong CRC is not possible; the stack must then give the payload without
  its CRC and let the modem add it.

### 2.7 BS activation (repeater wake-up)

- Send a BS_Dwn_Act CSBK (CSBKO 0x38, LB=1, FID 0, BS address 0 = any, source = own ID). Octets: B8 00 00 00 00 00 00
  + source (3). HR_C7000 practice: in two consecutive slots with the PA keyed; HR_C6000 practice: one burst.
- Then listen: the repeater is up when its outbound is heard with the slot number toggling correctly more than 3 times
  in a row.
- No answer within 600 ms: send again, up to the configured number of attempts; then report failure to the user.
- A short PTT press still runs the whole wake-up (the user only starts the call; other radios do the same). On release the
  stack keeps listening and retrying. If the repeater answers after release, nothing is sent and the call view's
  `wake_admits` counter goes up, so the radio plays its talk permit tone. If no repeater is heard in the last attempt, the
  call ends with `DMR_TX_ERR_NO_REPEATER` and the radio plays its deny tone.

### 2.8 Hang time and TX hold-off

- Repeater call hang time: the repeater keeps the outbound up after a call; an MS that keys in it does not need BS
  activation and should reply on the same slot (and talkgroup, if group hang applies).
- After hearing a call on our slot, a polite MS waits for the terminator / timeout before keying; an impolite one keys
  at the next aligned slot.
- After our own transmission, the receiver keeps the slot lock and the call's destination for the capture timeout.

---

## 3. Privacy hooks

- **Basic privacy** (Motorola-style, Anytone 16-bit): the voice LC carries the privacy bit (service options bit 6)
  and a manufacturer FID; the AMBE bits of each burst are XORed with a key-derived pattern that depends on the burst
  position (A-F). The modem must give the stack the raw 27-byte AMBE before decoding and take raw AMBE for
  transmission, with the burst position. The EMB PI bit is set on voice bursts of an encrypted call.
- **Receivers without the key**: drop bursts with PI set (do not play them).
- **Enhanced / AES schemes**: need a PI header (DT 0) before the voice and the per-superframe privacy information in
  burst F. The modem must let the stack send a PI header burst and set the burst F embedded content, or at least
  pass them through. Late entry needs the IV from burst F (not supported by the HR_C6000 path today).
- The HR_C6000 has its own "end-to-end encryption" interrupts (TX-start sub-status bit 5); not used.

---

## 4. Talker alias and embedded data

- TX: in alternate superframes, replace the embedded LC with one talker alias block (the header, FLCO 4, carries the
  format and length and the first 6 characters; blocks 1-3, FLCO 5-7, carry 7 characters each) or a GPS embedded LC
  (FLCO 8, 25-bit longitude scaled 2^25/360, 24-bit latitude scaled 2^24/180). The modem must accept a new embedded LC per superframe.
- RX: the modem reports each assembled embedded LC (FLCO 0, 3, 4-8) once per superframe; the stack assembles the
  alias and decodes the GPS.

---

## 5. What the modems provide

| | HR_C6000 (DM-1701, MD-UV380, MD-9600, GD-77) | HR_C7000 (DM-32) | HR_C5000 (MD-380) |
|---|---|---|---|
| Host interface | external chip; config SPI (page/register) + voice SPI + I2S | on-chip peripheral of the CK803S SoC, memory-mapped 32-bit registers | external; like the C6000 but config page 0 |
| Interrupts | timeslot, system, RF TX (RF RX not routed on the STM32 radios) | system (vector 61), TX timeslot (62), RX timeslot (63), RF TX (64) | timeslot, system, RF |
| RF TX lead | not measured | 3.1 ms before the burst with its RF control register at 0x41F1A (1.0 ms with 0x40A0A) | |
| PA off | MCU, at the next timeslot interrupt (>20 ms after on) or a 29 ms one-shot | one-shot timer, 28 ms default (user setting) | 30 ms timer |
| Burst status | one byte: data type / voice A-F (1-6 + voice bit), sync class, CRC flag, PI; separate CC/CACH and sync type registers | one 32-bit word: data type bits 11-8, voice type bits 14-12, sync class bits 1-0, error flags bits 30-24 | similar to the C6000 |
| Reports per voice burst | two (EMB, then burst) | the manual's EMB report exists too | |
| CRC of CSBK / data header | chip or MCU (CRC_MCU_Control) | chip only | chip only |
| Vocoder | AMBE on the MCU (binary blob), PCM over I2S | AMBE blob run in place from flash, tables in modem RAM | AMBE on the MCU |
| Timing modes | passive / active per register; "send next slot" / "receive next slot" / "resync" per slot | same model (LAYER2 control / TX-RX control / slot-on registers) | same model |
| RX restart | TX+RX enable passive, then sync-fail, clear, sync-fail, receive-next | slot-on off, sync-fail, receive-next, slot-on (RX+TX) | |

Common model across all three: **the host decides, at each slot boundary, what the slot after next does** (send a
burst of a given type, receive, or nothing), and the chip frames, codes and times it. That is the model the HAL keeps.

---

## 6. The `dmr_modem` HAL contract (proposal)

The driver owns the chip, its interrupts and the per-slot register work. The DMR stack owns calls, filters, policy and
the data service. They meet through queues and events so that the stack runs at task level, never inside the slot
interrupt.

### 6.1 Principles

1. **The driver keeps the slot clock.** It numbers slots with a free-running counter and, when known, the TDMA slot
   (TS1/TS2) and whether we are in step with a received signal.
2. **Lookahead TX queue.** The stack queues bursts ahead of time, each tagged with its target (our slot / a given slot
   number). The driver pops one at each of our slots and sets it up from its own interrupt. Deadline for the stack: a
   burst for slot N must be queued before the boundary of slot N-1 (30-60 ms ahead). An empty queue in the middle of a
   voice call means "send AMBE silence"; in data, "end of list".
3. **RX bursts are copied out by the driver** in the interrupt (the modem buffers are overwritten every slot) and
   delivered as events; the stack has no hard deadline beyond the queue depth (at least 8 bursts).
4. **The driver de-duplicates** modem quirks: one report per burst (drops the C6000's EMB double report), voice burst
   index 0-5, CRC flag normalized, stale LC flagged.
5. **"Transmitting" is the driver's slot state**, from the first burst set up to the end of the last burst's slot,
   never the PA state (the PA switches per burst).

### 6.2 Events (driver to stack)

| Event | Contents | Timing |
|---|---|---|
| `slot_tick` | slot counter, TS if known (from CACH TC or sync family), what this slot is (ours TX / RX / none), CACH AT (busy) if heard | every 30 ms; delivered within 2 ms of the boundary; informative (the stack must not need to answer it in time) |
| `burst_rx` | kind (voice A-F index / data type / LC-only), sync family (MS/BS/TS1/TS2), color code, CRC ok, PI, TS if known, in-step flag (report 2-10 ms after the boundary), RSSI if available, payload (LC 9+3 bytes, data 12/18/24 bytes, voice 27 bytes + the embedded LC if complete) | within 10 ms after the burst's slot ends |
| `call_acquired` | late entry announced by the modem (optional; the stack must also work without it) | |
| `voice_lost` | voice sync lost on a slot (the next voice frame should be silence) | |
| `tx_slot_started` / `tx_slot_ended` | per transmitted burst (RF keyed / unkeyed), for diagnostics and RF switching on boards where the driver doesn't own it | at the RF TX interrupt / PA off |
| `tx_rejected` | the modem refused to send (channel busy under a polite policy) | |
| `tx_done` | the last queued burst's slot has ended (terminator included); the driver is back in receive | after the last burst's slot, not before |
| `sync_lost` | no slot tick for 200 ms while not idle, or the timing was found out of step; the driver has restarted the receiver | |

### 6.3 Commands (stack to driver)

| Command | Effect |
|---|---|
| `configure` | own DMR ID, color code (or "any"), sync families to detect, default access policy (impolite / polite to CC / polite to all), DMO / RMO framing |
| `rx_start` / `rx_restart` | receive every slot, passive timing; restart = drop sync and search again |
| `rx_lock_slot(slot or none)` | receive only our slot / both |
| `tx_begin(timing)` | start a transmission: own timing (DMO), aligned to the received signal on the next slot carrying a given TS (RMO, SFR user), or as soon as possible; with the access policy for this transmission |
| `tx_queue_burst` | one burst: type (voice header, voice A-F with 27-byte AMBE, terminator, PI header, CSBK, data header / block, idle, raw DT), payload, "host supplied CRC" flag, embedded LC for B-E (voice), sync family override (SFR outbound) |
| `tx_end` | no more bursts: finish the queued ones (a voice call ends at the superframe boundary with its terminator, which the stack has queued), then `tx_done` |
| `tx_abort` | stop at the next slot (emergency, timeout) |
| `set_embedded_lc` | the LC (or talker alias / GPS embedded LC) the modem spreads over B-E from the next superframe |
| `set_cc_filter(cc or any)` | color code deframing |
| `is_transmitting` | slot-state based (6.1 point 5) |
| `set_audio_path` | vocoder PCM in / out routing and gains (where the modem owns the codec) |
| `power(on/off)` | chip power down / up with its init sequence |

### 6.4 Timing requirements on a driver

- Slot ticks every 30.000 ms +- 1.25 ms when following a signal (the chip's real-time sync correction window), with no
  missed tick while receiving or sending.
- Bursts set up from the driver's own interrupt for the slot after next (no stack round trip).
- RF keyed before the burst (C7000: 3.1 ms lead) and unkeyed after it (within the 2.5 ms guard time); burst length on
  air about 27.5-31 ms with sub-millisecond ramps.
- A `burst_rx` event for every burst heard on a slot being received, within 10 ms of its slot.
- `tx_done` only once the final burst (terminator, last data block) has left the antenna.
- Recovery: a stalled modem (no tick for 200 ms) re-initialized by the driver, reported as `sync_lost`.

### 6.5 What the stack must not assume

- That the modem tells it about late entry (it may only see voice bursts).
- That the LC buffer is fresh at the start of a call.
- That reports about received bursts are reliable while it transmits (HR_C6000: CRC, CC, sync type and header info
  bits are corrupt while sending).
- That the PA state means "transmitting".
- That the modem can send a CSBK or data header with a wrong CRC (C7000, C5000).
- That voice burst numbering is 0-5 or 1-6 (the driver normalizes to 0-5).

---

## 7. Gaps and uncertainties

- The HR_C7000 facts are from a build checked against disassembly of other firmware; nothing is on air yet.
- RF TX interrupt lead on the HR_C6000 has not been measured.
- Whether the C7000 has the double voice report and the stale LC behavior (assumed, as it derives from the C6000).
- ETSI hang time and wake-up retry timings are configuration in practice; the 600 ms retry is the TYT firmware's.
- Enhanced privacy (PI header, burst F IV) is not exercised by any of our drivers.
