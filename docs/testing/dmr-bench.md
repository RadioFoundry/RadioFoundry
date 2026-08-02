<!-- SPDX-License-Identifier: MIT -->

# DMR bench testing (DM-1701 on the bench-host rig)

How to test the DMR work (the HR_C6000 driver behind `hal/hal_dmr_modem.h`, the call stack in `core/dmr/call/`, the
AMBE FEC and vocoder hosting, the I2S audio) on the bench, what the tools are, each test case with its pass
criteria, and the order to run them. The tools are in `tools/bench/`; `tools/bench/dmr_bench.py` runs any case end
to end.

Status words as in `docs/spec/gotchas.md`: "on air" = checked with the SDR or another radio.

## 1. The rig

| Part | What | Normally run by |
|---|---|---|
| Radio under test | Baofeng DM-1701 with OpenDigitalRadio, on the bench. No USB in the firmware yet: it is driven by its keys (PTT, channel, P1/P2, side keys) and shows a diagnostics page (long press of the top side key) | the user |
| bench-host (`ssh bench-host`, user `pps`) | Radxa bench-host, 824 MB RAM, Debian, RadioDesk rfgw in `/opt/radiodesk-rfgw/.venv` | `radiodesk-rfgw@bench-host` |
| MMDVM_HS dual HAT | `/dev/ttyS0`, two ADF7021 (simplex or duplex), firmware MMDVM_HS. The other DMR station | rfgw receiver `ROAM1` (in the current config) |
| RTL-SDR | RTL2832U + R820T on USB, bias tee powering an LNA. The instrument | rfgw receivers SDR02..SDR15 (461-462 MHz group, tuner center 461.52875 MHz). SDR15 = 461.6875 MHz |
| RadioDesk server | 192.0.2.10: MQTT broker (rfgw publishes `DMR/<receiver>/sdr/...`), call recording, call audio | the owner |

Test channel used so far: 461.6875 MHz, color code 2 (Ch 15). The radio's codeplug also has DMR channels; any free
channel works as long as it sits well away from the SDR's tuner center (section 3).

While the HAT or the RTL-SDR is used by a test, rfgw must not hold it. The HAT is in rfgw's config now (ROAM1 is
running), so any HAT test stops the service. `dmr_bench.py` does this with a lease that always restores the service
(section 4). The earlier SFR rounds instead ran with ROAM1 taken out of the config
(`/etc/radiodesk-rfgw/bench-host.yaml.pre-sfrtest` is the original); that is a config change and needs the owner.

## 2. The tools

All in `tools/bench/`, MIT. The bench-host-side ones run in rfgw's venv and use RadioDesk code (the `rfgw` package's
MMDVM modem driver, channelizer, FM demodulator, layer 2 helpers, burst simulator; the `radiodesk` server package's
DMR FEC / PDU / packet / AMBE FEC code). That dependency stays outside this repository: the RadioDesk tree is the
owner's own project, installed on the bench-host and at `~/RadioDesk` on the Mac. Copy the bench-host-side
tools with `dmr_bench.py sync` (to `bench-host:~/odr-bench/`).

| Tool | Runs on | What | Needs |
|---|---|---|---|
| `dmr_bench.py` | Mac | Runs a named test case end to end: service lease, captures, HAT, prompts for the user, analysis, automatic checks, report. `list`, `show`, `sync`, `run [--dry-run]`, `check` | ssh to the bench-host, python3 |
| `hat_tx.py` | bench-host | The HAT as a DMR subscriber (simplex): `voice` (group / private, headers, superframes, late entry, back to back, no terminator, sync family ms/bs/ts1/ts2, real speech from an `.ambe` file, talker alias, PI bit, service options, wrong burst CC), `csbk` (preambles, then e.g. a call alert), `data` (unconfirmed / confirmed), `listen --json` (per burst, embedded LC and talker alias per superframe, a summary per call, AMBE files), `--listen-after S` (send, then listen for the radio's answer) | rfgw venv, HAT free |
| `hat_repeater.py` | bench-host | The dual HAT as a local duplex repeater (no network): wakes on the radio's BS_Dwn_Act CSBK, BS sync, CACH with short LC, both slots, call hang; JSON log of wakeups, key-ups and every burst heard on the input | rfgw venv, HAT free, a split frequency pair |
| `sdrmon.py` | bench-host | Raw IQ capture (`capture`, stops / restarts rfgw itself unless `--service ''`) and burst analysis (`analyze`): every burst incl. idle with time to 0.01 ms, slot phase, sync family and bit errors, CC, data type, LCs (header, terminator, embedded), power, frequency offset, key-up / key-down edges and ramps, ADC clipping, `--json` | rfgw venv, numpy, SoapySDR |
| `sdr_watch.py` | bench-host | rfgw's own decode of one SDR receiver from MQTT, with the sync family read from the burst bits. No service stop | rfgw venv, `MQTT_PASSWORD` |
| `mmdvm_hotspot.py` | Mac | Planned firmware capability. The radio as an MMDVM modem over USB CDC (or the optional MMDVM UDP transport): configure simplex or SFR, poll status, log what it sends up, send a network call; our extensions: command 0xD5 (SFR experiment switches) and the SFR counters at the end of GET_STATUS | pyserial; RadioDesk for `--send` |
| `hotspot_cases.py` | Mac | Planned. The automated `rf_*` / `net_*` hotspot cases (HAT sends, radio reports over MMDVM; MMDVM sends, HAT listens) | RadioDesk server venv |
| `sfr_call.sh` | Mac | Planned. One SFR call: radio as SFR over MMDVM + HAT voice + rfgw's SDR decode | ROAM1 out of rfgw's config |

Where they came from: `hat_tx.py`, `sdrmon.py`, `sdr_watch.py`, `hotspot_cases.py` (from `hs_normal.py`),
`mmdvm_hotspot.py` (from `hotspot_test.py`) and `sfr_call.sh` (from `run_sfr_call.sh`) are adapted from our own
earlier bench tools; `dmr_bench.py` and `hat_repeater.py` are new.

What the HAT can and cannot do:

- Simplex (DMO) transmit of any burst we build, one burst per 60 ms, any sync family, any CC. The MMDVM_HS keeps its
  carrier up for the whole call (one keyed segment on the SDR), unlike a radio, which keys per burst.
- Simplex receive: **MS sync only**. BS-synced and TDMA direct mode (TS1/TS2) bursts from the radio are not
  reported; use the SDR for those.
- Duplex (dual HAT): a real repeater outbound (BS sync, CACH, both slots, idle bursts) on a TX frequency different
  from its RX frequency. It cannot be a single frequency repeater.
- The HAT does not decode or encode audio. It never needs a vocoder: voice content is AMBE frames we supply.

## 3. Clean SDR captures: levels and the DC spike

**Clipping.** With the radio on the bench, the RTL-SDR saturates: at gain 15 dB (sdrmon's old default; rfgw runs 30)
28 % of samples were at the rails when the radio keyed at 1 W nearby, and an existing HAT capture at gain 15 shows
8.4 %. A clipped capture has wrong power, ramps and frequency offset and loses bursts. `sdrmon.py capture` now
records the clipping share in the capture's JSON and warns above 1 %; `analyze` recomputes it and `dmr_bench.py`
fails a TX check on a capture over 1 %.

- Run case **C0** first: it captures 4 s at gains 0, 5, 10 and 15 dB while the radio holds PTT, and prints the
  clipping at each. Pick the highest gain with clipping under 0.1 % and bursts at or below about -6 dBFS; pass it as
  `--gain` (the dmr_bench default is 5).
- Radio on **low power** for all TX captures; the HAT at `--hat-power 40` (of 255), as in the earlier clean runs.
- Still clipping at gain 0: move the SDR antenna away (a metre helps a lot), put a 20-30 dB SMA attenuator on the
  SDR, or run the radio into a dummy load (it leaks enough for the HAT and the SDR at bench distance). The bias tee
  powers an LNA; `sdrmon.py capture --no-biastee` turns it off, but check that the unpowered LNA still passes signal.

**DC spike.** The RTL has a spike (and IQ imbalance) at the tuner center. `sdrmon.py` tunes the dongle
`--offset-khz` (default -200 kHz) away from the channel: for 461.6875 MHz the tuner sits at 461.8875. Keep the
channel at least 100 kHz from the tuner center and don't pick a test channel near the tuner center of the capture
(461.8875 MHz with the defaults). The repeater case puts the center 100 kHz beside the midpoint of the pair so both
channels are clear of it.

**Timing of a capture.** `READY` is printed before samples really flow; wait 2 s after READY before keying
(dmr_bench does). The bench-host's 824 MB RAM: analysis reads captures in chunks (about 150 MB for 15 s); don't load a
whole capture into numpy there (the old `~/sfrtest/sdrmon/fmana.py` FM analysis does, and is not carried over).

## 4. Running a case, and the service

```
tools/bench/dmr_bench.py sync                       # once, and after any tool change
tools/bench/dmr_bench.py show T1                    # what it does, the pass criteria
tools/bench/dmr_bench.py run T1 --dry-run --radio-id <ID> --tg 9    # print every command, run nothing
tools/bench/dmr_bench.py run T1 --radio-id <ID> --tg 9 --gain 5
```

The dry run lists the exact commands, including the service stop and start. Get the owner's OK on that before the
first real run of a case that keys a transmitter (the HAT, or the repeater) or stops the service.

The **lease**: a case that needs the HAT or the raw SDR starts one detached process on the bench-host that runs
`sudo systemctl stop radiodesk-rfgw@bench-host`, waits while `~/odr-bench/hold` exists (at most `--lease-max`, 300 s
by default), then runs `sudo systemctl start radiodesk-rfgw@bench-host` and logs it to `~/odr-bench/lease.log`. The
script removes the hold file in a `finally` block and checks that the service is active again. If the Mac, the
script or the ssh link dies, the service still comes back when the lease expires. Ctrl-C at a prompt ends the case
and restores the service. T0 needs no lease.

Results: `bench-results/<date>_<case>/` on the Mac (`report.md`, `report.json`, the sdrmon JSON and text, the HAT
listen JSON lines, the repeater log); the captures stay on the bench-host in `~/odr-bench/runs/<run>/` (about 50 MB per
16 s; nothing is deleted automatically).

**Report format** (`report.json`): `case`, `title`, `run`, `args`, `steps` (each HAT command and outcome),
`radio` (each question asked and the user's answer: the diagnostics counters), `checks` (each automatic check:
`check`, `ok` true / false / null for manual, `detail`), `verdict` (PASS, FAIL, or MANUAL when only the user's
observations decide), `commands` (every command run, in order). `report.md` is the same as a table. Re-run the
checks on saved results with `dmr_bench.py check T1 <sdrmon.json> --hat <listen.jsonl> --radio-id <ID>`.

## 5. Bring-up order and the test cases

Run them in this order. The key-press cases (C, T0, R, T, Q, P) need only the radio's keys and its diagnostics page.
The hotspot cases (H, S) wait for USB + MMDVM in the firmware; once that lands they give much better automated
coverage of the modem driver and the call stack, because every burst the radio hears comes back over USB and every
burst it sends is under our control: run H1 after every driver change then.

What the radio's diagnostics page should show for these tests (firmware request 1): slot ticks, bursts by kind,
CRC errors, last CC, last sync family, last LC. Note the counters before each case; the cases ask for the new
values.

### 5.0 Setup

| Case | Setup | User | Tools show | Pass |
|---|---|---|---|---|
| C0 SDR levels | Radio low power, test channel | Hold PTT for the sweep (~30 s) | ADC clipping per gain | A gain with < 0.1 % clipping chosen |
| T0 TX smoke (no service stop) | Radio on a DMR channel at 461.6875, cc 2 | Key ~3 s | rfgw's SDR15 decode via MQTT: family, type, cc, src, dst | MS data headers, voice, terminator, cc 2, the radio's ID and TG |

T0 is the first thing to run after flashing a DMR TX build: it stops nothing.

### 5.1 The radio receives (a)

The HAT sends; the user reads the diagnostics page and the main screen. Each HAT transmission is a known stimulus:
source 9990301 (and 9990302), AMBE silence unless stated, 8 voice LC headers and 3 terminators (the MMDVM_HS profile).
To check the stimulus itself, run the same `hat_tx.py` command during an `sdrmon.py capture`.

| Case | HAT sends (`hat_tx.py ...`) | Radio should show | Pass |
|---|---|---|---|
| R1 group voice | `voice --dst TG --superframes 3` | +18 voice bursts, 8 headers, 3 terminators, CRC errors unchanged, CC 2, sync MS; call 9990301 -> TG | All counted, call shown and ended by the terminator |
| R2 private | `voice --private --dst <radio ID>` | Private call from 9990301 | Shown; not shown when sent to another ID |
| R3 late entry | `voice --late 8 --superframes 5` (no header, first 8 bursts dropped) | Call acquired from the embedded LC | Right caller (not a stale LC) within ~1 s; no call started by a lone burst |
| R4 back to back | `voice --superframes 2 --calls 3 --gap-ms 60` | Three calls, callers alternate | None merged or lost |
| R5 no terminator | `voice --superframes 2 --terminators 0` | Call ends by time-out | Ends 0.3-0.8 s after the last burst; slot ticks keep counting |
| R6 wrong CC | `voice --burst-cc 3` (radio on cc 2) | No call, no audio | Never counted as voice for us |
| R7 CSBK | `csbk --count 4 --opcode 1F --dst <radio ID>` (+ listen 3 s) | +5 CSBKs; with the data service, a call alert | Counted with good CRC; later: the radio's ack CSBK in the listen output |
| R8 data | `data --dst <radio ID> --octets 40` | 2 CSBK + 1 data header + 4 rate 1/2 blocks | Counted with good CRC |
| R9 confirmed data | `data --confirmed` (+ listen 4 s) | Data received | Later: the radio's response header heard by the HAT within 1 s |
| R10 BS sync | `voice --family bs` | Sync family BS | BS reported (no CACH: not a repeater outbound) |
| R11 TS1/TS2 sync | `voice --family ts1`, then `ts2` | Sync family TS1, then TS2 | Both reported |
| R12 long call | `voice --superframes 50` (18 s) | One call, +300 voice bursts | No restart, no SYNC_LOST, CRC errors unchanged |
| R13 emergency / privacy | `voice --emergency`; `voice --service-options 40 --pi` | Emergency shown; PI call not played without a key | As stated |
| R14 talker alias | `voice --superframes 8 --talker-alias 'ODR bench HAT'` | Alias shown | Within ~2 s; the call LC stays right |

### 5.2 The radio transmits (b)

The user keys the radio; the SDR captures raw IQ (`sdrmon.py`), the HAT listens (`hat_tx.py listen --json`), and
`dmr_bench.py` checks the capture automatically (only the radio's bursts: from its first header with
`--radio-id`, on that header's slot).

| Case | User | Automatic checks |
|---|---|---|
| T1 group call | PTT ~3 s, "one two three" | see below |
| T2 PTT tap | Tap PTT (< 0.3 s) | As T1; the over ends after a voice F with a terminator |
| T3 long over | PTT ~20 s | As T1; spacing stable over ~330 bursts, no missing burst |
| T4 private call | Private call to 9990301 | As T1 with FLCO 3, destination 9990301 |
| T5 reply | Reply right after the HAT's call ends | As T1 for the reply, to TG; in step with the HAT's slot |

T1 pass criteria (all automatic):

- capture clipping < 1 %;
- every burst MS sync (direct mode), color code = the channel's;
- at least one voice LC header (three expected, `docs/spec/dmr-modem.md` 2.5), header and terminator LC with
  source = the radio's ID, destination = the TG, FLCO 0;
- voice A-F in order, whole superframes; the embedded LC from B-E decodes and equals the header's;
- at least one terminator with LC after the last voice burst;
- same-slot spacing 60.00 +- 0.05 ms, standard deviation < 0.1 ms;
- median frequency offset within +-500 Hz (the FM carrier was within 50 Hz);
- the carrier keyed per burst (off in the other slot), each burst on 27-31.5 ms, key-up and key-down ramps < 1.5 ms;
- no overlapping bursts, no power step inside a burst, no carrier without a decoded burst;
- the HAT hears one call, started by its header, ended by its terminator, AMBE FEC corrections 0 on the bench.

### 5.3 Voice quality (c)

| Case | What | Pass |
|---|---|---|
| Q1 radio speech off air | The user talks ~8 s; `hat_tx.py listen --ambe-out` writes the call's AMBE as `.ambe` (9-byte FEC frames) and `.amb` (DSD format, 49 parameter bits per frame) | One call, AMBE FEC corrections 0, non-silence frames during speech; decoded audio intelligible, no clicks or gaps |
| Q2 speech to the radio | `hat_tx.py voice --ambe <recording>.ambe`: a real recorded call (9-byte frames, as RadioDesk's recorder writes them) | Clean speech from the speaker, right level |

Decoding AMBE to audio needs an AMBE+2 vocoder, and this project ships none (README, `docs/CLEANROOM.md`). What
exists on the rig, checked 2026-10-09:

- **Nothing on the bench-host decodes AMBE**: no mbelib, dsd / dsd-fme, md380-emu or Unicorn, and no DV dongle on its USB
  (only the RTL2832U and the WiFi chip).
- **The RadioDesk server** decodes AMBE in process by emulating the MD-380 firmware's vocoder (Unicorn), from
  firmware files it fetches at run time (not in its repository; their license is unknown). It records every call
  rfgw's SDR receivers hear, so a radio call on 461.6875 MHz heard by SDR15 is recorded and playable from RadioDesk
  while rfgw runs (T0, or any key-press TX with no lease). We can use that as an instrument; nothing of it may come
  into this repository.
- `hat_tx.py` writes `.amb` files, which `dsd-fme -r`, `dsd -r` and `md380-emu -d` read, if the person running the
  test has one of those on their own machine.

Options, best first: (1) a DVSI hardware vocoder on USB (ThumbDV / DV3000 class, AMBE-3000 chip): licensed
hardware, decode `.amb` / raw frames with its own host tool on the Mac or the bench-host; the cleanest for objective
quality numbers; (2) the RadioDesk server's playback of SDR15 recordings, as above; (3) an open decoder installed by
the tester locally (dsd-fme + mbelib), never vendored here. Independent of any vocoder, the objective checks are the
AMBE FEC correction count (0 on a clean bench), bit-exact comparison of a known transmitted pattern (firmware request
3) and of a known received pattern (request 4).

### 5.4 Repeater mode (d)

The HAT can be a real repeater (BS outbound, CACH, both slots) with `hat_repeater.py`; it needs a split frequency
pair you may use, ideally within 1 MHz so one SDR capture holds both (the case analyzes both channels from one
capture). `hat_tx.py --family bs` alone only gives BS sync in DMO timing (R10), not an outbound.

| Case | User | Tools show | Pass |
|---|---|---|---|
| P1 BS activation + call | Repeater channel on the radio (TX = `--rpt-rx`, RX = `--rpt-tx`, slot, cc 2); key ~3 s, then again within the hang | Repeater log: wakeup (BS_Dwn_Act, source), key-up, the radio's header and voice on its slot; SDR: the radio's inbound bursts and the outbound | Wakeup from the radio's ID, then the call repeated on its slot; the second over needs no wakeup; the radio's inbound phase-locked to the outbound slot; the radio shows its own call / the hang |

Then (later cases, same tools): a call heard through the repeater on the radio (HAT simplex can't key the duplex
repeater from the other side, so use a second radio or the repeater's network input), slot filter on / off, the
radio not ending its next reception on its own echoed terminator (`docs/spec/dmr-modem.md` 2.2), late entry on a
BS outbound, wakeup retries when the repeater is off (radio sends BS_Dwn_Act every ~600 ms, then gives up).

### 5.5 Privacy, talker alias, late entry, data and SMS (e)

Covered in part by R3 (late entry), R13 (PI bit and privacy flag), R14 (talker alias received), R7-R9 (CSBK,
data). To add as the features land: the radio sending talker alias and GPS (sdrmon decodes the embedded LC at each
voice E, FLCO 4-8); basic privacy both ways (needs AMBE scrambling in `hat_tx.py`; RadioDesk's `radiodesk.dmr.crypto`
can check the radio's encrypted TX off air); SMS both ways (`hat_tx.py data` sends a 40-octet test payload; a real
SMS payload builder is still to write; the radio's SMS is heard by `hat_tx.py listen` as a data header + blocks).

### 5.6 Hotspot and SFR (planned: USB + MMDVM in the firmware)

| Case | What | Pass |
|---|---|---|
| H1 | `hotspot_cases.py --cases all`: rf_voice8, rf_late, rf_long, rf_lost, rf_b2b, rf_csbk, rf_data_unconf, rf_data_conf, net_voice, net_voice_ctl, net_data, net_data_conf, net_b2b | All PASS |
| S1 | Radio as SFR (`mmdvm_hotspot.py --sfr`), one HAT call on the inbound slot, raw SDR capture | Inbound MS -> repeat gap 60 +- 1 ms, no overlaps |

`sfr_call.sh` is the quick SFR loop with rfgw's decode (needs ROAM1 out of rfgw's config); `mmdvm_hotspot.py --debug`
sends our 0xD5 experiment switches and prints the SFR counters from GET_STATUS.

## 6. Firmware requests (radio-side observation)

Smallest useful first.

1. **DMR diagnostics page** (key presses only, now). One page, decimal, counters 16 bit: slot ticks; bursts
   received by kind (voice LC header, terminator, voice A, voice B-F, CSBK, data header, rate 1/2, 3/4, 1, idle, PI
   header); CRC errors by kind; bursts per sync family (MS, BS, TS1, TS2); last CC, last sync family, last data type;
   last header / terminator LC (FLCO, src, dst) and last embedded LC; TX: bursts sent, TX_REJECTED, TX_DONE;
   SYNC_LOST and VOICE_LOST counts; the radio's own ID and the channel's CC / TS / DMO-RMO. A key to zero the
   counters and a key to freeze the display (reading during a call).
2. **RX content check**: on the same page, the number of AMBE frames of the last call, the AMBE FEC bits corrected,
   and a CRC-32 of the 9-byte AMBE frames as received (before decoding). A HAT call of N superframes of AMBE silence
   then has a known frame count and CRC (`hat_tx.py` can print it), so RX is checked bit-exact without a vocoder.
3. **TX test pattern** on a key in the diagnostics page: a DMR group call from the own ID to TG 9 on the current
   channel, 3 headers, 4 superframes of a fixed AMBE pattern (AMBE silence, or a fixed valid non-silence frame),
   terminator, without the vocoder or the microphone; a second key for one CSBK (e.g. BS_Dwn_Act or a call alert)
   and a short unconfirmed data packet. `hat_tx.py listen` and `sdrmon.py` then compare it bit for bit (frames,
   LC, embedded LC) and the TX timing is tested before the codec is.
4. **USB CDC (when USB lands)**: the MMDVM protocol on one CDC ACM interface (enables H1 / S1). For logging, use the
   standard MMDVM debug frames (0xF1-0xF5: text + up to 4 integers), which MMDVMHost and `mmdvm_hotspot.py` already
   log, for modem events (burst kind, sync family, CC, CRC, slot, time in us from the slot interrupt). For the
   counters of request 1, one extension command alongside our 0xD5: **0xD6 GET_DMR_COUNTERS**, reply payload = a
   version byte then little-endian uint16 counters in the request 1 order (new fields only appended), so the tools
   read the counters without anyone reading the screen.
5. Keep the SFR counters and 0xD5 switches (hrcSfr.h) in GET_STATUS when SFR comes back, as the tools already parse
   them.

## 7. First three tests once the DMR driver is flashed

1. **C0** (SDR levels) and **T0** (TX smoke through rfgw's decode, no service stop): T0 is safe to run first: it
   stops nothing and only the radio transmits. If the build only receives, skip T0.
2. **R1** (group voice from the HAT; diagnostics counters): the first RX check of the driver (slot ticks, burst
   kinds, CRC, CC, sync family) and of the call stack's header / terminator handling.
3. **T1** (radio group call: SDR + HAT listen, automatic checks): the full TX timing check (sync, CC, headers,
   superframes, embedded LC, terminator, 60 ms spacing, ramps, frequency offset).
