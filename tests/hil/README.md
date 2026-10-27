<!-- SPDX-License-Identifier: MIT -->

# Hardware-in-the-loop tests

pytest tests that drive a real radio over its USB debug link (`docs/spec/debuglink.md`, `tools/rfoundry_debug.py`) and,
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
python3 -m pytest tests/hil -k ui --shots /tmp/rfoundry-shots                    # keep the screenshots
python3 -m pytest tests/hil -v -rs -s --allow-rf --allow-service-stop -k "dmr_tx or fm_tx"   # radio TX off air
python3 -m pytest tests/hil -v -rs -s -k cps --allow-codeplug-write --allow-reboot      # CPS write + restart
python3 -m pytest tests/hil -v -rs -s -k aprs --allow-rf --allow-service-stop --aprs-call N0CALL --dmr-freq 467.375  # APRS on air (your call)
python3 -m pytest tests/hil -v -rs -s -k encoder --manual                              # turn the knob when asked
```

Without a radio every test is skipped with the reason (no failure). The radio is found by its USB ID 1FC9:0094;
`--radio-port` or `$RFOUNDRY_PORT` picks one when there are several. The port can also be a pyserial URL, such as
`rfc2217://host:7001` for a radio on another machine's RFC 2217 server.

### Default bench pass

Run this on every radio with DMR before a release, after a modem, vocoder, audio or board change, and after bringing
up a radio:

```
python3 -m pytest tests/hil -v -rs -s --allow-rf --ambe ~/recordings/speech.ambe --dmr-freq MHZ
```

`MHZ` is a frequency where one of the radio's zones has a DMR channel and the SDR bench can transmit and receive. The
run includes the DMR audio smoke (`test_dmr_audio_smoke.py`, marker `audio_smoke`): real audio both ways, which no
frame counter can check. Without `--allow-rf`, a radio or the SDR bench, these tests skip with a reason starting
`DMR AUDIO NOT CHECKED`: such a run hasn't checked DMR audio. With the radio and the bench there, nothing else skips
them: no recording, no vocoder, no DMR channel on `MHZ` or firmware without the view keys fails. Run only the smoke
with `-m audio_smoke`.

### Opt-in flags

| Flag | Allows |
|---|---|
| (none) | radio-only tests: version, screenshots, keys, FM receive, MMDVM requests that don't transmit |
| `--allow-rf` | tests that transmit (marker `rf`: the SDR bench's transmitter; marker `radio_tx`: the radio itself) |
| `--allow-usb-switch` | tests that switch the radio's USB personality to the network adapter and back (marker `usb_switch`); `test_dmr_ip.py` and `test_dmr_ip_burst.py` run pppd on the serial port instead on a radio without USB networking |
| `--allow-service-stop` | tests that stop the SDR bench's gateway service (marker `bench_service`) to use its transmitter and receiver. Not needed on a bench that runs the shared bench daemon (nothing is stopped); without the daemon and without the flag those tests skip when they take the lease |
| `--allow-codeplug-write` | tests that write the codeplug over CPS (marker `codeplug_write`): the qdmr regions are read and saved to the test's tmp directory first, one channel name is changed, and the backup is written back and verified in a `finally` block |
| `--allow-reboot` | tests that restart the radio (marker `reboot`) through the debug link's REBOOT (mode 0) |
| `--manual` | tests that ask a person at the radio to do something (marker `manual`; run with `-s` to see the prompts), e.g. turn the channel knob |

The RF tests need both flags. The gateway service is stopped through `tools/bench/dmr_bench.py`'s lease: a detached
process on the SDR bench stops it, waits while a hold file exists (at most 300 s) and starts it again whatever
happens to pytest or the SSH link; the test removes the hold file in a `finally` block and checks that the service
is back.

Other options: `--codeplug-backup-dir` (where codeplug_write tests keep their backups, outside the repository), `--codeplug-image FILE` (or `RFOUNDRY_HIL_CODEPLUG_IMAGE`: the radio's codeplug as a file, taken as the backup; only the sectors an edit writes are read from the radio, to check the file against it, verify the write and verify the restore), `--tx-power` (bench transmitter level 0-255, default 40), `--dmr-freq` / `--dmr-cc` / `--dmr-tg`
(DMR test channel, default 461.6875 MHz, color code 2, talkgroup 9), `--noaa-mhz` / `--rssi-floor` (FM test),
`--ambe` / `$RFOUNDRY_HIL_AMBE` (speech recording for the voice test), `--shots DIR`, `--sdr-gain` (RTL-SDR gain for
raw captures, default 5 dB; `dmr_bench.py run C0` picks it), `--sdr-ppm` (overrides the SDR's frequency error for
FM carrier offsets; by default the SDR bench corrects them itself), `--max-foff` (largest carrier offset the TX tests
accept, default 500 Hz), `--fm-mhz`
(FM TX test frequency, default 461.6875), `--aprs-call` / `$RFOUNDRY_APRS_CALL` (test_aprs: the licensed operator's callsign, RAM only), `--aprs-ssid` (default 15), `--aprs-test-mhz` (APRS AFSK test frequency, default 446.050; why in test_aprs.py), `--aprs-dmr-dst` (the DMR position's destination ID, default 9990301), `--roam-beacon` / `--roam-beacon-len` (test_roaming's beacon interval and duration, default 20 s / 3 s), `--split-tx-mhz` (test_dmr_tx_split_repeater: the radio's TX frequency on its DMR test channel made split, the bench repeater's input; no default), `--split-calls` (that test keys up this many times per run, default 1; the split is written and restored once), `--roam-rx` / `--roam-tx` (test_roaming's bench repeater site as the radio
sees it: its receive and transmit MHz; no default).

SDR bench names come from the environment (`docs/testing/dmr-bench.md` 1): `$RFOUNDRY_SDR_BENCH_HOST` (ssh host or alias,
default `sdr-bench`), `$RFOUNDRY_SDR_BENCH_SERVICE`, `$RFOUNDRY_SDR_BENCH_DIR`, `$RFOUNDRY_SDR_BENCH_PY`, `$RFOUNDRY_SDR_BENCH_ENV_FILE`;
`$RFOUNDRY_SDRBENCH` names an rfoundry_sdrbench checkout when the package isn't installed on the test host.

### Skip kinds

Every skip names its kind. The reason starts with it (`-rs` shows `[opt_in] transmits: run with --allow-rf`), the
report carries it as `report.skip_kind`, and the JUnit XML (`--junitxml`) as the test's `skip_kind` property. A
bring-up run counts a skip as a failure unless its kind is one the feature and the run allow.

| Kind | Meaning | Example |
|---|---|---|
| `not_built` | The firmware was built without it: a switch off (including one the board lists in `BOARD_NOT_WRITTEN`: the hardware could, the code isn't written for the radio yet), or firmware older than the feature | built without CONFIG_APRS; VOX on a radio with no mic level hook |
| `no_hw` | The radio's hardware lacks the capability (`BOARD_UNSUPPORTED`) | the board has no PWM audio output |
| `state` | A precondition the test could set up itself: a mode, a setting, the codeplug, the channel | the radio is in hotspot mode |
| `opt_in` | An opt-in option was not given: an `--allow-*` flag, a frequency, a file, a second radio | run with --allow-rf |
| `bench` | The radio, the SDR bench or the test host's tools (a program, git history) are missing, unreachable or busy | the shared SDR bench stayed busy; the tree is not a git checkout |
| `other_board` | A board-specific test on another board | a wiring check run on a board it doesn't cover |

### Bring-up mode

`features.toml` is the feature checklist: one `[feature.<id>]` table per feature (every switch of
`config/features.mk`, and the features without one) with its switch, spec, optional `source` paths, planned tests
and the manual steps no single test asks for. It has no radio columns and lists no tests: each test names the
feature it checks.

```
@pytest.mark.feature("fm")                                  # required: gates the feature
@pytest.mark.feature("aprs", optional=True)                 # run and recorded, doesn't gate
@pytest.mark.feature("channel_knob", owner="turn the knob by the clicks the test asks for")
@pytest.mark.feature("split_duplex", gate=True)             # the capability gate: runs first
```

A test may carry several feature markers. What a test needs comes from its markers and fixtures: `rf` and `radio_tx`
need `--allow-rf`, `bench_service` and the `sdr_bench` fixture the SDR bench, `manual` a person (`--manual`, and the
marker's `owner=` says what they do), `second_radio` and the `bench_station` fixture a second radio, `usb_switch`,
`codeplug_write` and `reboot` their `--allow-*` flags. `tools/bringup.py` (Python 3.11 or later) resolves each
radio's state for each feature from the board registry (`tools/boards.py`) and the board's build data:

| State | When |
|---|---|
| `n/a` | The switch is in the board's `BOARD_UNSUPPORTED`: the hardware can't (`config.mk`, one switch a line with the reason in its comment: `BOARD_UNSUPPORTED += AM_RX  # no air band receiver`). `tools/boards.py check` fails on one without a reason |
| `not written` | The switch is in the board's `BOARD_NOT_WRITTEN`: the hardware could, the driver or glue isn't written for the radio yet. Same form and check; its tests skip `not_built` ([build-config.md](../../docs/build-config.md#unsupported-not-written-unconfirmed)) |
| `unsupported` | `board.toml` `[features] <id> = { state = "unsupported", reason = "..." }`: built, but known not to work on the radio yet. Code not written for the radio goes in `BOARD_NOT_WRITTEN` instead |
| `off` | The board's default build leaves the switch off |
| `required` | Anything else: the feature's tests must pass on the radio |

```
tools/bringup.py list --radio ID                  # the radio's checklist and the last result per feature
tools/bringup.py run --radio ID [--feature F] -- --allow-rf --dmr-freq ...   # under the radio lock, recorded
tools/bringup.py status                           # every radio and feature
tools/bringup.py check                            # host-side consistency (tools/ci.sh)
```

`run` takes the lock of the radio it runs on (`$RFOUNDRY_RADIO_LOCK`, the file the HIL workflow locks, with `{port}` in it
replaced by the radio's port; without it `~/.rfoundry-radio-<port>.lock`, the port from `--radio-port`, `$RFOUNDRY_PORT` or
`usb`), so a bench with two radios runs a bring-up on each. It fails at once when another run holds it (`--lock-wait S`
waits). It prints the owner actions of the run first; they run with `--manual`. pytest runs with `--bringup FEATURES`,
which runs only the tests marked for those features and makes a required test's skip a failure unless the test's needs
excuse it:

- `other_board`: always excused.
- `bench`: excused when the test needs the SDR bench. A run that finds no radio fails.
- `opt_in`: excused when the test needs something the run didn't allow (`--allow-rf` for `rf`, `--manual` for a
  `manual` test and so on). With every need allowed, a missing option such as `--ambe` fails.
- `not_built`, `no_hw`, `state`: never excused.

An excused skip is recorded but doesn't verify the feature, and an optional test never fails the run. A feature that
depends on hardware glue has a capability gate, its one test marked `gate=True` (`test_split_capability`): it checks
the radio offers the feature and skips `no_hw` when it doesn't. Bring-up runs each feature's gate first. When the gate
fails or skips, the feature's other required tests fail with "capability gate failed" without running.
`tools/bringup.py check` fails on a feature with more than one gate.

### Results

Each `run` writes `docs/testing/results/<board>/<date>-<commit>.json`: the commit and whether the tree had changes, the
git object ids of each feature's spec and source paths, the date, the features, every test's id, the features it counts
for, outcome, skip kind and reason, the firmware's VERSION keys (git, board, features, codec), the allowed options and
the Python and pytest versions. Ports, host names and serial numbers are left out. A record needs the git history
(the commit, the content hashes): in a tree that isn't a git checkout, such as a `git archive` export, `run` stops
before testing unless it has `--no-record`, and `list` and `status` say that nothing was checked for staleness. Commit the record with the change it
checks. A feature's status never understates the radio. The newest record that ran it decides: `pass`, `partial`
(excused skips), `fail`, or `needs re-run` when its `spec` or `source` paths differ from the content the record hashed
(`sources`; by content, so a squash merge changes nothing). Before a run, the board's seed record decides (the evidence
the docs held before bring-up records existed), marked as a seed. With neither, the feature is `built, not yet
recorded`. Only a failing record shows a failure
([docs/testing/results/README.md](../../docs/testing/results/README.md#status)). The steps for a new radio, from its
first record to `supported`, are in [Adding a radio](../../docs/dev/adding-a-radio.md#bring-up-records).

### Bench hardware markers

Every test that uses the SDR bench carries `bench_service` (it takes the lease) and says which piece of the bench it
needs. A test with none of these markers needs no bench. The `bench_hardware` fixture (`hil_plugin.py`) reads the bench's
`rfoundry-bench info` (`SdrBench.lacks`) and skips with the reason when the piece is missing; a bench without the shared
bench daemon is assumed to have both.

| Marker | The test needs | Tests |
|---|---|---|
| `sdr_rx` | the RTL-SDR receiver only | the radio transmits and the SDR measures it: `test_fm_tx.py`, `test_audio_probe.py` (the FM TX probes), `test_mic_gain.py`, `test_hotspot_cwid.py`, `test_usrp_fm_tx`, `test_split_tx_half_*`, the three `test_aprs_on_air_*` |
| `bench_modem` | the bench modem or a bench station only (it transmits to the radio, listens to it or runs as a repeater) | `test_dmr_rx.py`, `test_dmr_voice.py`, `test_dmr_call_features.py`, `test_dmr_data.py`, `test_dmr_emergency.py`, `test_dmr_ip.py`, `test_dmr_ip_burst.py`, `test_control_station.py`, `test_roaming.py` (the bench repeater), `test_hotspot_voice.py`, `test_dmr_gps_tx`, `test_dmr_tx_split_repeater` (its repeater is required, the SDR's view of the radio is optional), `test_aprs_dmr_position`, `test_split_rx_half_labels`, `test_net_split_rx_half_labels`, `test_usrp_dmr_rx`, the four scan / battery-save tests in `test_radio_features.py` |
| `bench_modem` and `sdr_rx` | both | `test_dmr_tx_off_air` (the SDR's waveform and the modem's decode), `test_sfr_repeats_bench_call` (the modem sends the call, the SDR sees the repeat) |
| `sdr_or_modem` | either one; the test accepts what one of them heard | `test_dmr_tx_talker_alias`, `test_dmr_privacy_tx` |
| none (radio only) | neither | everything else |

`test_probe_fm_tx_ram_writer_sweep` holds the lease but uses neither receiver.

### Bursts on the bench, raw I/Q only where needed

A test that needs to know what the radio sent as DMR does not pull raw I/Q any more. `SdrBench.dmr_watch_start()` runs
`rfoundry-bench dmr-bursts` on the bench (`can_watch()`: a bench with the shared daemon whose client has the tool and
whose shared receiver covers the frequency), which decodes the bursts there: sync, color code, LC, FEC, timing and
carrier offset come back as one JSON line per burst, nothing else. `dmr_watch_result()` returns them in the shape of
`analyze_dmr()` (`bursts` with `t_ms`, `slot`, `family`, `kind`, `cc`, `lc`, `foff_hz`, `cach_ts`). The tool uses the
daemon's `bursts` op when the daemon has it and decodes short captures on the client side when it doesn't. Without
the daemon, or outside the shared receiver's window, `dmr_watch_start()` falls back to a raw capture and
`analyze_dmr()`, as before. `RFOUNDRY_HIL_RAW_IQ=1` forces the raw path (to compare the two).

Used by `_over()` (`test_dmr_tx_talker_alias`, `test_dmr_privacy_tx`), `test_dmr_tx_split_repeater` and
`test_sfr_repeats_bench_call`. These stay on raw I/Q:

| Test | Why |
|---|---|
| `test_dmr_tx_off_air` | judges the waveform: burst on-time, keying ramps, power steps, overlaps, ADC clipping |
| `test_split_tx_half_*` | decodes the CACH from the waveform (`sdrmon.py analyze --cach`) |
| FM tests (`test_fm_tx`, `test_audio_probe`, `test_mic_gain`, `test_hotspot_cwid`, `test_usrp_fm_tx`) | deviation, tones, CTCSS / DCS and carrier offset come from the demodulated audio |
| `test_aprs_on_air_*` | AFSK is audio on an FM carrier |

## Rig assumptions

- The radio on USB, with a codeplug that holds a DMR channel on the DMR test frequency in any zone (the tests look
  for it by frequency and mode, the current zone first, and put the radio back on its zone and channel afterwards).
- The SDR bench reachable by SSH without a password (passwordless sudo for `systemctl` there), with the rfoundry_sdrbench
  package in `$RFOUNDRY_SDR_BENCH_PY`'s environment and the bench tools installed (`tools/bench/dmr_bench.py sync`; the
  FM tests copy `tools/sdr_fm_check.py` there themselves). Its
  gateway configuration has Ch 1 at 467.375 MHz and Ch 15 at 461.6875 MHz, color code 2; the tests use Ch 15.
- For the TX tests: the radio has a DMR ID, a DMR channel on the test frequency with a talk target, and may
  transmit on `--fm-mhz` in its FM VFO; it runs on its lowest power, into an antenna or a dummy load at bench
  distance.
- An antenna or attenuator so the bench transmitter at power 40 doesn't overload the radio.
- For `test_fm`: NOAA weather radio audible at the bench (162.550 MHz by default; change it with `--noaa-mhz`).
- Speech recordings for `test_dmr_voice` stay outside the repository; give the path with `--ambe` or
  `$RFOUNDRY_HIL_AMBE`. Never commit a recording.

## Tests

| Module | What | Needs |
|---|---|---|
| `test_health.py` | Any radio's health: display geometry and build features, data flash JEDEC ID and factory calibration, battery reading, a receiver that reads alive, DMR timeslot interrupts, the stock codec, GPS sentences. Each board's numbers come from its `board.toml` `[hil]` table (keys in `rfoundry_hil.HIL_KEYS`, read through `tools/boards.py`); a check the board has no expectation for skips `other_board`, one whose diagnostics line the firmware doesn't print skips `not_built` | radio only; a radio without USB on its programming cable: `--radio-port /dev/cu.usbserial-...` |
| `test_board_hil.py` | The board data the HIL tests read, without a radio: every board's VERSION name maps back to it, the expectations use known keys, the health checks pass, fail and skip on a fake radio as they should, and a board's own test runs only on its radio | nothing |
| `boards/<id>/hil/test_*.py` | A board's own checks, which no other radio has a use for (for example `boards/md380/hil/test_md380_rfbus.py`: the bit-banged RF bus reads back and the synthesizer locks). A run of `tests/hil` runs them too; on another radio they skip `other_board` | radio |
| `test_boot.py` | version / features, ping latency, screenshot geometry, firmware clock vs wall clock (no stalls), view sections, diagnostics clock, error replies (unknown command, bad CRC, oversized request), log ring | radio |
| `test_task_stacks.py` | after every view section and a screenshot, the `tasks` view section: every task has at least 64 bytes of stack left since it started (`RFOUNDRY_HIL_STACK_MIN_FREE` overrides), none untracked, the core's and the kernel's tasks present; prints each task's least free stack. Skipped without the section | radio, firmware with `CONFIG_TASK_STACKS` |
| `test_ptt_host_traffic.py` | Host traffic never makes a PTT press (radios whose PTT pin is also the cable's serial line, [gotchas](../../docs/spec/gotchas.md) 16.2; trivially passes elsewhere): 30 s of polling with gaps of 40 to 120 ms around the 50 ms gate and some requests back to back, TX inhibit on (RAM, put back): `input.ptt_downs` unchanged, the state never `tx` | |
| `test_ui.py` | UP / DOWN change the channel on Home; OK opens the Hub (6 rows), digits open its destinations, BACK goes up, long BACK goes Home from depth 3; Quick settings open and close (BACK and QUICK); digits on Home start Find (`ui.query`); long QUICK locks and unlocks; long # opens Recent; diagnostics through Tools (Hub 5, 7); screenshots `ui_quick.png` and `ui_find.png`; injected keys count as activity; the Quick tiles for now (DMR: slot `dmr.ts` + temp 0x20, color code `dmr.cc` + 0x40, hear all `dmr.hear_all_latch`; FM: monitor latch, VOX `vox_on`), each undone by BACK on Home or a second press; Quick 4 on a DMR channel copies it to the VFO (`vfo`, `mode`, the toast; the channel unchanged, the VFO's own values put back); the picker's focus row (`dmr.focus_id`, cleared by long BACK); Settings long press time (`input.long_ms`, set back) and auto lock 30 s (`ui.locked` after 30 s, set back to Off); opt-in: the timeslot saved to the channel and back (`storage.job_busy`, `temporary`), the zone editor's Move down and Move up on the channel in use (`zone.edits`, `zone.edit_last`, `zone_pos`; the zone ends as it was), deleting the newest message (`$RFOUNDRY_HIL_DELETE_MESSAGE=1`), factory reset with a restart (`$RFOUNDRY_HIL_FACTORY_RESET=1`). State is read from the link section (`ui.screen`, `ui.depth`, `ui.count`, `ui.locked`, `ui.query`, `ui.toast`) | radio; `--allow-codeplug-write`, `--allow-reboot` |
| `test_encoder.py` | The channel knob: injected detents move to the next channel on Home and down a list (forward), back the other way; with `--manual`, asks for 10 clicks each way and a wobble and checks the decoder's counts (`input.enc_detents` = clicks, `input.enc_transitions` = clicks x `input.enc_steps`, `input.enc_illegal` near 0, no count from the wobble) | radio; a zone of 3+ channels; `--manual -s` and a person for the second test |
| `test_vfo_copy.py` | Copy to VFO (`dmr-voice.md` 2.2), nothing transmitted: RADIO op 32 puts the first FM channel of the zone into the VFO (frequencies, mode, bandwidth, tones), the channel stays as it was, a second copy in the VFO is refused, the VFO keeps the values over a channel and back, then its own values are put back | radio with the FM VFO |
| `test_vfo_dmr.py` | the DMR VFO (`dmr-voice.md` 2.1), nothing transmitted: mode, color code, slot and talk target set over the debug link (RADIO ops 31 and 15) show in the `core` / `dmr` views with no temporary flag, survive a channel and back, and the VFO goes back to FM with the defaults | radio with the DMR and FM VFO |
| `test_fm.py` | VFO on NOAA weather: squelch opens or RSSI above the floor within 10 s (receive only); RX tone squelch: with CTCSS 100.0 and then DCS D023N required, untoned NOAA stays muted (`tone_ok=0`, never `rx`), and opens again without the tone (the squelch stepped down until NOAA opens it) | radio with the FM VFO |
| `test_hotspot.py` | MMDVM GET_VERSION / GET_STATUS answered (client with DTR clear, so its close doesn't end the session), the hotspot takes the modem (slot 2 space, `hs.running`, `hs.starts`), the debug link answers in hotspot mode, hotspot left through the debug link (no transmission) | radio with the hotspot |
| `test_hotspot_sfr.py` | MMDVM SFR on the radio's channel: the version suffix, duplex with RX = TX turns it on (121-byte status, both space bytes 16, `sfr.on`), command 0xD5 ACK / NAK 4, simplex turns it off; sends nothing and expects a quiet channel (`sfr.tx_starts` unchanged). `test_sfr_repeats_bench_call`: the bench sends a TS1-synced call, its header goes up as DMR_DATA1 and the repeat with the bench's LC is on air on TS2 (SDR); then the whole call, heard between the radio's own bursts and repeated (`docs/spec/sfr.md` 3); the SFR keys are in the debug link's `sfr` section | `--allow-rf` (the repeat test also `--allow-service-stop`), firmware with `CONFIG_SFR` |
| `test_sfr_standalone.py` | The codeplug SFR roles without the hotspot, the bench modem as the talker on --sfr-freq (default --dmr-freq): a repeater channel repeats a 4-superframe call (at least 19 bursts, 60 of 72 inbound frames decoded, at most 3 lost, under 4 bit errors a frame; `sfr.rx_fec`, `sfr.rx_fec_pos`); TX inhibit stops the role and it restarts when cleared; the CW ID follows the first call (`dmr.sfr_cwid_sent` or `_cut`); a user channel in receive mode 0 drops the talker's direct copy and in mode 1 plays it (`call.rx_sfr_direct`, `call.rx_voice_bursts`). `test_sfr_chain_rfoundry_user`: RadioFoundry radios only, the --radio-port radio repeating, an ODR talker (`--sfr-talker-port`, keyed with the voice test signal) and an ODR listener (`--sfr-listener-port`) on user channels; per listener receive mode (`--sfr-listen-modes`, default 0,1) the listener plays one call with 90 % of the talker's frames, `voice.rx_peak` at least 1000 and `voice.rx_repeats` at most a tenth of its frames. Skips without a channel of the role on the frequency | `--allow-rf`, SDR bench (the chain test needs none: it takes a bench lease only when one is configured); repeater tests transmit |
| `test_audio_probe.py` | USB audio feasibility measurements ([hardware-dm1701.md](../../docs/spec/hardware-dm1701.md) 21.3): the PCM stream runs on an FM channel; per HR_C6000 setting, whether FM receive audio reaches the I2S link (NOAA on vs a quiet frequency, RMS of both channels); with the bench, whether a 1 kHz PCM tone reaches the FM modulator (voice band deviation, microphone muted). Prints tables, asserts only that they ran; every register touched is restored | firmware with `CONFIG_AUDIO_PROBE=y`; the TX part `--allow-rf --allow-service-stop` |
| `test_usb_network.py` | USB network personality: re-enumerates as CDC-NCM, DHCP gives the host 12.<ID>, ping and the debug link over UDP 3334 at the radio's address, back to serial. Passes on the bench Mac (macOS 26). Firmware before 5a13814 stayed a network adapter when the host never configured it (power it off and on); since then it falls back to serial after 10 s | `--allow-usb-switch`, firmware with `CONFIG_DMR_IP_GATEWAY` |
| `test_tcp_services.py` | The TCP services at the radio's address in network mode (`docs/spec/tcp-services.md`): the HTTP page, `/status` and view JSON, the screen BMP, a 404; KISS over TCP (a foreign-source frame taken by the TNC and refused by the APRS engine; an own-callsign frame on air is in test_aprs.py); rigctld (`f`, `m`, `t`, `get_info`, `+f`, a VFO set 12.5 kHz up and back, `dump_state`, and Hamlib's `rigctl -m 2` where installed); the APRS-IS client with `RFOUNDRY_HIL_APRSIS_SERVER=a.b.c.d[:port]` and NAT on the host. Not run on the bench yet | `--allow-usb-switch`, firmware with `CONFIG_NET_TCP` |
| `test_net_services.py` | the services in USB network mode: mDNS (`rfoundry-<ID>.local` resolves to the radio, `dns-sd -B` / `-L` or python zeroconf find `_rfoundry-debug._udp` on port 3334 with its TXT record), TFTP (`info.txt`, two reads of `codeplug.bin` identical, the current channel's record matches the view); opt-in: a channel renamed over TFTP, read back, the backup written again and verified, a wrong file refused (`docs/spec/mdns.md`, `docs/spec/tftp.md`) | `--allow-usb-switch`, firmware with `CONFIG_MDNS` / `CONFIG_TFTP`; `--allow-codeplug-write` for the write |
| `test_control_station.py` | Control station mode (`docs/spec/usb-network.md` 5): the USB network settings stored (radio op 27) and their view keys; the fixed link (DHCP gives 192.168.10.2, ping and the debug link at 192.168.10.1); booting as the adapter (`usb_mode`); passthrough: an ETSI IP text and a TMS from the bench reach the PC's sockets and not the inbox, no TMS ACK, the PC's datagram goes out from the radio's ID, confirmed data still gets the radio's response. The bench needs the current `bench_tx.py` (`data --ip-text`). Manual follow-up with a dispatch application: usb-network.md 5.6 | radio only for the settings test; `--allow-usb-switch` (and `--allow-reboot` for the boot test, `--allow-rf --allow-service-stop` for the passthrough tests); firmware with `CONFIG_DMR_CONTROL_STATION` |
| `test_snmp.py` | the SNMP agent over the USB network: walks the whole MIB at the radio's link address with `tools/rfoundry_snmp.py`, compares values with the debug link, the SET rules with a temporary write community (restored), a wrong community dropped, the test notification received on UDP 16162; back to serial. Nothing transmitted. Not run yet | `--allow-usb-switch`, firmware with `CONFIG_SNMP` |
| `test_dmr_rx.py` | bench sends 2 group calls (10 superframes, 8 headers, 3 terminators each); counter deltas | `--allow-rf --allow-service-stop` |
| `test_dmr_voice.py` | bench sends a speech recording; every frame reaches the speaker path. `test_vocoder_encode_options` (radio only): the encode options in force follow Mic noise reduction and Mic AGC (`voice.enc_options`, `settings.audio_flags`), and the codec word (`voice.enc_flags`) is 0x800 plus option bits, never 0x100 / 0x4000; it is the word the codec was last given, so one that still belongs to the previous options (no transmission since a change) is only printed | the same, and `--ambe`; radio only for the options test |
| `test_dmr_audio_smoke.py` | DMR audio both ways (marker `audio_smoke`, part of the default bench pass). RX: the bench sends the speech recording to the call the radio's channel listens to (its talk target, or a private call to its ID, on the channel's color code); the decoded audio peaks high and the frames aren't one frame repeated. TX: the radio keys 3 s with the voice layer's test signal in place of the mic (RADIO op 33); the bench modem records the call, the frames on air must be distinct and not silence, and the recording played back to the radio must decode to audio. Prints the mic level of the over (`voice.tx_mic_peak`) | `--allow-rf --ambe`; skips only without the radio, the bench, DMR or `--allow-rf` |
| `test_dmr_tx.py` | radio keys 3 s on DMR (debug-link PTT); SDR capture + sdrmon analysis and the bench modem's listener check src ID, TG, CC, headers / voice / terminator, timing, carrier offset (waveform checks reported as not judged when the capture clips); our ETSI GPS Info from a fixed APRS position decoded by the bench modem's listener (`CONFIG_DMR_GPS` and `CONFIG_APRS`); privacy TX (Motorola BP key 1: service option 0x40 in every LC from us, FID 0x10 in the embedded LCs; a clear over has neither); talker alias TX (the channel's alias flag set over CPS, backed up and restored: the alias on air equals `dmr.tx_alias`); vocoder encode options (`test_dmr_tx_encode_options`, one over each for flags 0x1840, 0x800, 0x1800 and 0x3840, set over the debug link's SETTING 31: the same checks, `voice.enc_options` as set, `voice.enc_flags` after the over equal to the word, the options put back; AMBE parameter statistics of what the bench modem heard printed, not judged); repeater TX on a split channel (the channel's TX frequency set over CPS, backed up and restored; the bench modem as a duplex repeater: the radio wakes it, sees it up while talking, and the repeater takes the call) | `--allow-rf --allow-service-stop` (`--sdr-gain 0` on this bench); the alias test also `--allow-codeplug-write`; the split test also `--allow-codeplug-write --split-tx-mhz MHz` |
| `test_fm_tx.py` | radio FM VFO keys 3 s with CTCSS 100.0, DCS D023N, and at 12.5 / 25 kHz; the 1750 Hz burst, the roger beep (on and off), DTMF (digits, timing, deviation, twist; a held digit stopped by PTT release and by the time-out) and the three lowest power levels; `tools/sdr_fm_check.py` (with `--tones`) on an SDR capture | `--allow-rf --allow-service-stop` |
| `test_mic_uart_inject.py` | The DMR mic path without a person: keys a DMR channel on `--dmr-freq` (lowest power, the firmware's own PTT time-out), sends tone bursts on the radio's serial line (0x55 at 6000 baud, a 3 kHz square, 0.2 s on and off, 8 times), which reach the mic input on radios whose programming cable shares it, and checks the vocoder's mic statistics (`voice.tx_mic_level_db` / `tx_mic_spread_db`) against a quiet over: the bursts must stand out. Catches a mic route that never reaches the codec ADC, which the test signal tests can't. Also checks the debug link answers after the odd-baud bytes and the radio isn't left keyed | `--allow-rf --mic-inject`; a local serial port (not a network bridge), nothing else on it; the DM-1801 Pro, DM-32UV and DB25-D, others with `--mic-inject-any` |
| `test_mic_gain.py` | FM mic gain against deviation ([radio-features.md](../../docs/spec/radio-features.md) 3.6): per Mic gain step (`--mic-gain-steps`, default 3,5,7) at 12.5 and then 25 kHz, the step set over the debug link (SETTING 29), then a person unplugs the cable (on the DM-1801 Pro it cuts the microphone), keys, gives a steady tone or the same sentence, releases and plugs back in while the SDR bench records (`--mic-gain-window` s); prints `tools/sdr_fm_check.py`'s voice peak / rms and tone deviation and GCR_ADCL per over. Asserts only a keyed segment per over and the step read back ([dm1801pro-bringup.md](../../docs/testing/dm1801pro-bringup.md) 5.1). No injected tone stands in for the microphone: they all join after the codec ADC | `--manual -s --allow-rf`; a board with `hal_radio_t.mic_gain` (`set.mic_gain_board=1`) |
| `test_cps.py` | open-firmware CPS over USB with `tools/cps_client.py`: radio info (a radio type from the board's `board.toml` `[cps] radio_types`, struct 3), two reads of the qdmr regions identical, the current channel's record matches the view, a channel change saved by the settings store within 2 s (the `storage` view section); the write dry run (no write: backup file round trip, the rename touches only the name, the write and restore plans match and restore every byte); opt-in: one channel renamed, read back and the backup restored over what the radio then holds and verified; the channel position survives a restart | radio; `--allow-codeplug-write` (`--codeplug-backup-dir DIR` keeps the backups), `--allow-reboot` |
| `test_crash.py` | the crash log (`docs/spec/crash-log.md`): the `crash` view section, CRASH TEST refused without its token; opt-in: a trap instruction and a failed assertion each restart the radio and leave a record (kind, pc, the host task, the build's commit) the next start shows, then cleared | radio; `--allow-reboot` for the crash tests; firmware with `CONFIG_CRASH_LOG` |
| `test_dmr_call_features.py` | bench sends a private call to us and to another ID, an emergency call, a talker alias, an ETSI GPS Info (after an alias), a late entry (no header, 8 bursts left out), a call with the privacy service option (without and with a key), a call with the PI bit; plus the talk target / privacy key requests (radio only) | `--allow-rf --allow-service-stop` (the last test: radio) |
| `test_radio_features.py` | the `features` view section, settings round trip, beep counters (does PCM play in this mode), zone scan + cancel, nuisance delete, VFO band scan, dual watch and its debug link controls (select A or B, stop), battery save cycling and MONITOR waking it (radio only); TX inhibit, TOT warning and time-out, roger beep, 1750 Hz burst, VOX quiet (no key-up with nobody talking) on the FM VFO at lowest power (`radio_tx`); the mic level (`vox.level`, and on the HR_C7000 radios the `mic lvl` diagnostics line: the meter counts PCM halves) with VOX off (radio only); the zone scan, the VFO scan and dual watch hold on a bench DMR call, battery save wakes for it (`rf`) | radio; `--allow-rf` (radio_tx); `--allow-rf --allow-service-stop` (the bench ones) |
| `test_dmr_emergency.py` | DMR emergency: the bench sends an emergency alarm CSBK and an emergency voice call to the channel's talkgroup (alerts raised); the radio sends its alarm (3 CSBKs) and an emergency call with the hot mic (the bench listens); the emergency settings request (radio only) | `--allow-rf --allow-service-stop` (the first test: radio), firmware with `CONFIG_DMR_EMERGENCY` |
| `test_split_duplex.py` | split duplex (`docs/spec/split-duplex.md` 10): the capability gate (both halves offered); 0xD6 INFO / SET_ROLE / STATUS, unknown sub-command and command answers, the SPLIT debug view (no RF); the TX half keyed 10 s at the lowest power, the SDR bench's `sdrmon.py analyze --cach` (BS sync only, 30 ms slots, one key-up edge, CACH decode printed; variant from `$RFOUNDRY_SPLIT_VARIANT`); the RX half labeling bench calls and the label flipping with the epoch | firmware with `CONFIG_HOTSPOT_SPLIT`; the TX and RX tests `--allow-rf --allow-service-stop` |
| `test_hotspot_cwid.py` | MMDVM SEND_CWID in hotspot mode: the radio keys the callsign in Morse on FM on the hotspot TX frequency (the SDR bench records), then the hotspot's DMR set-up is back | `--allow-rf --allow-service-stop` |
| `test_dmr_data.py` | DMR data: bench sends unconfirmed and confirmed data, a TMS and an ETSI text message, a call alert and radio checks to us (the radio's answers listened for); the radio sends a call alert, a radio check (Motorola and ETSI forms) and a text message (TMS and ETSI) to the bench (the bench modem listens); the data settings request (radio only) | `--allow-rf --allow-service-stop` (the first test: radio) |
| `test_dmr_ip.py` | IP over DMR ([dmr-data.md](../../docs/spec/dmr-data.md) 11, 12): the `dmr_ip` section and the IP settings request (radio only); a build with the gateway has a host netif (`net.netif=1`, radio only); a UDP datagram (50000 to 161) put into the IP gateway's air path by the debug link's IP_SEND, heard by the bench modem and decoded by `tools/bench/dmr_packet.py`: SAP 3 with the 9 octet header in auto, SAP 4 with compression off, SAP 4 in all for an IPv4 destination other than the ID's (IP_SEND's explicit address; the gateway ID round trip, radio only), confirmed to the bench (which doesn't answer) with 2 tries: two transmissions (F set, then clear, the same N(S)), the CRC-9s checked, then failed; with `--allow-usb-switch` a datagram from this host over the USB network | `--allow-rf --allow-service-stop` (the first test: radio), firmware with `CONFIG_DMR_IP_GATEWAY`; the last test also `--allow-usb-switch` |
| `test_dmr_ip_burst.py` | UDP over DMR in bursts and at the air MTU ([dmr-data.md](../../docs/spec/dmr-data.md) 12.6, 12.8; [usb-network.md](../../docs/spec/usb-network.md) 3.2): 5 datagrams back to back from the debug link's IP_SEND, from this host over the USB network (or PPP on the serial host port on a radio without USB networking: pppd, root or passwordless sudo), and from the bench to this host's socket (all sent, none dropped, in order); the view keys `net.mtu net.air_mtu` (radio only); DHCP option 26 on the host's adapter (a warning if the host ignores it; over PPP the interface MTU must be `ppp.mru`); the largest datagram each way (431 octets SAP 3 and 412 SAP 4 to the air, 412 and the receiver's 448 from it), one octet more (to the air: refused, counted, nothing on air, the link still works; from it: 41 blocks dropped, the next one delivered); the ICMP fragmentation needed for a DF datagram (a raw socket, root) | `--allow-rf --allow-service-stop --allow-usb-switch` by test, firmware with `CONFIG_DMR_IP_GATEWAY`; the bench's UDP mode needs `tools/bench/dmr_bench.py sync`; the ICMP test runs as root |
| `test_hotspot_voice.py` | the hotspot on air both ways (`tools/bench/hotspot_cases.py` under the lease): bench call and data up the USB link as MMDVMHost would see them (`rf_voice8`, `rf_data_unconf`), a network call and data sent by the host heard on air by the bench modem (`net_voice`, `net_data`) | `--allow-rf --allow-service-stop`, rfoundry_sdrbench and numpy on this machine |
| `test_roaming.py` | DMR repeater roaming: the `roam` view section, the roaming settings round trip (RAM), the zone flag toggled and back, roam now on the current zone (passive: listens only; the radio ends on one of the sites, no probe sent), lock and roam now ending it (radio only); the wake probe brings up the bench repeater (`tools/bench/bench_repeater.py`) as a second site and the radio moves there (`rf`, `radio_tx`, `bench_service`); the survey settings round trip; the bench repeater keys an idle beacon (`bench_repeater.py --beacon`) and the radio, listening only, learns the beacon window from it as its own site and surveys the other sites in the window, or (slow) finds it from a silent site with a discovery visit and measures it (`rf`, `bench_service`). The zone flag and the roaming settings are put back | radio (the current zone with 2+ repeater channels on one talkgroup for roam now); `--allow-rf --allow-service-stop --roam-rx MHz --roam-tx MHz [--roam-beacon S --roam-beacon-len S]`, firmware with `CONFIG_DMR_ROAMING` |
| `test_aprs.py` | APRS (`docs/spec/aprs.md` 13): the `aprs` / `aprs_set` view sections and a setting round trip, the RAM test callsign and host NMEA as the position (radio only); on air at the lowest power on `--aprs-test-mhz` (default 446.050 MHz, never an APRS frequency, no path, the RAM test call `--aprs-call` with `--aprs-ssid`): a position (from the channel and from the FM VFO), a status and a message decoded off air by `tools/aprs_afsk_check.py` on the SDR bench, with deviation, twist, phase continuity and TX delay measured; positions over DMR as LRRP and UDT decoded from the bench modem's listener; the debug link keeps answering (gaps under 0.5 s) while a frame goes out, sent over serial, over the USB network and from a KISS client on TCP. Every APRS setting touched is put back | radio; `--allow-rf --allow-service-stop --aprs-call CALL` (`--allow-usb-switch` for the network cases), firmware with `CONFIG_APRS` and the `aprs.tx_power` view key (the AFSK tests skip without it) |

### Pass criteria and tolerances

`test_dmr_rx`: voice bursts exactly 120; voice LC headers 14 to 16 (the first header of a call can fall in the
receiver's acquisition); terminators 5 to 6; CRC errors 0; the call stack saw exactly 2 calls.

`test_dmr_audio_smoke`: RX: the call played (`call.rx_calls_played` +1); frames into the voice layer at least 90 % of
the frames sent; `voice.rx_peak` at least 1000; `voice.rx_repeats` at most the recording's own repeats + the superframe padding
+ 2 + 2 % of the frames; FEC corrections at most 2 bits a frame on average. TX: the radio made one call of at least 30
frames a second of PTT with at most 10 % `voice.tx_repeats`; the bench heard a call from the radio's ID with at least
as many frames, at most 15 % of them the AMBE silence frame, at most 10 % equal to the frame before and at least a
tenth distinct (the test signal repeats its cycle); played back to the radio, the RX criteria without the FEC limit (the recording carries what the bench heard).
Why: the DM-1801 Pro once played every DMR call silent with every counter clean (each burst read the same stale voice
RAM: `voice.rx_peak` 6, exactly 3 bits corrected a frame). `voice.tx_mic_peak` 0 means the board delivered no mic
signal (a speaker-mic cable that cuts the mic gives 0 too); it is printed, not judged.

`test_dmr_voice`: frames into the AMBE voice layer = the frames sent (the recording padded to whole superframes of 18
frames); frames played at least sent - 3 (`AMBE_VOICE_PRIME`) and at most sent; no late entry; zero concealed,
muted, lost or overflowed frames in the voice layer, zero lost frames in the call stack, zero audio overruns and
dropped blocks; the decoded speech peaks at 1000 or more (`voice.rx_peak`: a decoder fed the wrong bits plays silence
with every counter clean). One concealed block is allowed only for a single underrun after the last frame
(`voice.rx_underrun_at` = frames played): the queue ran dry before the terminator arrived. When it fails, the printed deltas show which layer lost frames: modem (`modem.voice`, `modem.crc`),
call stack (`call.rx_voice_bursts`, `call.rx_lost_frames`), voice layer (`voice.rx_in`, `voice.rx_frames`,
`voice.rx_concealed`, `voice.rx_underruns`, `voice.rx_overflows`) or audio (`audio.ov`, `audio.dr`).

`test_dmr_tx`, `test_fm_tx`, `test_radio_features`, `test_hotspot_voice`, `test_hotspot_sfr`, `test_sfr_standalone`: the criteria are in
each module's docstring (dmr_bench.py case T1's checks for DMR TX; CTCSS within 0.5 Hz and 200-900 Hz deviation at 12.5
kHz, DCS with the right polarity, the 25 / 12.5 kHz deviation ratio 1.5-2.6 for FM; for the radio features: warning
2.5-4.5 s and time-out 5.5-7.5 s with tot_s 6 / tot_warn_s 3, roger=1 seen after release while still on air,
beep.blocks grows when beep.played does).

Screenshots: every SCREENSHOT request freezes the UI's redraws for a moment (`docs/spec/debuglink.md` 3.3), so a
capture taken right after a key can still show the old screen. `Radio.screenshot()` waits out the previous
capture's hold, and `Radio.shot_until(pred)` repeats until the picture is what the test waits for (and not blank);
UI tests use `shot_until`.

`test_dmr_call_features`: one call heard per RF test (`call.rx_calls` delta 1). Private to us: played, `last.to_us=1`,
the reply target is the bench ID. Private to another ID: counted, 0 bursts played. Emergency: `last.emergency=1`.
Talker alias: `call.alias` equals the text sent, `call.rx_alias_complete` 1. GPS Info: `call.rx_gps` 1 or more,
`last.gps_valid=1`, `last.gps_err=1`, latitude and longitude within 2e-5 degree of what was sent. Late entry (after a call from
another ID): `call.rx_late_entries` 1, 18 to 20 voice bursts played of the 28 sent after the gap (voice is taken from
the first burst A, the embedded LC is complete at the next F, burst 18 of 36); the same late call again (same LC as the
last one, so the modem's sticky LC rule waits one superframe more): 12 to 14. Privacy option without a key: 0 bursts played, 20 or more
dropped as encrypted; with a key: none dropped, 22 or more played. PI bit: only that one call is heard (the
DM-1701's modem driver clears PI on voice, so it plays as clear).

`test_dmr_data` (counters from the `dmr_data` section, `docs/spec/debuglink.md` 3.7): unconfirmed data:
`data.rx_headers` 1, `data.rx_packets` 1, `data.rx_for_us` 1, `data.rx_blocks` 4 or more, CRC errors and stale
blocks 0. Confirmed: `data.rx_confirmed` 1, `data.responses_sent` 1, `data.tx_failed` 0. Text message:
`data.sms_received` 1, `data.tms_acks_sent` 1, inbox entry 0 is the text from the bench ID, unread until
MARK_READ; the ACK is the last data list (`data.tx_last_kind=reply`, a newer `data.tx_last_ms`) and the radio's own
transmission (`tx_auto=1`, so it didn't light the screen). Call alert: `data.call_alerts_received` 1, `data.csbk_answers_sent` 1, `data.event=call_alert` from the
bench ID. Radio check: with `radio_check_reply` off (the default) counted and not answered; on, answered
(`data.event=radio_check_answered`). Radio TX: call alert and radio check end `failed` after 3 tries counted by the radio; the bench modem must hear 1
to 3 of them with a good CRC (bench_tx.py listen's `crc_ok`, after the FEC) and every one heard must carry exactly
the expected opcode, FID and addresses (fewer than 3 only warns: the bench modem sits next to the radio and is
overloaded even at 50 mW). The text message (no ACK asked) ends `ok` and its data header (to the bench, from us) is
heard; a send the bench didn't hear is repeated, up to 3 sends. An answer to the bench that the bench modem misses only warns: the radio sends it
within a few frames of the bench's last burst, inside the bench modem's TX to RX turnaround.
ETSI forms (`dmr_ext` section): an ETSI text message in: `data.sms_received` 1, `data.etsi_sms_received` 1,
`data.responses_sent` 1, no TMS ACK, inbox entry 0 with format `etsi`. ETSI text out (no response asked): `ok`,
`data.etsi_sms_sent` 1, a data header with DPF 13 and SAP 10 to the bench heard. Radio check in the ETSI form: 3
tries counted, 1 to 3 heard with a good CRC, each with FID 0x00.

`test_dmr_emergency` (`dmr_ext` section): an alarm in (opcode 0x27, FID 0x10, to the channel's talkgroup):
`data.emergency_alarms_received` 1 or more, `em.alert=1`, `em.alert_kind=alarm` from the bench ID, `data.event=
emergency_alarm`, nothing transmitted (`data.tx_lists` unchanged). An emergency voice call in (another source):
`em.alerts_call` +1, `em.alert_kind=call`, `last.emergency=1`. Alarm out (alarm mode, 3 tries):
`data.emergency_alarms_sent` +3, `em.state=active`, 1 to 3 CSBKs heard with a good CRC, each with FID 0x10, the
talkgroup in octets 4-6 and us in 7-9, at least 0.9 s apart. Emergency call out (call mode, 3 s hot mic, needs the codec): the radio keys by itself
and unkeys, the LCs heard to the talkgroup from us all carry service options bit 0x80, `em.hot_mic_keyed` 1 or more.

`test_aprs`: the view keys present, the SSID round trip; with a test call and host NMEA `aprs.pos_src=host`,
`aprs.lat` / `aprs.lon` within 1e-3 degree. On air (AFSK): `aprs.tx_power=0` before keying (else skipped), one frame
with a valid FCS from `CALL-SSID` to `APZRDF` with no digipeaters, control 0x03, PID 0xF0; the position within 1e-4
degree of 33.7490, -84.3880, the status `>` + its text, the message `:ADDRESSEE:text{N`; `aprs.rf_sent` +1 within
15 s, `aprs.rf_active=0`, the radio idle on its channel afterwards; deviation peak (99.9th percentile) 2.0-4.5 kHz,
|twist| under 6 dB; printed: mark and space deviation, TX delay (flags and carrier to first frame bit), the phase
residual at tone changes (about 10 degrees RMS for a continuous phase generator, about 100 for random restarts).
The message's two retries follow (`aprs.rf_sent` +3, `aprs.msg_tries=3`). DMR (LRRP and UDT): `aprs.dmr_sent` +1, a
data header from our ID to `--aprs-dmr-dst` with a good CRC, all its blocks; LRRP: CRC-32, IPv4 / UDP 4001, document
0x0D; UDT: DPF 0, format 5, block CRC; the position within 2e-5 degree.
`test_dmr_gps_tx`: one call heard by the bench modem, its `gps` within 2e-5 degree of 33.7490, -84.3880, error code 3.

`test_hotspot_cwid` (DM-1701 or another board with `tx_tone_key`): SEND_CWID "N0CALL" (73 units) ACKed, the
GET_STATUS TX flag set while keying and clear after, `hs.cw_sent` +1, `hs.cw_wpm=20`, `hs.cw_last_ms` within 10 %
of 150 + 73 x 60 + 150 = 4680 ms, one keyed segment off air.

Counter names: `Radio.counters()` merges the diagnostics lines (`modem.*` from the DM-1701's HR_C6000 lines, `call.*`
and `voice.*` short names, `audio.*`) with the `dmr_stats` and `hotspot` view sections (full names such as
`voice.rx_frames`, `hs.net_frames`).

### On this bench

The radio overloads both receivers of the SDR bench at bench distance, even at 50 mW: the RTL-SDR clips (8 % of the
samples on DMR at gain 0, 20 % on FM) and the bench modem misses bursts or hears them with bit errors. FM
demodulation and the decoded DMR content survive that; burst waveforms and levels don't. Hence: `--sdr-gain 0`,
the DMR TX waveform checks reported as not judged on a clipped capture, `test_fm_tx_power_levels` skipped while the
SDR clips, and the bench modem's tries counted with a minimum of one. An attenuator between the radio and the bench
(or more distance) would let those checks run fully.

## Writing a test

- Use the `radio` fixture (an `rfoundry_hil.Radio`); the autouse `restore_radio` fixture snapshots the radio's channel or
  VFO, zone, power, squelch and monitor before the test and puts them back afterwards, and releases PTT. A test that
  changes the VFO's frequency wraps that in `with radio.vfo_session():` (the VFO frequency isn't visible while on a
  channel). The session puts the VFO in FM and puts its frequency and mode back afterwards; `vfo_session(mode=None)`
  keeps the mode. The VFO's mode, color code, slot and talk target are its own settings and outlive a power cycle,
  so a test that changes them (Copy to VFO does) puts them back itself.
- Drive the radio with `radio.press("up")`, `radio.long_press("back")`, `radio.link.key_hold("monitor", 2000)`,
  radio requests (`radio.link.vfo_set(hz)`, `radio.link.zone_pos(n)`, ...) and `radio.ptt_for(seconds)` (at most 10 s;
  the firmware releases PTT by itself at its time-out). Call `radio.go_home()` first for UI tests.
- Check state with `radio.wait_for(lambda v: ..., timeout)` on the view (key=value strings, `docs/spec/debuglink.md`
  3.7), never with fixed sleeps alone; counters with `radio.reset_counters()` then `radio.counters()` deltas.
- Anything that transmits gets `@pytest.mark.rf` (the radio: `radio_tx`); anything that uses the SDR bench's
  transmitter or receiver also gets `@pytest.mark.bench_service`, one of `sdr_rx` / `bench_modem` / `sdr_or_modem`
  (both of the first two for a test that needs both, see Bench hardware markers) and runs inside
  `with sdr_bench.lease():` (`transmit` / `voice_call`, `listen_start` / `listen_result`, `dmr_watch_start` /
  `dmr_watch_result` for DMR bursts, `capture_start` / `capture_wait` then `analyze_fm` for raw I/Q).
- Captures go in a run directory on the bench (`sdr_bench.run_dir(tag)`, about 50 MB each). The `sdr_bench` fixture
  deletes the directories it made when the session ends; set `RFOUNDRY_HIL_KEEP_RUNS=1` to keep them for a look.
- Never sleep and capture: use `radio.shot_until(...)` for screenshots that should show a new screen.
- Skip with `rfoundry_hil.skip(kind, reason)` (marks: `skip_mark(kind, reason)`, `skip_if(condition, kind, reason)`),
  never `pytest.skip`: the kinds are in [Skip kinds](#skip-kinds), and `tools/check_hil_skips.py` (run by
  `tools/ci.sh`) fails on a skip without one.
- Document the pass criteria and tolerances in the module docstring and in the table above.
- Write tests for every radio, with the board's numbers in its `board.toml` `[hil]` table (`rfoundry_hil.board_hil`) or
  other registry tables (`rfoundry_hil.board_entry`). A pass limit one radio can't meet is a `[hil.limits]` entry,
  `name = { value, reason }`, not a looser default: the names a board may override are `tools/boards.py` `HIL_LIMITS`,
  the check reads the override (`dmr_bench.limit`) and prints the reason beside its result. A check only one board can use goes in `boards/<id>/hil/test_*.py`:
  `conftest.py` adds those files to a run of this directory and `hil_plugin.py` skips them `other_board` on other
  radios. Give the file a name no other board's test has (pytest imports test files by their base name).
- Mark the test with its feature: `@pytest.mark.feature("<id>")`, `optional=True` when it shouldn't gate, `owner=`
  when it asks a person for something. `tools/bringup.py check` fails on a marker naming a feature `features.toml`
  doesn't have, a `manual` test without `owner=`, and a switch without an entry.
