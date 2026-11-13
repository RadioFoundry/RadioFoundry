<!-- SPDX-License-Identifier: MIT -->

# DMR bench testing on the SDR bench

How to test the DMR work (the board's modem driver behind `hal/hal_dmr_modem.h`, the call stack in `core/dmr/call/`,
the AMBE FEC and vocoder hosting, the audio path) on the bench: the rig, the tools, and the cases a person runs at the
radio.

Status words as in `docs/spec/gotchas.md`: "on air" = checked with the SDR or another radio.

A radio's checklist of required HIL tests is `tools/bringup.py list --radio <id>`
([tests/hil/README.md](../../tests/hil/README.md#bring-up-mode)). The cases here are the manual procedures and what
the HIL tests don't script yet.

## 1. The rig

| Part | What |
|---|---|
| Radio under test | A radio with RadioFoundry, on the bench, on USB: driven by the debug link (`tools/rfoundry_debug.py`, the HIL tests) or its keys; the board's diagnostics page |
| Bench board | A board that serves the bench line protocol (TCP 7373): it builds and sends DMR bursts as jobs, decodes what it receives into burst records, and sends and receives FM. Given as `--bench-api HOST[:PORT]` or `$RFOUNDRY_BENCH_API` |
| Second bench board (optional) | The receive side (`--bench-api-rx`, `$RFOUNDRY_BENCH_API_RX`). A half-duplex board can't hear while it transmits |

The test channel is the rig's (`--dmr-freq` / `--dmr-cc`: tests/hil/README.md, "The rig's configuration"). It must be
inside the board's transmit list and receive window.

## 2. The tools

All in `tools/bench/`, MIT.

| Tool | What |
|---|---|
| `bench_line.py` | Client of the bench line protocol. `bench_line.py HOST[:PORT] info` shows the board; `run bench_tx ARGS...` sends one job |
| `mmdvm_hotspot.py` | The radio as an MMDVM modem over USB CDC (or the optional MMDVM UDP transport): configure simplex or SFR, poll status, log what it sends up; our extensions: command 0xD5 (SFR experiment switches) and the SFR counters at the end of GET_STATUS. Needs pyserial |
| `dmr_packet.py` | Reassembles the data packets in a listen job's JSON lines and decodes the IP in them |
| `dmr_cach.py` | The CACH and short LC, from the standard |
| `dmr_bench.py` | The DMR TX checks the HIL tests share (`check_tx`, `check_modem_calls`), their limits and a board's overrides |
| `sdrmon.py` | The carrier floor and off-slot residual of a power envelope |

Burst analysis of what the radio sends comes from the receive board's records through the HIL harness
([tests/hil/README.md](../../tests/hil/README.md#the-bench-board)).

## 3. The radio receives

The bench board sends with `bench_line.py HOST run bench_tx ARGS...`; the user reads the diagnostics page and the main
screen. Each transmission is a known stimulus: source 9990301, AMBE silence unless stated. Many of these are also
automated in `tests/hil` (`test_dmr_rx`, `test_dmr_call_features`, `test_dmr_data`). A board refuses an option it
doesn't do (`unsupported`).

| Case | Job | Radio should show | Pass |
|---|---|---|---|
| R1 group voice | `voice --dst TG --superframes 3` | +18 voice bursts, 8 headers, 3 terminators, CRC errors unchanged, the channel's CC, sync MS; call 9990301 -> TG | All counted, call shown and ended by the terminator |
| R2 private | `voice --private --dst <radio ID>` | Private call from 9990301 | Shown; not shown when sent to another ID |
| R3 late entry | `voice --late 8 --superframes 5` (no header, first 8 bursts dropped) | Call acquired from the embedded LC | Right caller (not a stale LC) within ~1 s; no call started by a lone burst |
| R4 back to back | `voice --superframes 2 --calls 3 --gap-ms 60` | Three calls, callers alternate | None merged or lost |
| R5 no terminator | `voice --superframes 2 --terminators 0` | Call ends by time-out | Ends 0.3-0.8 s after the last burst; slot ticks keep counting |
| R6 wrong CC | `voice --burst-cc N` (N not the radio's color code) | No call, no audio | Never counted as voice for us |
| R7 CSBK | `csbk --count 4 --opcode 1F --dst <radio ID>` | +5 CSBKs; with the data service, a call alert | Counted with good CRC |
| R8 data | `data --dst <radio ID> --octets 40` | 2 CSBK + 1 data header + 4 rate 1/2 blocks | Counted with good CRC |
| R9 confirmed data | `data --confirmed` | Data received | Data counted |
| R10 BS sync | `voice --family bs` | Sync family BS | BS reported (no CACH: not a repeater outbound) |
| R11 TS1/TS2 sync | `voice --family ts1`, then `ts2` | Sync family TS1, then TS2 | Both reported |
| R12 long call | `voice --superframes 50` (18 s) | One call, +300 voice bursts | No restart, no SYNC_LOST, CRC errors unchanged |
| R13 emergency / privacy | `voice --emergency`; `voice --service-options 40 --pi` | Emergency shown; PI call not played without a key | As stated |
| R14 talker alias | `voice --superframes 8 --talker-alias 'RadioFoundry bench modem'` | Alias shown | Within ~2 s; the call LC stays right |

## 4. Voice quality

`tests/hil/test_dmr_audio_smoke.py` runs in every bench pass (`tests/hil/README.md`, Default bench pass). RX: the
bench's recording to the call the radio's channel listens to. TX: the voice layer's test signal (RADIO op 33) in place
of the mic, and the bench's recording played back to the radio.

| Direction | Pass |
|---|---|
| RX | `voice.rx_peak` >= 1000, frames not one frame repeated (`voice.rx_repeats`) |
| TX | Distinct, non-silent frames on air, and the played-back recording decodes to audio on the radio |

Missing prerequisites with the radio and bench present fail; they don't skip.

`test_dmr_audio_rx_deviation` checks the receive level window (`docs/spec/dmr-voice.md` 7.1) and needs a bench board
that scales its deviation (info feature `dev_scale`) and sets its level in dBm (`power_dbm`); it skips on others. To
tune a board's receive level, `tools/dmr_rx_sweep.py` (its help has the options), for example:

```
tools/dmr_rx_sweep.py --allow-rf --port PORT --bench-api HOST --freq MHZ --ambe SPEECH.ambe \
    --reg 0:0x44 --values 0x07FF,0x07EF,0x07DF,0x07CF --dbm=-30,-50 --devs 0.85,1.15 --offsets=-1000,0,1000
```

Decoding AMBE to audio on the test host needs an AMBE+2 vocoder, and this project ships none (README,
`docs/CLEANROOM.md`). Options: a DVSI hardware vocoder on USB (ThumbDV / DV3000 class, AMBE-3000 chip), or an open
decoder the tester installs locally, never vendored here. Independent of any vocoder, the radio's own decoder judges a
call (`voice.rx_peak`, the radio's own over played back to it).

## 5. Hotspot and SFR

`test_hotspot` covers the MMDVM protocol and `test_hotspot_sfr` the repeat on air. By hand: `mmdvm_hotspot.py --sfr`
turns SFR on and logs what the radio sends up, and `mmdvm_hotspot.py --debug` sends our 0xD5 experiment switches and
prints the SFR counters from GET_STATUS.
