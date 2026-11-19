<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DMR bench cases

This page covers testing the DMR stack on the SDR bench by hand: the modem driver behind `hal/hal_dmr_modem.h`, the
call stack in `core/dmr/call/`, the AMBE FEC and vocoder hosting, and the audio path. It lists the rig, the tools, and
the cases you run at the radio.

For a radio's checklist of required HIL tests, run `tools/bringup.py list --radio <id>`
([Bring-up mode](../../tests/hil/README.md#bring-up-mode)). The cases here are the manual procedures, including the
ones the HIL tests don't script.

## 1. The rig

| Part | What |
|---|---|
| Radio under test | A radio running RadioFoundry, on USB. You drive it with the debug link (`tools/rfoundry_debug.py`, the HIL tests) or its keys, and read the board's diagnostics page |
| Bench board | A board that serves the bench line protocol (TCP 7373). It builds and sends DMR bursts as jobs, decodes what it receives into burst records, and sends and receives FM. Give it as `--bench-api HOST[:PORT]` or `$RFOUNDRY_BENCH_API` |
| Second bench board (optional) | The receive side (`--bench-api-rx`, `$RFOUNDRY_BENCH_API_RX`). A half-duplex board can't hear while it transmits |
| Second transmitter (optional) | A board that transmits while the first does (`--bench-api-2`, `$RFOUNDRY_BENCH_API_2`), for a test with two signals at once |

The test channel is the rig's (`--dmr-freq` and `--dmr-cc`; see
[The rig's configuration](../../tests/hil/README.md#the-rigs-configuration)). It must be inside the board's transmit
list and receive window.

## 2. The tools

The tools are in `tools/bench/`, under the MIT license.

| Tool | What |
|---|---|
| `bench_line.py` | The bench line protocol client. `bench_line.py HOST[:PORT] info` shows the board; `run bench_tx ARGS...` sends one job |
| `mmdvm_hotspot.py` | Runs the radio as an MMDVM modem over USB CDC (or the optional MMDVM UDP transport): configures simplex or SFR, polls status, and logs what the radio sends up. It also uses the RadioFoundry extensions: command 0xD5 (SFR experiment switches) and the SFR counters at the end of GET_STATUS. Needs pyserial |
| `dmr_packet.py` | Reassembles the data packets in a listen job's JSON lines and decodes the IP in them |
| `dmr_cach.py` | Decodes the CACH and short LC, from the standard |
| `dmr_bench.py` | The DMR TX checks that the HIL tests share (`check_tx`, `check_modem_calls`), their limits, and a board's overrides |
| `sdrmon.py` | Measures the carrier floor and off-slot residual of a power envelope |

The burst measurements of what the radio sends come from the receive board's records through the HIL harness
([The bench board](../../tests/hil/README.md#the-bench-board)).

## 3. The radio receives

Each transmission is a known stimulus: source 9990301, AMBE silence unless the case says otherwise. Many cases are
also automated in `tests/hil` (`test_dmr_rx`, `test_dmr_call_features`, `test_dmr_data`). A board refuses an option it
doesn't support (`unsupported`).

1. Send the case's job from the bench board:

   ```sh
   tools/bench/bench_line.py HOST run bench_tx ARGS
   ```

2. Read the radio's diagnostics page and main screen.
3. Compare them with the table.

| Case | Job | Expected | Pass |
|---|---|---|---|
| R1 group voice | `voice --dst TG --superframes 3` | +18 voice bursts, 8 headers, 3 terminators, CRC errors unchanged, the channel's CC, sync MS; call 9990301 -> TG | All counted; the call is shown and ended by the terminator |
| R2 private | `voice --private --dst <radio ID>` | Private call from 9990301 | Shown; not shown when sent to another ID |
| R3 late entry | `voice --late 8 --superframes 5` (no header, first 8 bursts dropped) | Call acquired from the embedded LC | The right caller (not a stale LC) within about 1 s; no call started by a lone burst |
| R4 back to back | `voice --superframes 2 --calls 3 --gap-ms 60` | Three calls, callers alternate | None merged or lost |
| R5 no terminator | `voice --superframes 2 --terminators 0` | The call ends by time-out | Ends 0.3-0.8 s after the last burst; slot ticks keep counting |
| R6 wrong CC | `voice --burst-cc N` (N isn't the radio's color code) | No call, no audio | Never counted as voice for the radio |
| R7 CSBK | `csbk --count 4 --opcode 1F --dst <radio ID>` | +5 CSBKs; with the data service, a call alert | Counted with good CRC |
| R8 data | `data --dst <radio ID> --octets 40` | 2 CSBKs + 1 data header + 4 rate 1/2 blocks | Counted with good CRC |
| R9 confirmed data | `data --confirmed` | Data received | Data counted |
| R10 BS sync | `voice --family bs` | Sync family BS | BS reported (no CACH: not a repeater outbound) |
| R11 TS1/TS2 sync | `voice --family ts1`, then `ts2` | Sync family TS1, then TS2 | Both reported |
| R12 long call | `voice --superframes 50` (18 s) | One call, +300 voice bursts | No restart, no SYNC_LOST, CRC errors unchanged |
| R13 emergency and privacy | `voice --emergency`; `voice --service-options 40 --pi` | Emergency shown; the PI call isn't played without a key | As expected |
| R14 talker alias | `voice --superframes 8 --talker-alias 'RadioFoundry bench modem'` | Alias shown | Within about 2 s; the call LC stays right |

## 4. Voice quality

`tests/hil/test_dmr_audio_smoke.py` runs in every bench pass
([Default bench pass](../../tests/hil/README.md#default-bench-pass)). For RX, it sends the bench's recording to the
call that the radio's channel listens to. For TX, it uses the voice layer's test signal (RADIO op 33) in place of the
microphone, and plays the bench's recording back to the radio.

| Direction | Pass |
|---|---|
| RX | `voice.rx_peak` >= 1000; the frames aren't one frame repeated (`voice.rx_repeats`) |
| TX | Distinct, non-silent frames on air, and the played-back recording decodes to audio on the radio |

With the radio and the bench present, a missing prerequisite fails the test; it doesn't skip.

### Receive level

`test_dmr_audio_rx_deviation` checks the receive level window (`docs/spec/dmr-voice.md` 7.1). It needs a bench board
that scales its deviation (info feature `dev_scale`) and sets its level in dBm (`power_dbm`), and it skips on others.
To tune a board's receive level, use `tools/dmr_rx_sweep.py` (its help lists the options):

```sh
tools/dmr_rx_sweep.py --allow-rf --port PORT --bench-api HOST --freq MHZ --ambe SPEECH.ambe \
    --reg 0:0x44 --values 0x07FF,0x07EF,0x07DF,0x07CF --dbm=-30,-50 --devs 0.85,1.15 --offsets=-1000,0,1000
```

For a carrier offset window, `--detune --offsets=-4000:4000:500` tunes the radio away from the bench's carrier instead
of moving the carrier. Any bench board can do this, and it reaches past the bench's own offset limit (2.5 kHz).
`test_dmr_audio_rx_offset_wide` checks the window from the other side, with the bench's carrier offset.

### Strong signals

`test_dmr_audio_rx_strong` checks the receiver's AGC with strong simplex speech (-75 and -55 dBm at the radio), where a
clean receiver corrects almost nothing. A bit or more per frame there, with none from a repeater's continuous carrier,
means the AGC overloads the start of every burst (`docs/spec/dmr-voice.md` 7.1).

1. Sweep the AGC's settings at those levels, one bit field at a time with `--field`:

   ```sh
   tools/dmr_rx_sweep.py --allow-rf --port PORT --bench-api HOST --freq MHZ --ambe SPEECH.ambe \
       --reg 0:0x32 --field 2:0 --values 0:7 --dbm=-60,-40,-20
   ```

2. Check the winning value near sensitivity as well: levels about 120 dB down at the radio, `--devs 0.85,1,1.15`.

### Decoding AMBE on the host

Decoding AMBE to audio on the test host needs an AMBE+2 vocoder, and the project ships none (README,
`docs/CLEANROOM.md`). Use a DVSI hardware vocoder on USB (ThumbDV or DV3000 class, AMBE-3000 chip), or a decoder you
install locally; never add one to the repository. Without any vocoder, the radio's own decoder judges a call
(`voice.rx_peak`, with the radio's own over played back to it).

## 5. Hotspot and SFR

`test_hotspot` covers the MMDVM protocol, and `test_hotspot_sfr` covers the repeat on air. To check them by hand:

| Command | Does |
|---|---|
| `mmdvm_hotspot.py --sfr` | Turns SFR on and logs what the radio sends up |
| `mmdvm_hotspot.py --debug` | Sends the 0xD5 experiment switches and prints the SFR counters from GET_STATUS |
