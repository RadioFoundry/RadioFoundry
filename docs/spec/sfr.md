<!-- SPDX-License-Identifier: MIT -->

# Single frequency repeater (SFR)

A single frequency repeater receives a DMR call on one TDMA slot of a frequency (the inbound slot) and repeats it on
the other slot of the same frequency (the outbound slot), 90 ms later. Radios in direct mode hear both copies. Switch:
`CONFIG_SFR`, off by default (`docs/build-config.md`). Background and the HR_C6000 findings: `docs/spec/gotchas.md`
8.11, 8.12 and 14; the host protocol: `docs/spec/mmdvm-protocol.md` 6.

## 1. Layers

| Layer | File | What it does |
|---|---|---|
| Logic | `core/dmr/sfr.c` | roles, channel settings, framing, the repeat queue and its rules (`sfrEngine_t`: RF priority, TOT, data hold, hang), voice dedup, late entry, the uplink rules, checksums, version suffix, the MMDVM trigger. No hardware |
| Runner | `core/dmr/sfrRunner.c` | the repeater roles on any modem behind `hal_dmr_modem_t`: received bursts filtered and fed to the engine, the outbound slot kept fed, an optional uplink to a host |
| Hotspot SFR mode | `core/hotspot/hotspotSfr.c` | the MMDVM extension (version suffix, status flag 0x80, the space bytes, 108 extra status bytes, command 0xD5) and `hotspot.c`'s other mode (`hotspot_alt_t`) |

The legacy-flavored HR_C6000 glue in `drivers/modem/hrc6000/sfr_glue/` is not built; it documents what a driver
level SFR did (raw RX RAM reads, RF switching per burst) and holds the extra status bytes' layout.

## 2. What is built

- **Hotspot SFR mode.** The host turns it on with SET_CONFIG's simplex flag clear and SET_FREQ RX = TX (both non-zero),
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
    colour code from SET_CONFIG. `hotspot_sfr_set_settings()` takes a channel's settings once a UI or codeplug
    provides them; command 0xD5 overrides the framing at run time.
- **Runner behavior** (gotchas 14):
  - a burst the modem places on the outbound slot (`ts` = the outbound slot) is ignored: our echo or another
    repeater's;
  - own echo: a burst with the kind, position and payload of one we put on air in the last 250 ms is dropped (the
    HR_C6000 reports what it sent in empty inbound slots);
  - late entry: outside a voice call a lone voice A is held; a B 60 ms after it starts the call;
  - while keyed (or for a burst the modem reports out of step) the checksums are checked here, not taken from the
    chip: RS(12,9) of headers and terminators, CRC-CCITT of PI, CSBK, MBC and data headers. A header or terminator
    that fails and matches the call's LC in its first five bytes is rebuilt from the call's LC (`sfrLcBurstGuess`);
  - keyed aligned to the inbound call (`DMR_TIMING_ALIGNED`, the outbound TS) when one is being heard, else on its own
    slot clock (`DMR_TIMING_OWN`), always impolite (gotchas 1.8); two bursts kept queued in the modem, one engine
    decision per outbound slot, idle bursts in the gaps and the hang; `DMR_EV_TX_REJECTED` unkeys.
- **Debug link** (section `hotspot`): `sfr.on sfr.keyed sfr.busy sfr.slots sfr.rx sfr.repeated sfr.ignored
  sfr.echo sfr.bad_crc sfr.tx_bursts sfr.tx_idle sfr.tx_starts sfr.up sfr.up_lost`.

## 3. What the modem driver must do

The runner assumes these of `hal_dmr_modem_t`; they are the open work for each radio's driver:

1. Keep receiving, and report bursts, while it transmits: the inbound slot is heard between our bursts. Report `ts`
   when it knows the slot, and `in_step` false for a burst whose status it can't trust.
2. `tx_begin(DMR_TIMING_ALIGNED, ts)`: send in the slot opposite to the bursts being received, in step with them
   (the repeat goes out 90 ms after the burst, gotchas 14.3). `DMR_TIMING_OWN` with a ts: its own slot clock.
3. `dmr_tx_burst_t.sync_override`: the outbound sync (MS, BS, or TS1 / TS2 direct mode sync) on every burst.
4. `DMR_BURST_IDLE` with the payload given (the ETSI idle message) and no CRC of its own (gotchas 14.4); data bursts
   with `host_crc` sent as they are.
5. `DMR_EV_TX_SLOT_STARTED` once per burst taken from its queue, `DMR_EV_TX_DONE` after `tx_end` has drained it.
6. An SFR channel never switches to repeater (RMO) mode on BS sync (gotchas 14.7).

Today's DM-1701 driver (`drivers/modem/hrc6000/hrc6000_dmr.c`, owned by the DMR modem work) already honors
`sync_override`, idle bursts, `host_crc`, `DMR_TIMING_ALIGNED` (to the slot numbering it learned from the bursts
heard) and the TX slot events, and it keeps passing reports up while it transmits. What it lacks is item 1 at the RF
side: the radio keys the transmitter for the whole transmission, so nothing is received in the inbound slots between
our bursts. An SFR needs the RF switched between receive and transmit per burst, inside the guard time (the legacy
glue did it from the slot interrupts: `radioSfrSwitch` in `drivers/modem/hrc6000/sfr_glue/hrcSfr.h`). Until a driver
does, the hotspot SFR mode answers the host correctly and repeats what it heard before keying, but misses inbound
bursts while it is keyed (the rest of a call after the first repeat). Not tested on air.

## 4. Not built yet

- The stand-alone repeater roles (Rpt, Rpt Moto) and the user roles (User, ERDM) on a channel: they need the
  codeplug's SFR bytes (`sfrChannelDecode`), a radio core mode that hands the modem to the runner (repeater) and
  call stack changes for the users (transmit on the inbound slot aligned to the repeater, hear both copies with
  `sfrDedupVoice`, the ERDM outbound colour code). The call stack is `core/dmr/call/dmr_call.c`.
- The HR_C6000 driver level parts: raw RX RAM classification (`sfrRawClassify`), per-burst RF switching, the
  register switches of command 0xD5.

A two-frequency repeater made of two radios is the split duplex extension (`split-duplex.md`, `CONFIG_HOTSPOT_SPLIT`),
not the SFR: it registers in mmdvm.c and hotspot.c ahead of the SFR (a priority extension), so while a split role
is active the SFR stays off, and without a role the SFR works exactly as described here.

## 5. Tests

- Host: `tests/host/sfrTest.c` (the logic), `tests/host/hotspotSfrTest.c` (the hotspot SFR mode through mmdvm.c and
  hotspot.c against the simulated modem: trigger and suffix, status bytes, an RF call repeated and sent up, a network
  stream, RF priority, echo, late entry, DMR_LOST1, command 0xD5, DMR_ABORT, back to simplex).
- HIL: `tests/hil/test_hotspot_sfr.py` (SFR mode on and off through MMDVM on the radio's channel, no traffic sent;
  marked `radio_tx` because a call heard would be repeated). On-air repeat tests need the driver work of section 3
  and the SDR bench (`tools/bench/mmdvm_hotspot.py --sfr`, `tools/bench/sfr_call.sh`).
