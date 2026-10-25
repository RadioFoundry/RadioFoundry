<!-- SPDX-License-Identifier: MIT -->

# Single frequency repeater (SFR)

A single frequency repeater receives a DMR call on one TDMA slot of a frequency (the inbound slot) and repeats it on
the other slot of the same frequency (the outbound slot), 90 ms later. Radios in direct mode hear both copies. Switch:
`CONFIG_SFR`, on with DMR except where a board lists it as unsupported (the HR_C5000 has no CRC_MCU_Control for the idle bursts of section 3 item 4, gotchas 14.6) or not written (see `docs/build-config.md`). Background and the HR_C6000 findings: `docs/spec/gotchas.md` 8.11,
8.12 and 14; the host protocol: `docs/spec/mmdvm-protocol.md` 6.

There are two kinds:

| Name | What | State |
|---|---|---|
| **MMDVM SFR** | The hotspot repeats; an MMDVM host turns it on and links it to a network (section 2) | Built, passes on air |
| **Standalone SFR** | A channel set up for SFR in the codeplug, no host: the repeater roles (Rpt, Rpt Moto) and the user roles (User, ERDM) | Experimental: built, host tested, on the SDR bench; not reliable end to end (section 6) |

## 1. Layers

| Layer | File | What it does |
|---|---|---|
| Logic | `core/dmr/sfr.c` | roles, channel settings, framing, the repeat queue and its rules (`sfrEngine_t`: RF priority, TOT, data hold, hang), voice dedup, late entry, the uplink rules, checksums, version suffix, the MMDVM trigger. No hardware |
| Runner | `core/dmr/sfrRunner.c` | the repeater roles on any modem behind `hal_dmr_modem_t`: received bursts filtered and fed to the engine, the outbound slot kept fed, an optional uplink to a host |
| MMDVM SFR | `core/hotspot/hotspotSfr.c` | the MMDVM extension (version suffix, status flag 0x80, the space bytes, 108 extra status bytes, command 0xD5) and `hotspot.c`'s other mode (`hotspot_alt_t`) |
| Standalone SFR | `core/radio/radio_sfr.c`, `core/radio/radio_sm.c`, `core/dmr/call/dmr_call.c` | a channel's role from its codeplug bytes; the repeater roles run the runner instead of the call stack; the user roles are the call stack's SFR user mode |

The HR_C6000 glue in `drivers/modem/hrc6000/sfr_glue/` is not built; it documents what a driver
level SFR did (raw RX RAM reads, RF switching per burst) and holds the extra status bytes' layout.

## 2. What is built

- **MMDVM SFR.** The host turns it on with SET_CONFIG's simplex flag clear and SET_FREQ RX = TX (both non-zero),
  evaluated after either command; the simplex flag set, RX != TX, a retune or the session's end turns it off.
  - Uplink: every burst heard on the inbound slot goes up as DMR_DATA1 (inbound TS1) or DMR_DATA2, framed as the
    simplex uplink frames them (headers and terminators from their LC, voice with the EMB of its position and the
    call's embedded LC in B-E); a call heard without its header gets a header made from the embedded LC first, once
    voice F has given it. DMR_LOST1 / DMR_LOST2 300 ms after the last burst of a call without a terminator. Our own
    repeat never goes up.
  - Downlink: either slot's DMR_DATA command feeds the outbound slot (both space bytes carry its space, up to 16).
    Each frame is ACKed or NAKed: 6 RF busy (an inbound call is being repeated; the rest of a stream an RF call cut
    into), 5 full, 4 a burst type that can't be sent. Idle bursts and repeated terminators are ACKed and dropped.
    DMR_ABORT, DMR_START and DMR_SHORTLC are ACKed (the radio owns its keying and sends no CACH).
  - Defaults: TS1 in, TS2 out, TS framing (the ETSI direct mode sync of the outbound slot), 1 s hang, 180 s TOT, the
    color code from SET_CONFIG. `hotspot_sfr_set_settings()` takes a channel's settings once a UI or codeplug
    provides them; command 0xD5 overrides the framing at run time.
- **Runner behavior** (gotchas 14):
  - a burst the modem places on the outbound slot (`ts` = the outbound slot) is ignored: our echo or another
    repeater's;
  - own echo: a burst with the kind, position and payload of one we put on air in the last 250 ms is dropped (the
    HR_C6000 reports what it sent in empty inbound slots), except a voice header or terminator while an inbound call
    is open: a header run is identical copies (gotchas 14.10);
  - late entry: outside a voice call a lone voice A is held; a B 60 ms after it starts the call;
  - while keyed (or for a burst the modem reports out of step) the checksums are checked here, not taken from the
    chip: RS(12,9) of headers and terminators, CRC-CCITT of PI, CSBK, MBC and data headers. A header or terminator
    that fails and matches the call's LC in its first five bytes is rebuilt from the call's LC (`sfrLcBurstGuess`);
  - keyed aligned to the inbound call (`DMR_TIMING_ALIGNED`, the outbound TS) when one is being heard, else on its own
    slot clock (`DMR_TIMING_OWN`), always impolite (gotchas 1.8); two bursts kept queued in the modem, one engine
    decision per outbound slot; a gap inside an inbound call (a burst missed) is `DMR_BURST_EMPTY`: the slot stays
    empty and is received, never an idle burst, which corrupts the inbound burst decoded next to it (gotchas 14.4,
    `sfr.tx_gaps`); a burst heard while such placeholders wait in the modem takes the oldest one's slot (`tx_fill`),
    so it still goes out 90 ms later; idle bursts only in the hang and a network stream's gaps; `DMR_EV_TX_REJECTED`
    unkeys.
- **Idle beacon** (optional, off by default): with the radio setting `rpt_beacon` on, the runner keys its outbound
  with idle bursts for `roam_beacon_len_s` every `roam_beacon_s` while it carries nothing, so roaming radios can
  measure the site (`sfrRunnerSetBeacon`, `hotspot_sfr_set_beacon`; `dmr-roaming.md` 7). Traffic has priority. The
  phase is the runner's own (no network time).
- **Debug link** (section `sfr`, the channel's repeater role when it runs, else the hotspot's mode, `sfr.mode` =
  `channel`, `hotspot` or `off`; `sfr.evq_drops` the role's modem events lost): `sfr.on sfr.keyed sfr.busy sfr.slots sfr.rx sfr.repeated sfr.ignored
  sfr.echo sfr.bad_crc sfr.tx_bursts sfr.tx_idle sfr.tx_starts sfr.up sfr.up_lost sfr.tx_beacons sfr.tx_gaps
  sfr.ignored_slot sfr.ignored_cc sfr.last_ignored` (why bursts were ignored: reported on the outbound slot, another
  color code; the last one's color code and data type or voice index in hex), `sfr.rx_fec` (frames/bits/lost: the
  repeated voice's AMBE frames as heard, the C0/C1 Golay bits corrected in them and the frames whose C0 failed; the
  bits go out as heard, so an SDR seeing more errors in the repeat than this means the transmit side adds them),
  `sfr.rx_fec_pos` (those bits by the frame's place in the burst: errors piling up in the first frame point at the
  receiver still settling at the slot's start).

## 3. What the modem driver must do

The runner assumes these of `hal_dmr_modem_t`; they are the open work for each radio's driver:

1. Keep receiving, and report bursts, while it transmits: the inbound slot is heard between our bursts. The runner
   asks for it with `tx_listen()` after every `tx_begin` (a plain call never does). Report `ts` when it knows the slot,
   and `in_step` false for a burst whose status it can't trust.
2. `tx_begin(DMR_TIMING_ALIGNED, ts)`: send in the slot opposite to the bursts being received, in step with them
   (the repeat goes out 90 ms after the burst, gotchas 14.3). `DMR_TIMING_OWN` with a ts: its own slot clock.
3. `dmr_tx_burst_t.sync_override`: the outbound sync (MS, BS, or TS1 / TS2 direct mode sync) on every burst.
4. `DMR_BURST_IDLE` with the payload given (the ETSI idle message) and no CRC of its own (gotchas 14.4); data bursts
   with `host_crc` sent as they are; `DMR_BURST_EMPTY`: nothing sent in that slot (received with `tx_listen`).
5. `DMR_EV_TX_SLOT_STARTED` once per burst taken from its queue, `DMR_EV_TX_DONE` after `tx_end` has drained it.
6. An SFR channel never switches to repeater (RMO) mode on BS sync (gotchas 14.7).

Status per modem driver:

| Driver | Radios | Items |
|---|---|---|
| HR_C6000 (`drivers/modem/hrc6000/hrc6000_dmr.c`, owned by the DMR modem work) | STM32 boards with an HR_C6000 (an HR_C5000 board builds without SFR) | all: `sync_override`, idle bursts, `host_crc`, `DMR_TIMING_ALIGNED` (to the slot numbering it learned from the bursts heard), `tx_listen`, `DMR_BURST_EMPTY`, the TX slot events; reports keep coming while it transmits |
| HR_C7000 (`drivers/modem/hrc7000/hrc7000_dmr.c`) | HR_C7000 boards | all: `sync_override`, `DMR_TIMING_ALIGNED` (the TX slot built from the receive slot, ours picked by slot number), `tx_listen` (with the voice-access flag during a call), `DMR_BURST_EMPTY`, `tx_fill`, the TX slot events; headers and terminators heard while sending are passed on with the chip's CRC verdict. On air (gotchas 14.11) |

The board switches the RF per burst: the RF TX interrupt keys the PA, the timeslot interrupt that starts the next slot
drops it (the AT1846S goes back to RX about 1.3 ms into the slot, gotchas 3.4). The rest of this
section is the HR_C6000 bring-up, measured on an AT1846S board. Item 1 was missing in the chip: the slot between our bursts was written
"no receive" (0x41 = 0x00), so nothing was heard there and MMDVM SFR repeated only what it heard before
keying (HIL 2026-10-10: mode switching and the first repeat on TS2 passed, the rest of the call failed). With
`tx_listen` (`hrc6000_dmr_tx_listen`) the engine writes "receive" (0x50) for that slot instead, for that transmission
only; `hrc6000DmrTest` test_tx_listen checks it. Two more things were needed on air (gotchas 14.9): the per-burst
switch back to RX goes receiver off, RX frequency, RX supply, RX on (it went from DMR TX straight to RX on), and a gap
in the inbound call leaves our slot empty (`DMR_BURST_EMPTY`) instead of an idle burst. On air (HIL 2026-10-12,
06c5bc6): the bench's 3-superframe call goes up whole (18 voice frames and the terminator) and is repeated whole on
TS2 (17-18 of 18 voice bursts, the terminator, then the hang's idle bursts). The voice repeat came 210 ms after the
inbound burst, not 90 ms, and only 2 of the bench's 8 headers were repeated: the echo rule dropped the identical
headers, and the gap left two placeholders queued ahead of the voice (gotchas 14.10). Both fixed in the runner
(host tested), to be confirmed on air.

## 4. Standalone SFR

A channel's role comes from its codeplug bytes (`sfrChannelDecode`, `docs/spec/open-firmware-cps.md` 7.3.3: flag 1
bit 1, SFR bytes A and B; the CPS keeps them as read, the Codeplug Editor sets them). `codeplug.c` hands them to the
core as read (`channel_t.sfr_flag1 / sfr_a / sfr_b`); `radio_sfr_channel()` turns them into a role and an
`sfrConfig_t`. A role needs one frequency: RX = TX (a user may have no TX frequency). On a two-frequency channel, or
in the VFO, the bytes are ignored and the channel is an ordinary one.

| Role | Behavior |
|---|---|
| Rpt, Rpt Moto | The radio core starts the runner (section 2) on the modem instead of the call stack: DMO framing, every slot received, the inbound color code, no uplink. The keying stays armed on the frequency for the modem's per-burst keying, as in hotspot mode. The modem events reach the radio task through a queue (`radio_sfr_poll` every tick). PTT is refused (`TXDENY_SFR_RPT`). The screen shows receiving while an inbound call is repeated, the red LED while the outbound slot is sent. The settings' idle beacon (`rpt_beacon`) applies. With TX inhibit set, a critical battery or a frequency outside the TX bands the channel is an ordinary one, and it goes back to one if that happens while it runs. Once the radio may transmit again, the role starts by itself between calls (`sfr_denied`), with no channel change |
| User, ERDM | The call stack's SFR user mode (`dmr_call_config_t.sfr_user`). TX on the inbound slot (it replaces the channel's timeslot), every burst with the sync of `radio_sfr_user()`: TS1 / TS2 direct mode sync with TS framing, MS sync with MS or BS framing. Keyed in step with the repeater's slots (`DMR_TIMING_ALIGNED`) when a burst whose sync named its slot (TS1, TS2, BS) was heard in the last `DMR_CALL_ALIGN_MS`, else on its own timing (the repeater aligns to us); always impolite at the modem (the repeater's hang). RX by the setting `sfr_rx` (`dmr_call_config_t.sfr_rx`, table below). The repeat is the burst with `rx_sync`: the outbound slot's TS sync (TS framing) or BS sync (BS framing); the talker's copy has the inbound sync or comes on the inbound slot. With MS framing the copies look alike, and the call locks to the first slot heard. ERDM: the outbound color code is heard too (the modem passes every color code, the stack filters) |

| `sfr_rx` | What the user plays |
|---|---|
| 0 Repeater (default) | The repeat only. The talker's copy is dropped (`dmr_call` stat `rx_sfr_direct`) |
| 1 First copy | Both copies; each voice burst once, whichever comes first (`sfrDedupVoice`, stat `rx_sfr_dup`). The second copy of a header doesn't restart the burst sequence |
| 2 Dual-path RX (`CONFIG_SFR_DUALPATH_RX`) | As first copy, but a voice burst with a frame whose C0 Golay check fails (`ambeFecDecode`) is held up to `DMR_SFR_DUALPATH_MS` (120 ms) for the other copy, and each failed frame is taken from it when its own check passes (stats `rx_sfr_held`, `rx_sfr_fixed`). A pass/fail check per frame, nothing ranked. No other copy in time: the burst plays as it came. Encrypted voice is never held |

The setting is stored in bits 1-2 of the settings record's `rpt_beacon` byte (`settings-store.md`), set in
Settings > Talk & listen > SFR receive and by debug link setting 30.

CW ID (`CONFIG_SFR_CWID`, `core/radio/radio_cwid.c`): the repeater role can identify itself in Morse. The setting
`sfr_cwid` picks the interval: 0 off (default), 1 5 min, 2 10 min, 3 15 min, 4 30 min. It is stored in bits 3-5 of
`rpt_beacon`, set in Settings > Talk & listen > SFR CW ID and by debug link setting 32.

| Rule | Detail |
|---|---|
| When | The repeater has transmitted since the last ID (a repeat or its beacon), has been quiet `RADIO_CWID_QUIET_MS` (2 s), and the interval has passed since the last ID. The first ID comes as soon as the first traffic ends. No traffic, no ID |
| What | The callsign: the radio name's first word, up to a space, `-` (an SSID) or `/`, in upper case, and only if it clearly is one: 4 to 7 letters and digits with at least one of each (`radio_cwid_callsign`). Otherwise (a personal name, an empty name) nothing is keyed and the ID counts as skipped |
| How | The runner stops (`radio_sfr_stop`), the board goes to FM and keys a carrier on the channel's frequency at the channel's power, the TX tone is keyed in Morse with the settings' speed and tone (`cw_wpm`, `cw_tone_hz`; 20 wpm, 800 Hz by default) after a 150 ms lead and before a 150 ms tail. Then DMR mode again, the receiver back on the frequency, and the role starts again with its schedule kept. This is the hotspot's CW keyer (`radio_sm.c`) |
| Meanwhile | The role stays active: PTT is still refused, calls aren't repeated, the Home screen says "Sending ID" |
| Cut | A channel change or hotspot mode during the ID ends it at once; it counts as cut |

On air the ID came 2 s after the hang, at 20 wpm (60 ms elements), 800 Hz, about 1.8 kHz deviation, with the 150 ms lead and
tail, and the next one an interval later after new traffic. The runner's counters carry on over the ID (`radio_sfr_start`
`keep_stats`). Bench figures: the HR_C7000 repeater's hardware page.

Debug link view `dmr`: `dmr.sfr_cwid` (the setting), `dmr.sfr_cw` (keying now), `dmr.sfr_cwid_sent`,
`dmr.sfr_cwid_cut` (cut or skipped).

Why a user may want both slots: the inbound copy comes straight from a talker in range, the repeat from one out of range.
A user whose inbound slot is TS2 has TS1 as its outbound slot, so two users on the same frequency with different
inbound slots still hear each other: they are users of two different repeater set-ups, which can't share one frequency.

No CACH: the repeater keys only its own slot (it listens on the other one), so there is no continuous BS outbound to
carry it. Users find the slots from the outbound slot's sync (TS framing) and the hang's idle bursts.

On air (SDR bench, 2026-10-19), the numbers on the radios' own hardware pages:

- The user's call goes out on slot A with the TS1 direct mode sync; the repeater repeats it on slot B with the TS2
  sync, every voice burst 90.2 ms after the original (72 of 72), then the hang's idle bursts. The repeat carried 2 of
  the 3 headers and no terminator (the users' fade time ends the call), and its voice frames needed Golay
  corrections where the direct copy was clean: an HR_C7000 repeater's receiver takes bit errors while it switches per
  burst (gotchas 14.13). Both open.
- An HR_C6000 user played a bench call (TS1 sync) whole, no frame lost. It never reported the repeated copy during
  the call (`rx_sfr_dup` 0): the chip decodes the voice of the slot it locked onto, although the driver writes
  "receive" for every slot (`rx_lock_slot(0)`); its reports for the other slot are stale status (gotchas 14.12). With
  `sfr_rx` 1 or 2 it falls over to the other copy only between calls or after a loss. A user out of range of the
  talker locks to the repeat; one whose talker fades mid-call hears a gap until the receiver's lost-signal restart.
  Hearing both slots during a call needs the raw RX RAM path (section 3).
- An HR_C7000 user reports both copies. Its driver gives the voice-access flag to every chip slot with a call on it;
  with the flag on one slot only, every burst read the voice RAM of the last one decoded and the user played the same
  burst for the whole call (gotchas 14.15). Fixed in the driver, not yet rechecked on air.

Not built: the HR_C6000 driver level parts (raw RX RAM classification, `sfrRawClassify`; the register switches of
command 0xD5); a setting on the radio for the role (the Codeplug Editor sets it). On the radio, Tools > Single-freq repeater shows the
channel's role, slots and the repeater's state, and a repeater channel's Home screen says "SFR repeater" or "Repeating"
where a DMR channel shows its talk target.

A two-frequency repeater made of two radios is the split duplex extension (`split-duplex.md`, `CONFIG_HOTSPOT_SPLIT`),
not the SFR: it registers in mmdvm.c and hotspot.c ahead of the SFR (a priority extension), so while a split role
is active the SFR stays off, and without a role the SFR works exactly as described here.

## 5. Tests

- Host: `tests/host/sfrTest.c` (the logic), `tests/host/hotspotSfrTest.c` (MMDVM SFR through mmdvm.c and
  hotspot.c against the simulated modem: trigger and suffix, status bytes, an RF call repeated and sent up, a network
  stream, RF priority, echo, late entry, DMR_LOST1, command 0xD5, DMR_ABORT, back to simplex),
  `tests/host/dmrCallTest.c` test_sfr_user (both copies played once, the talker fading out, TX slot, sync and timing,
  ERDM color code), `tests/host/radioCoreTest.c` test_sfr_roles (the repeater role from the codeplug bytes repeats a
  call on TS2, PTT refused, TX inhibit, two-frequency channel refused; the user role's TX slot and sync), test_sfr_cwid (the ID schedule and callsign, an ID
  after a call in FM on the channel's frequency, the role back after it, a retune cutting it, no callsign);
  `tests/host/roamTest.c` (the `sfr_cwid` bits stored and clamped).
- HIL: `tests/hil/test_hotspot_sfr.py` (MMDVM SFR on and off through MMDVM on the radio's channel, no traffic sent;
  marked `radio_tx` because a call heard would be repeated). On-air repeat tests need the driver work of section 3
  and the SDR bench (`tools/bench/mmdvm_hotspot.py --sfr`, `tools/bench/sfr_call.sh`).
  `tests/hil/test_sfr_standalone.py` runs the standalone roles from codeplug channels: the repeater's inbound quality
  (`sfr.rx_fec`, `sfr.rx_fec_pos`), TX inhibit, the CW ID and the user's receive modes against the bench talker, and
  `test_sfr_chain_rfoundry_user`, a whole chain of RadioFoundry radios (talker, repeater, listener) judged by what the
  listener played (`voice.rx_in`, `voice.rx_peak`, `voice.rx_repeats`).

## 6. Known issues by radio

Standalone SFR is experimental. This section names radios on purpose (an owner-approved exception to the generic-docs
rule, `tools/generic_allow.txt`); keep each radio's details on its hardware page. Unless noted: SDR bench, 461.1375
MHz, CC 2, the bench's MMDVM talker sending a 4-superframe group call (72 voice frames). "Bits" are AMBE bits the
receiving radio's Golay decoders corrected (`sfr.rx_fec`, by frame place `sfr.rx_fec_pos`).

| Radio | Role | What fails | How often | Evidence | Workaround |
|---|---|---|---|---|---|
| Any | Whole chain (user, repeater, user) | A call through a RadioFoundry repeater between RadioFoundry users doesn't come through clean | Hand test 2026-10-20 (DM-1701 user, DM-1801 Pro repeater, DM-32UV user, live voice): about 1 clean call in 30. SDR bench, same radios, debug-link PTT: 9 of 11 calls played whole | PR hand test; `test_sfr_chain_rfoundry_user` | None. MMDVM SFR (a host) is not affected |
| DM-1701 | Repeater | Inbound bit errors sit in the first frame of each burst, the one right after the radio's own outbound burst, and the repeat carries them on | Every call: 54-66 bits per 72 frames, 48-54 of them in the first frame, no frame lost (3 calls, 2026-10-25) | Switching back to RX earlier trades them against our own burst: 28.0 ms after the key, inbound 17-39 bits but the repeat's last frame took 45-61 bits on the SDR (the burst cut short); 28.8 ms, inbound 104-143 bits. The timeslot interrupt (30.25 ms) stays. Dropping only the PA earlier did nothing (`hardware-dm1701.md` 6) | None needed to play: the errors are corrected. A weak inbound signal adds to them |
| DM-1701 | User | Hears one copy of a call at a time, so First copy and Dual-path RX only switch copies between calls | Always | Gotchas 14.12 | Receive mode Repeater (the default) |
| DM-1801 Pro | Repeater | The repeat carries the inbound bit errors on (about 1.3 bits a frame) | Every call | Gotchas 14.13; SDR bench rerun of the chain, 2026-10-24 | None; it plays |
| DM-1801 Pro, DM-32UV | User | The listener played the call's first decoded burst over and over (stale voice RAM: the voice-access flag on the wrong chip slot) | Hand test 2026-10-20: most calls | Gotchas 14.15; fixed in the driver, `hrc7000DmrTest` test_rx_sfr_both_copies; on-air recheck: below | Receive mode Repeater |
| DM-32UV | Repeater | Repeat quality falls with a weak, fading inbound path | One site: 39 of 72 frames repeated | `hardware-dm32.md` 8.9 | Put the repeater where it hears the users well |
| DB25-D PRO | Any | Not built (`CONFIG_SFR=n`): same-frequency TX needs the BK4819 without its per-burst VCO calibration | - | `boards/db25d/config.mk` | None |
| MD-380 | Any | Not available: the HR_C5000 lacks what the SFR uses (section 3) | - | Gotchas 14.6 | None |
