<!-- SPDX-License-Identifier: MIT -->

# Single frequency repeater (SFR)

A single frequency repeater receives a DMR call on one TDMA slot of a frequency (the inbound slot) and repeats it on
the other slot of the same frequency (the outbound slot), 90 ms later. Radios in direct mode hear both copies. Switch:
`CONFIG_SFR`, on with DMR except on the MD-380, whose HR_C5000 has no CRC_MCU_Control for the idle bursts of section
3 item 4 (gotchas 14.6; `docs/build-config.md`). Background and the HR_C6000 findings: `docs/spec/gotchas.md` 8.11,
8.12 and 14; the host protocol: `docs/spec/mmdvm-protocol.md` 6.

There are two kinds:

| Name | What | State |
|---|---|---|
| **MMDVM SFR** | The hotspot repeats; an MMDVM host turns it on and links it to a network (section 2) | Built, passes on air |
| **Standalone SFR** | A channel set up for SFR in the codeplug, no host: the repeater roles (Rpt, Rpt Moto) and the user roles (User, ERDM) | Not built (section 4) |

## 1. Layers

| Layer | File | What it does |
|---|---|---|
| Logic | `core/dmr/sfr.c` | roles, channel settings, framing, the repeat queue and its rules (`sfrEngine_t`: RF priority, TOT, data hold, hang), voice dedup, late entry, the uplink rules, checksums, version suffix, the MMDVM trigger. No hardware |
| Runner | `core/dmr/sfrRunner.c` | the repeater roles on any modem behind `hal_dmr_modem_t`: received bursts filtered and fed to the engine, the outbound slot kept fed, an optional uplink to a host |
| MMDVM SFR | `core/hotspot/hotspotSfr.c` | the MMDVM extension (version suffix, status flag 0x80, the space bytes, 108 extra status bytes, command 0xD5) and `hotspot.c`'s other mode (`hotspot_alt_t`) |

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
- **Debug link** (section `hotspot`): `sfr.on sfr.keyed sfr.busy sfr.slots sfr.rx sfr.repeated sfr.ignored
  sfr.echo sfr.bad_crc sfr.tx_bursts sfr.tx_idle sfr.tx_starts sfr.up sfr.up_lost sfr.tx_beacons sfr.tx_gaps`.

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
| HR_C6000 (`drivers/modem/hrc6000/hrc6000_dmr.c`, owned by the DMR modem work) | DM-1701 (the MD-380 builds without SFR) | all: `sync_override`, idle bursts, `host_crc`, `DMR_TIMING_ALIGNED` (to the slot numbering it learned from the bursts heard), `tx_listen`, `DMR_BURST_EMPTY`, the TX slot events; reports keep coming while it transmits |
| HR_C7000 (`drivers/modem/hrc7000/hrc7000_dmr.c`) | DM-32UV | `sync_override` and the TX slot events; the rest not yet |

The board switches the RF per burst: the RF TX interrupt keys the PA, the timeslot interrupt that starts the next slot
drops it (on the DM-1701 the AT1846S goes back to RX about 1.3 ms into the slot, gotchas 3.4). The rest of this
section is the HR_C6000 bring-up, measured on the DM-1701. Item 1 was missing in the chip: the slot between our bursts was written
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

## 4. Not built yet

- Standalone SFR: the repeater roles (Rpt, Rpt Moto) and the user roles (User, ERDM) on a channel: they need the
  codeplug's SFR bytes (`sfrChannelDecode`), a radio core mode that hands the modem to the runner (repeater) and
  call stack changes for the users (transmit on the inbound slot aligned to the repeater, hear both copies with
  `sfrDedupVoice`, the ERDM outbound color code). The call stack is `core/dmr/call/dmr_call.c`.
- The HR_C6000 driver level parts: raw RX RAM classification (`sfrRawClassify`), the register switches of command
  0xD5.

A two-frequency repeater made of two radios is the split duplex extension (`split-duplex.md`, `CONFIG_HOTSPOT_SPLIT`),
not the SFR: it registers in mmdvm.c and hotspot.c ahead of the SFR (a priority extension), so while a split role
is active the SFR stays off, and without a role the SFR works exactly as described here.

## 5. Tests

- Host: `tests/host/sfrTest.c` (the logic), `tests/host/hotspotSfrTest.c` (MMDVM SFR through mmdvm.c and
  hotspot.c against the simulated modem: trigger and suffix, status bytes, an RF call repeated and sent up, a network
  stream, RF priority, echo, late entry, DMR_LOST1, command 0xD5, DMR_ABORT, back to simplex).
- HIL: `tests/hil/test_hotspot_sfr.py` (MMDVM SFR on and off through MMDVM on the radio's channel, no traffic sent;
  marked `radio_tx` because a call heard would be repeated). On-air repeat tests need the driver work of section 3
  and the SDR bench (`tools/bench/mmdvm_hotspot.py --sfr`, `tools/bench/sfr_call.sh`).
