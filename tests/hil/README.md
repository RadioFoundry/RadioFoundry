<!-- SPDX-License-Identifier: MIT -->

# Hardware-in-the-loop tests

pytest tests that drive a real radio over its USB debug link (`docs/spec/debuglink.md`, `tools/odr_debug.py`) and,
when allowed, the SDR bench as the other station. They are meant to be run again and again: every test leaves the
radio as it found it, and nothing transmits unless the run asks for it.

## Running

Needs Python 3 with `pytest` and `pyserial` (nothing else), and a radio running firmware built with
`CONFIG_DEBUGLINK` (the default) on USB.

```
python3 -m pytest tests/hil -v -rs                    # radio only, never transmits
python3 -m pytest tests/hil -v -rs --radio-port /dev/cu.usbmodem2101
python3 -m pytest tests/hil -v -rs --allow-rf --allow-service-stop          # adds the RF tests
python3 -m pytest tests/hil -v -rs --allow-rf --allow-service-stop --ambe ~/recordings/speech.ambe
python3 -m pytest tests/hil -k ui --shots /tmp/odr-shots                    # keep the screenshots
```

Without a radio every test is skipped with the reason (no failure). The radio is found by its USB ID 1FC9:0094;
`--radio-port` or `$ODR_PORT` picks one when there are several.

### Opt-in flags

| Flag | Allows |
|---|---|
| (none) | radio-only tests: version, screenshots, keys, FM receive, MMDVM requests that don't transmit |
| `--allow-rf` | tests that transmit (marker `rf`: the SDR bench's transmitter; marker `radio_tx`: the radio itself) |
| `--allow-service-stop` | tests that stop the SDR bench's gateway service (marker `bench_service`) to use its transmitter and receiver |

The RF tests need both flags. The gateway service is stopped through `tools/bench/dmr_bench.py`'s lease: a detached
process on the SDR bench stops it, waits while a hold file exists (at most 300 s) and starts it again whatever
happens to pytest or the SSH link; the test removes the hold file in a `finally` block and checks that the service
is back.

Other options: `--tx-power` (bench transmitter level 0-255, default 40), `--dmr-freq` / `--dmr-cc` / `--dmr-tg`
(DMR test channel, default 461.6875 MHz, color code 2, talkgroup 9), `--noaa-mhz` / `--rssi-floor` (FM test),
`--ambe` / `$ODR_HIL_AMBE` (speech recording for the voice test), `--shots DIR`.

## Rig assumptions

- The radio on USB, with a codeplug whose current zone holds a DMR channel on the DMR test frequency (the tests
  look for it by frequency and mode, and put the radio back on its channel afterwards).
- The SDR bench reachable by SSH without a password (passwordless sudo for `systemctl` there), with the bench tools
  installed (`tools/bench/dmr_bench.py sync`). Its gateway configuration has Ch 1 at 467.375 MHz and Ch 15 at
  461.6875 MHz, color code 2; the tests use Ch 15. `$ODR_SDR_BENCH_HOST` overrides the SSH host.
- An antenna or attenuator so the bench transmitter at power 40 doesn't overload the radio.
- For `test_fm`: NOAA weather radio audible at the bench (162.550 MHz by default; change it with `--noaa-mhz`).
- Speech recordings for `test_dmr_voice` stay outside the repository; give the path with `--ambe` or
  `$ODR_HIL_AMBE`. Never commit a recording.

## Tests

| Module | What | Needs |
|---|---|---|
| `test_boot.py` | version / features, ping latency, screenshot geometry, firmware clock vs wall clock (no stalls), view sections, diagnostics clock, error replies (unknown command, bad CRC, oversized request), log ring | radio |
| `test_ui.py` | UP / DOWN change the channel on Home, Quick settings open and close (screenshots differ, then match), diagnostics page, injected keys count as activity | radio |
| `test_fm.py` | VFO on NOAA weather: squelch opens or RSSI above the floor within 10 s (receive only) | radio with the FM VFO |
| `test_hotspot.py` | MMDVM GET_VERSION / GET_STATUS answered, the debug link answers in hotspot mode, hotspot left through the debug link (no transmission) | radio with the hotspot |
| `test_dmr_rx.py` | bench sends 2 group calls (10 superframes, 8 headers, 3 terminators each); counter deltas | `--allow-rf --allow-service-stop` |
| `test_dmr_voice.py` | bench sends a speech recording; every frame reaches the speaker path | the same, and `--ambe` |
| `test_dmr_call_features.py` | bench sends a private call to us and to another ID, an emergency call, a talker alias, a late entry (no header, 8 bursts left out), a call with the privacy service option (without and with a key), a call with the PI bit; plus the talk target / privacy key requests (radio only) | `--allow-rf --allow-service-stop` (the last test: radio) |
| `test_placeholders.py` | planned: radio DMR TX checked by the SDR bench's receiver; incoming private voice call through the RadioDesk sandbox alert API | skipped |

### Pass criteria and tolerances

`test_dmr_rx`: voice bursts exactly 120; voice LC headers 14 to 16 (the first header of a call can fall in the
receiver's acquisition); terminators 5 to 6; CRC errors 0; the call stack saw exactly 2 calls.

`test_dmr_voice`: frames into the AMBE voice layer = the frames sent (the recording padded to whole superframes of 18
frames); frames played at least sent - 3 (`AMBE_VOICE_PRIME`) and at most sent; no late entry; zero concealed,
muted, lost or overflowed frames in the voice layer, zero lost frames in the call stack, zero audio overruns and
dropped blocks. When it fails, the printed deltas show which layer lost frames: modem (`modem.voice`, `modem.crc`),
call stack (`call.rx_voice_bursts`, `call.rx_lost_frames`), voice layer (`voice.rx_in`, `voice.rx_frames`,
`voice.rx_concealed`, `voice.rx_underruns`, `voice.rx_overflows`) or audio (`audio.ov`, `audio.dr`).

`test_dmr_call_features`: one call heard per RF test (`call.rx_calls` delta 1). Private to us: played, `last.to_us=1`,
the reply target is the bench ID. Private to another ID: counted, 0 bursts played. Emergency: `last.emergency=1`.
Talker alias: `call.alias` equals the text sent, `call.rx_alias_complete` 1. Late entry: `call.rx_late_entries` 1,
18 to 28 voice bursts played of the 28 sent after the gap. Privacy option without a key: 0 bursts played, 20 or more
dropped as encrypted; with a key: none dropped, 22 or more played. PI bit: only that one call is heard (the
DM-1701's modem driver clears PI on voice, so it plays as clear).

Counter names: `Radio.counters()` merges the diagnostics lines (`modem.*` from the DM-1701's HR_C6000 lines, `call.*`
and `voice.*` short names, `audio.*`) with the `dmr_stats` and `hotspot` view sections (full names such as
`voice.rx_frames`, `hs.net_frames`).

## Writing a test

- Use the `radio` fixture (an `odr_hil.Radio`); the autouse `restore_radio` fixture snapshots the radio's channel or
  VFO, zone, power, squelch and monitor before the test and puts them back afterwards, and releases PTT. A test that
  changes the VFO's frequency wraps that in `with radio.vfo_session():` (the VFO frequency isn't visible while on a
  channel).
- Drive the radio with `radio.press("up")`, `radio.long_press("back")`, `radio.link.key_hold("monitor", 2000)`,
  radio requests (`radio.link.vfo_set(hz)`, `radio.link.zone_pos(n)`, ...) and `radio.ptt_for(seconds)` (at most 10 s;
  the firmware releases PTT by itself at its time-out). Call `radio.go_home()` first for UI tests.
- Check state with `radio.wait_for(lambda v: ..., timeout)` on the view (key=value strings, `docs/spec/debuglink.md`
  3.7), never with fixed sleeps alone; counters with `radio.reset_counters()` then `radio.counters()` deltas.
- Anything that transmits gets `@pytest.mark.rf` (the radio: `radio_tx`); anything that uses the SDR bench's
  transmitter or receiver also gets `@pytest.mark.bench_service` and runs inside `with sdr_bench.lease():`.
- Document the pass criteria and tolerances in the module docstring and in the table above.
