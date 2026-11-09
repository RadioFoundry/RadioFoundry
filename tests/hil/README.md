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
python3 -m pytest tests/hil -v -rs -s -k aprs --allow-rf --allow-service-stop    # APRS on air (the rig's callsign)
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
| `--radio-tx-watts W` | the radio's on-air tests key at its lowest power level named at least W watts (the core view's `power_names`), not the lowest: for a bench that can't hear the lowest level through a dummy load. The hotspot tests use the matching RF level. APRS on its ham test frequency keeps the lowest |
| `--allow-usb-switch` | tests that switch the radio's USB personality to the network adapter and back (marker `usb_switch`); `test_dmr_ip.py` and `test_dmr_ip_burst.py` run pppd on the serial port instead on a radio without USB networking |
| `--allow-service-stop` | accepted and ignored: a bench board stops nothing |
| `--allow-codeplug-write` | tests that write the codeplug over CPS (marker `codeplug_write`): the qdmr regions are read and saved to the test's tmp directory first, one channel name is changed, and the backup is written back and verified in a `finally` block |
| `--allow-reboot` | tests that restart the radio (marker `reboot`) through the debug link's REBOOT (mode 0) |
| `--manual` | tests that ask a person at the radio to do something (marker `manual`; run with `-s` to see the prompts), e.g. turn the channel knob |

Other options: `--codeplug-backup-dir` (where codeplug_write tests keep their backups, outside the repository), `--codeplug-image FILE` (or `RFOUNDRY_HIL_CODEPLUG_IMAGE`: the radio's codeplug as a file, taken as the backup; only the sectors an edit writes are read from the radio, to check the file against it, verify the write and verify the restore), `--rssi-floor` (FM test),
`--ambe` / `$RFOUNDRY_HIL_AMBE` (speech recording for the voice test), `--shots DIR`, `--max-foff` (largest carrier offset the TX tests
accept, default 500 Hz), `--aprs-ssid` (default 15), `--no-gps` (the radio has no GPS module fitted: test_gps_nmea skips), `--roam-beacon` / `--roam-beacon-len` (test_roaming's beacon interval and duration, default 20 s / 3 s), `--split-tx-mhz` (test_dmr_tx_split_repeater and test_dmr_repeater_slots: the radio's TX frequency on its DMR test channel made split, the bench repeater's input; no default), `--outbound-mhz` (test_dmr_outbound_voice: a repeater's output the bench board sends BS bursts and voice on; the radio needs a duplex DMR channel receiving it; no default), `--am-mhz` (test_am_rx and test_am_rx_speaker: the AM test frequency in the air band, 108-137 MHz, that the bench board sends full-carrier AM on and the radio's VFO receives; the bench transmits on it: a cabled setup unless the frequency is approved for the air; no default), `--split-calls` (that test keys up this many times per run, default 1; the split is written and restored once), `--roam-rx` / `--roam-tx` (test_roaming's bench repeater site as the radio
sees it: its receive and transmit MHz; no default; with `--allow-codeplug-write` the zone's sites are given TX
frequencies the bench serves for the test and put back after). The rig's own values (frequencies, color code, talkgroup,
callsign) are in the next section.

### The rig's configuration

The repository holds no frequency, callsign or ID of any rig. Each value comes from the command line, else
`$RFOUNDRY_HIL_<NAME>` (the option's name in capitals, `_` for `-`), else the rig's TOML file: `--hil-config FILE`,
else `$RFOUNDRY_HIL_CONFIG`, else `~/.config/rfoundry/hil.toml` when it exists. The file's keys are the option names
with `_` for `-`; an unknown key is an error. A test that needs a value no one gave skips (kind `opt_in`) and names
the three ways to give it. `tests/hil/hil_config.py` reads them.

| Option | Value |
|---|---|
| `--dmr-freq` / `--dmr-cc` / `--dmr-tg` | the DMR test channel: MHz (simplex, a channel in the radio's codeplug), color code, the talkgroup the bench calls |
| `--fm-mhz` | the FM test frequency (the radio's VFO) |
| `--noaa-mhz` | a NOAA weather radio station heard at the rig (`test_fm`, receive only; optional tests) |
| `--vhf-fm-mhz` | a VHF FM frequency the bench transmits on (`test_fm`: receive in the VHF band) |
| `--aprs-test-mhz` | `test_aprs`'s AFSK frequency: an amateur simplex frequency that is not an APRS one |
| `--aprs-dmr-dst` | the DMR position's destination ID (`test_aprs`) |
| `--callsign` | the licensed operator's callsign: every transmission that carries one (APRS, CW ID, talker alias); RAM only on the radio |
| `--wattmeter-mhz` | where the wattmeter check (`test_fm_tx_power_levels --manual`) keys every power level, comma separated; default 445 and 146, the middle of where hams transmit on 70 cm and 2 m. A frequency not in `tx_mhz` or not tuned by the radio is left out |
| `--tx-mhz` | the frequencies the rig may transmit on, comma separated (a TOML list in the file) |
| `--microphone` | an ALSA capture device (`arecord -D`) whose microphone is at the radio's speaker: the speaker tests ([Speaker tests](#speaker-tests)) |
| `--mic-processed` | `1` when that microphone has processing of its own (a headset's noise suppression): the speaker tests check tone starts only ([Speaker tests](#speaker-tests)) |

With `--allow-rf`, every transmit frequency the run is given (`--dmr-freq`, `--fm-mhz`, `--aprs-test-mhz`,
`--sfr-freq`, `--roam-rx` / `--roam-tx`, `--split-out-mhz`, `--split-tx-mhz`, `--outbound-mhz`, `--am-mhz`,
`--vhf-fm-mhz`) must be in `tx_mhz`, or the run
stops before the first test. The bench client refuses to key anywhere else. Example file (placeholder values: use
your license's and your site's):

```toml
# ~/.config/rfoundry/hil.toml
callsign = "Q0TST"
tx_mhz = [433.4500, 433.4750, 145.5500]
dmr_freq = 433.4500
dmr_cc = 1
dmr_tg = 91
fm_mhz = 433.4750
noaa_mhz = 162.4000
vhf_fm_mhz = 145.5500
aprs_test_mhz = 433.4750
aprs_dmr_dst = 9990301
```

The tools in `tools/bench/` take `--freq` and `--cc` or `$RFOUNDRY_BENCH_FREQ` / `$RFOUNDRY_BENCH_CC`, and the SDR
receiver's name `--receiver` or `$RFOUNDRY_SDR_RECEIVER`; none has a default.

### Transmit frequencies

The radio transmits only where the rig allows, in two layers (`rfoundry_hil.py`, `tests/hil/test_tx_guard.py`):

| Layer | What it covers | How |
|---|---|---|
| 1. Requests | every debug link request that keys the radio: PTT, the PTT or emergency key, the 1750 Hz burst, DMR data sends (SMS, call alert, radio check, emergency, IP), APRS sends | With `--allow-rf`, the frequency it would key on is compared with `tx_mhz` first: the channel's TX frequency (a channel found by its RX frequency can be a split one), for an APRS position the way out `set.aprs_via` picks (the APRS frequency, the DMR channel or both), for an APRS status or message the APRS frequency. Outside it, the request raises. |
| 2. Test TX limit | everything the radio keys, by request or by itself: data and CSBK replies, registration, beacons, roaming probes, a split channel's TX frequency, the hotspot over its MMDVM port | The firmware's test TX limit (RADIO op 35, `radio-features.md` 3.1; RAM only, a restart clears it): every test that uses the radio runs with every transmission blocked unless it is marked `radio_tx`, and a `radio_tx` test may key only on the run's own transmit frequencies (`--dmr-freq`, `--fm-mhz`, `--aprs-test-mhz` and the other frequency options given; `hil_config.run_tx_mhz`). The radio is blocked again after each `radio_tx` test and the limit is off at the end of the run. The radio's TX inhibit setting is left alone. Firmware without the limit has layer 1 only. |

So a test where the radio transmits, by request or by itself, needs `radio_tx`: without it, the transmission is
refused (`tx_deny=inhibit`, or not keyed with `txlim.refused` counted) and the test fails. KEY's physical key bits
(`keys_mask`) are the board's and not checked by layer 1. The second radio of a two-radio test has layer 1 only.

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
marker's `owner=` says what they do), `second_radio` a second radio, `radio_chain` two more (`--sfr-talker-port`), `usb_switch`,
`codeplug_write` and `reboot` their `--allow-*` flags, the `microphone` fixture `--microphone`, and `factory_reset`
`RFOUNDRY_HIL_FACTORY_RESET=1`. `tools/bringup.py` (Python 3.11 or later) resolves each
radio's state for each feature from the board registry (`tools/boards.py`) and the board's build data:

| State | When |
|---|---|
| `n/a` | The switch is in the board's `BOARD_UNSUPPORTED`: the hardware can't (`config.mk`, one switch a line with the reason in its comment: `BOARD_UNSUPPORTED += AM_RX  # no air band receiver`). `tools/boards.py check` fails on one without a reason. A feature without a switch: `board.toml` `[features] <id> = { state = "n/a", reason = "..." }` (key lock on a radio with no keypad) |
| `requires` | Not a state: a feature without a switch of its own that is part of another's (`requires = "DMR"` for DMR data) takes that switch's `n/a`, `not written` or `off` on a board that leaves it out |
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
- `not_built`: excused when the test carries `@pytest.mark.needs_switch("X")` and the board leaves switch `X` out
  (`n/a`, `not written` or `off`; `run` passes them as `--bringup-absent`). A test of a built feature that also
  needs another switch says so with this marker. Never excused otherwise.
- `no_hw`, `state`: never excused.

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
(excused skips), `fail`, or `needs re-run` when its `source` paths differ from the content the record hashed
(`sources`; by content, so a squash merge changes nothing; a spec edit alone changes nothing either). Before a run, the board's seed record decides (the evidence
the docs held before bring-up records existed), marked as a seed. With neither, the feature is `built, not yet
recorded`. Only a failing record shows a failure
([docs/testing/results/README.md](../../docs/testing/results/README.md#status)). The steps for a new radio, from its
first record to `supported`, are in [Adding a radio](../../docs/dev/adding-a-radio.md#bring-up-records).

### Bench hardware markers

Every test that uses the bench board carries `bench_service` (it takes the lease) and says which half of the bench it
uses. A test with none of these markers needs no bench. The `sdr_bench` fixture skips (kind `bench`) without
`--bench-api` or when the board doesn't answer.

| Marker | The test needs | Tests |
|---|---|---|
| `sdr_rx` | the board's receiver only | the radio transmits and the SDR measures it: `test_fm_tx.py`, `test_audio_probe.py` (the FM TX probes), `test_mic_gain.py`, `test_hotspot_cwid.py`, `test_usrp_fm_tx`, `test_split_tx_half_off_air`, `test_split_tx_half_voice`, the three `test_aprs_on_air_*` |
| `bench_modem` | the board's transmitter only (it transmits to the radio, listens to it or runs as a repeater) | `test_dmr_rx.py`, `test_dmr_voice.py`, `test_dmr_call_features.py`, `test_dmr_data.py`, `test_dmr_emergency.py`, `test_dmr_ip.py`, `test_dmr_ip_burst.py`, `test_control_station.py`, `test_roaming.py` (the bench repeater), `test_dmr_gps_tx`, `test_dmr_tx_split_repeater` (its repeater is required, the SDR's view of the radio is optional), `test_dmr_repeater_slots` (the bench repeater only), `test_aprs_dmr_position`, `test_split_rx_half_labels`, `test_net_split_rx_half_labels`, `test_usrp_dmr_rx`, the four scan / battery-save tests in `test_radio_features.py` |
| `bench_modem` and `sdr_rx` | both | `test_dmr_tx_off_air` (the SDR's waveform and the modem's decode), `test_sfr_repeats_bench_call` (the modem sends the call, the SDR sees the repeat), `test_dmr_repeater_burst_offset` (the bench repeater; the SDR times both sides) |
| `sdr_or_modem` | either one; the test accepts what one of them heard | `test_dmr_tx_talker_alias`, `test_dmr_privacy_tx` |
| none (radio only) | neither | everything else |

`test_probe_fm_tx_ram_writer_sweep` holds the lease but uses neither receiver.

### The bench board

`--bench-api HOST[:PORT]` (or `$RFOUNDRY_BENCH_API`) uses a board that serves the bench line protocol (TCP 7373; client `tools/bench/bench_line.py`) as the SDR bench. `--bench-api-rx HOST[:PORT]` (or `$RFOUNDRY_BENCH_API_RX`) is a
second board for what the bench receives. A half-duplex board can't hear while it transmits, so a test that watches
the radio during the bench's own key-up needs the second board.

| Bench call | On a bench board |
|---|---|
| `lease()` | a session on the TX board for the test; a test's transmit frequencies are claimed in it, so another session's jobs wait for them |
| `transmit()`, `voice_call`, `data`, `csbk`, `listen_start()` | a `bench_tx` job (`run`): the board builds and sends the bursts, and a `--listen-after` or `listen` reports what its receiver hears, as JSON lines. The board sends at its own level |
| `repeater_start()` | a `bench_repeater` job; a board that runs no duplex repeater refuses it (skip kind `bench`) |
| `outbound_start()` | a repeater's idle outbound with no repeater behind it (a `bench_tx` `outbound` job): BS bursts on both slots with the CACH |
| `dmr_watch_start()`, `bursts_start()` | the receive board's decoded bursts (`bursts`) |
| `tx_start()`, `fm_tx_start()`, `fm_transmit()` | FM (or AM, `tx_start("am", ...)`) from the TX board (`tx`, mode `fm` or `am`): a program of segments (tones, sweeps, DTMF, AFSK as `fsk` bits, noise, silence) with an optional CTCSS or DCS, at the board's lowest power, within its key limit. A board without the mode in `info`'s `signal_modes` skips as `bench`. |
| `aprs_rx_start()`, `aprs_rx_result()` | the receive board decodes APRS itself (`rx`, mode `fm`, `"aprs": true`): Bell 202 / AX.25 frames with a good FCS, each with its deviation, tone levels and frequencies, twist and TX delay, in `aprs_afsk_check.py`'s JSON shape. A board whose summary has no `aprs` skips as `bench` |
| `can_fm_rx()`, `fm_rx_start()`, `fm_over_start()`, `fm_over_result()` | the receive board's FM demodulator (`rx`, mode `fm`). `fm_over_result()` reads one job as `fm_measure.py`'s report: the keyed segments from its blocks; carrier, deviation, the strongest tone and the CTCSS or DCS from its summary; DTMF from its digit records; tone runs and voice peaks from its audio stream (300-3000 Hz, the audio the board filtered). Samples the board lost (`gap_samples`) skip as `bench`; so does a board without the mode. The channel must be inside the board's receive window |
| `dmr_watch_start(waveform=True)` | the DMR waveform from the board's envelope (`bursts`, `"envelope": true`), measured on the board. Times come from the sample count, since the board can step its UTC mid-transmission. A board without `bursts_envelope` skips as `bench` |
| `need_bursts_cach()` | the bursts' CACH (`bursts_cach`), decoded by `dmr_cach.py`. A board without it skips as `bench` |

FM from the board is what the radio receives in `test_rx_tone_decode_bench` (CTCSS / DCS decode) and
`test_aprs_rx_on_air` (an AFSK frame), on `--fm-mhz`, which must then be a frequency the board may transmit on.

`--bench-api-level 0-255` (or `$RFOUNDRY_BENCH_API_LEVEL`) is the TX board's level for its jobs and FM; without it
the board uses its own. The level spans the board's whole output range, so a low one may be too weak for a receiver
across the bench.

`--dmr-freq` and `--dmr-cc` must be a frequency the board may transmit on, with the radio's channel (or the VFO) on
it: the receive tests don't tune the radio. `test_dmr_repeater_burst_offset` times the radio against the TX board's
outbound, both decoded by the receive board.

### Measurements come from the bench, never as I/Q

No test pulls raw I/Q. What the radio sent is what the bench board decoded and measured.

| Signal | Helper | What comes back |
|---|---|---|
| DMR | `dmr_watch_start()`, `dmr_watch_result()` | one record per burst: `t`, `t_ms`, `slot`, `family`, `kind`, `cc`, `lc`, `foff_hz`, `cach_ts`, `sync_errors`, on a board's BS bursts the decoded `cach`. `waveform=True` adds the edges from the board's envelope. Where the bench can't decode the frequency, the test skips as `bench` |
| FM | `fm_over_start()`, `fm_over_result()` | `fm_measure.py`'s report |
| APRS | `aprs_rx_start()`, `aprs_rx_result()` | the board's frames |

The waveform and CACH tests skip as `bench` on a board without the feature (`bursts_envelope`, `bursts_cach` in its
`info`).

## Rig assumptions

- The radio on USB, with a codeplug that holds a DMR channel on the DMR test frequency in any zone (the tests look
  for it by frequency and mode, the current zone first, and put the radio back on its zone and channel afterwards).
- A bench board reachable over the bench line protocol (`--bench-api`), with the DMR test frequency inside its
  receive window.
- For the TX tests: the radio has a DMR ID, a DMR channel on the test frequency with a talk target, and may
  transmit on `--fm-mhz` in its FM VFO; it runs on its lowest power, into an antenna or a dummy load at bench
  distance.
- An antenna or attenuator so the bench transmitter at power 40 doesn't overload the radio.
- For `test_fm`: the bench board's FM transmitter on `--fm-mhz` and `--vhf-fm-mhz`. A NOAA weather radio station
  audible at the bench (`--noaa-mhz`) runs the optional over-the-air versions.
- Speech recordings for `test_dmr_voice` stay outside the repository; give the path with `--ambe` or
  `$RFOUNDRY_HIL_AMBE`. Never commit a recording.

## Tests

| Module | What | Needs |
|---|---|---|
| `test_health.py` | Any radio's health: display geometry and build features, data flash JEDEC ID and factory calibration, battery reading, a receiver that reads alive, DMR timeslot interrupts, the stock codec, GPS sentences. Each board's numbers come from its `board.toml` `[hil]` table (keys in `rfoundry_hil.HIL_KEYS`, read through `tools/boards.py`); a check the board has no expectation for skips `other_board`, one whose diagnostics line the firmware doesn't print skips `not_built` | radio only; a radio without USB on its programming cable: `--radio-port /dev/cu.usbserial-...` |
| `test_board_hil.py` | The board data the HIL tests read, without a radio: every board's VERSION name maps back to it, the expectations use known keys, the health checks pass, fail and skip on a fake radio as they should, and a board's own test runs only on its radio | nothing |
| `boards/<id>/hil/test_*.py` | A board's own checks, which no other radio has a use for (for example `boards/md380/hil/test_md380_rfbus.py`: the bit-banged RF bus reads back and the synthesizer locks). A run of `tests/hil` runs them too; on another radio they skip `other_board` | radio |
| `test_boot.py` | version / features, ping latency, screenshot geometry (in the packed form), firmware clock vs wall clock (no stalls), view sections, diagnostics clock, error replies (unknown command, bad CRC, oversized request), log ring | radio |
| `test_task_stacks.py` | after every view section and a screenshot, the `tasks` view section: every task has at least 64 bytes of stack left since it started (`RFOUNDRY_HIL_STACK_MIN_FREE` overrides), none untracked, the core's and the kernel's tasks present; prints each task's least free stack. Skipped without the section | radio, firmware with `CONFIG_TASK_STACKS` |
| `test_ptt_host_traffic.py` | Host traffic never makes a PTT press (radios whose PTT pin is also the cable's serial line, [gotchas](../../docs/spec/gotchas.md) 16.2; trivially passes elsewhere): 30 s of polling with gaps of 40 to 120 ms around the 50 ms gate and some requests back to back, TX inhibit on (RAM, put back): `input.ptt_downs` unchanged, the state never `tx` | |
| `test_ui.py` | UP / DOWN change the channel on Home; OK opens the Hub (6 rows), digits open its destinations, BACK goes up, long BACK goes Home from depth 3; Quick settings open and close (BACK and QUICK); digits on Home start Find (`ui.query`); long QUICK locks and unlocks; long # opens Recent; diagnostics through Tools (Hub 5, 7); screenshots `ui_quick.png` and `ui_find.png`; injected keys count as activity; the Quick tiles for now (DMR: slot `dmr.ts` + temp 0x20, color code `dmr.cc` + 0x40, hear all `dmr.hear_all_latch`; FM: monitor latch, VOX `vox_on`), each undone by BACK on Home or a second press; Quick 4 on a DMR channel copies it to the VFO (`vfo`, `mode`, the toast; the channel unchanged, the VFO's own values put back); the picker's focus row (`dmr.focus_id`, cleared by long BACK); Settings long press time (`input.long_ms`, set back) and auto lock 30 s (`ui.locked` after 30 s, set back to Off); opt-in: the timeslot saved to the channel and back (`storage.job_busy`, `temporary`), the zone editor's Move down and Move up on the channel in use (`zone.edits`, `zone.edit_last`, `zone_pos`; the zone ends as it was), deleting the newest message (`$RFOUNDRY_HIL_DELETE_MESSAGE=1`), factory reset with a restart (`$RFOUNDRY_HIL_FACTORY_RESET=1`). State is read from the link section (`ui.screen`, `ui.depth`, `ui.count`, `ui.locked`, `ui.query`, `ui.toast`) | radio; `--allow-codeplug-write`, `--allow-reboot` |
| `test_encoder.py` | The channel knob: injected detents move to the next channel on Home and down a list (forward), back the other way; with `--manual`, asks for 10 clicks each way and a wobble and checks the decoder's counts (`input.enc_detents` = clicks, `input.enc_transitions` = clicks x `input.enc_steps`, `input.enc_illegal` near 0, no count from the wobble) | radio; a zone of 3+ channels; `--manual -s` and a person for the second test |
| `test_vfo_copy.py` | Copy to VFO (`dmr-voice.md` 2.2), nothing transmitted: RADIO op 32 puts the first FM channel of the zone into the VFO (frequencies, mode, bandwidth, tones), the channel stays as it was, a second copy in the VFO is refused, the VFO keeps the values over a channel and back, then its own values are put back | radio with the FM VFO |
| `test_vfo_dmr.py` | the DMR VFO (`dmr-voice.md` 2.1), nothing transmitted: mode, color code, slot and talk target set over the debug link (RADIO ops 31 and 15) show in the `core` / `dmr` views with no temporary flag, survive a channel and back, and the VFO goes back to FM with the defaults | radio with the DMR and FM VFO |
| `test_fm.py` | VFO on NOAA weather: squelch opens or RSSI above the floor within 10 s (receive only); RX tone squelch: with CTCSS 100.0 and then DCS D023N required, untoned NOAA stays muted (`tone_ok=0`, never `rx`), and opens again without the tone (the squelch stepped down until NOAA opens it). A radio whose bands miss NOAA hears the bench board's FM on `--fm-mhz` instead (short keys, `--allow-rf`), printed as a fallback | radio with the FM VFO |
| `test_hotspot.py` | MMDVM GET_VERSION / GET_STATUS answered (client with DTR clear, so its close doesn't end the session), the hotspot takes the modem (slot 2 space, `hs.running`, `hs.starts`), the debug link answers in hotspot mode, hotspot left through the debug link (no transmission) | radio with the hotspot |
| `test_hotspot_sfr.py` | MMDVM SFR on the radio's channel: the version suffix, duplex with RX = TX turns it on (121-byte status, both space bytes 16, `sfr.on`), command 0xD5 ACK / NAK 4, simplex turns it off; sends nothing and expects a quiet channel (`sfr.tx_starts` unchanged). `test_sfr_repeats_bench_call`: the bench sends a TS1-synced call, its header goes up as DMR_DATA1 and the repeat with the bench's LC is on air on TS2 (SDR); then the whole call, heard between the radio's own bursts and repeated (`docs/spec/sfr.md` 3); the SFR keys are in the debug link's `sfr` section | `--allow-rf` (the repeat test also `--allow-service-stop`), firmware with `CONFIG_SFR` |
| `test_sfr_standalone.py` | The codeplug SFR roles without the hotspot, the bench modem as the talker on --sfr-freq (default --dmr-freq): a repeater channel repeats a 4-superframe call (at least 19 bursts, 60 of 72 inbound frames decoded, at most 3 lost, under 4 bit errors a frame; `sfr.rx_fec`, `sfr.rx_fec_pos`); TX inhibit stops the role and it restarts when cleared; the CW ID follows the first call (`dmr.sfr_cwid_sent` or `_cut`); a user channel in receive mode 0 drops the talker's direct copy and in mode 1 plays it (`call.rx_sfr_direct`, `call.rx_voice_bursts`). `test_sfr_chain_rfoundry_user`: RadioFoundry radios only, the --radio-port radio repeating, an ODR talker (`--sfr-talker-port`, keyed with the voice test signal) and an ODR listener (`--sfr-listener-port`) on user channels; per listener receive mode (`--sfr-listen-modes`, default 0,1) the listener plays one call with 90 % of the talker's frames, `voice.rx_peak` at least 1000 and `voice.rx_repeats` at most a tenth of its frames. Skips without a channel of the role on the frequency | `--allow-rf`, SDR bench (the chain test needs none: it takes a bench lease only when one is configured); repeater tests transmit |
| `test_audio_probe.py` | USB audio feasibility measurements ([hardware-dm1701.md](../../docs/spec/hardware-dm1701.md) 21.3): the PCM stream runs on an FM channel; per HR_C6000 setting, whether FM receive audio reaches the I2S link (NOAA on vs a quiet frequency, RMS of both channels); with the bench, whether a 1 kHz PCM tone reaches the FM modulator (voice band deviation, microphone muted). Prints tables, asserts only that they ran; every register touched is restored | firmware with `CONFIG_AUDIO_PROBE=y`; the TX part `--allow-rf --allow-service-stop` |
| `test_usb_network.py` | USB network personality: re-enumerates as CDC-NCM, DHCP gives the host 12.<ID>, ping and the debug link over UDP 3334 at the radio's address, back to serial. Passes on the bench Mac (macOS 26). Firmware before 5a13814 stayed a network adapter when the host never configured it (power it off and on); since then it falls back to serial after 10 s | `--allow-usb-switch`, firmware with `CONFIG_DMR_IP_GATEWAY` |
| `test_tcp_services.py` | The TCP services at the radio's address in network mode (`docs/spec/tcp-services.md`): the HTTP page, `/status` and view JSON, the screen BMP, a 404; KISS over TCP (a foreign-source frame taken by the TNC and refused by the APRS engine; an own-callsign frame on air is in test_aprs.py); rigctld (`f`, `m`, `t`, `get_info`, `+f`, a VFO set 12.5 kHz up and back, `dump_state`, and Hamlib's `rigctl -m 2` where installed); the APRS-IS client with `RFOUNDRY_HIL_APRSIS_SERVER=a.b.c.d[:port]` and NAT on the host. Not run on the bench yet | `--allow-usb-switch`, firmware with `CONFIG_NET_TCP` |
| `test_net_services.py` | the services in USB network mode: mDNS (`rfoundry-<ID>.local` resolves to the radio, `dns-sd -B` / `-L` or python zeroconf find `_rfoundry-debug._udp` on port 3334 with its TXT record), TFTP (`info.txt`, two reads of `codeplug.bin` identical, the current channel's record matches the view); opt-in: a channel renamed over TFTP, read back, the backup written again and verified, a wrong file refused (`docs/spec/mdns.md`, `docs/spec/tftp.md`) | `--allow-usb-switch`, firmware with `CONFIG_MDNS` / `CONFIG_TFTP`; `--allow-codeplug-write` for the write |
| `test_control_station.py` | Control station mode (`docs/spec/usb-network.md` 5): the USB network settings stored (radio op 27) and their view keys; the fixed link (DHCP gives 192.168.10.2, ping and the debug link at 192.168.10.1); booting as the adapter (`usb_mode`); passthrough: an ETSI IP text and a TMS from the bench reach the PC's sockets and not the inbox, no TMS ACK, the PC's datagram goes out from the radio's ID, confirmed data still gets the radio's response. The board needs `data --ip-text` in its `bench_tx` job. Manual follow-up with a dispatch application: usb-network.md 5.6 | radio only for the settings test; `--allow-usb-switch` (and `--allow-reboot` for the boot test, `--allow-rf --allow-service-stop` for the passthrough tests); firmware with `CONFIG_DMR_CONTROL_STATION` |
| `test_snmp.py` | the SNMP agent over the USB network: walks the whole MIB at the radio's link address with `tools/rfoundry_snmp.py`, compares values with the debug link, the SET rules with a temporary write community (restored), a wrong community dropped, the test notification received on UDP 16162; back to serial. Nothing transmitted. Not run yet | `--allow-usb-switch`, firmware with `CONFIG_SNMP` |
| `test_dmr_rx.py` | bench sends 2 group calls (10 superframes, 8 headers, 3 terminators each); counter deltas | `--allow-rf --allow-service-stop` |
| `test_dmr_voice.py` | bench sends a speech recording; every frame reaches the speaker path. `test_vocoder_encode_options` (radio only): the encode options in force follow Mic noise reduction and Mic AGC (`voice.enc_options`, `settings.audio_flags`), and the codec word (`voice.enc_flags`) is 0x800 plus option bits, never 0x100 / 0x4000; it is the word the codec was last given, so one that still belongs to the previous options (no transmission since a change) is only printed | the same, and `--ambe`; radio only for the options test |
| `test_dmr_audio_smoke.py` | DMR audio both ways (marker `audio_smoke`, part of the default bench pass). RX: the bench sends the speech recording to the call the radio's channel listens to (its talk target, or a private call to its ID, on the channel's color code); the decoded audio peaks high and the frames aren't one frame repeated. TX: the radio keys 3 s with the voice layer's test signal in place of the mic (RADIO op 33); the bench modem records the call, the frames on air must be distinct and not silence, and the recording played back to the radio must decode to audio. Prints the mic level of the over (`voice.tx_mic_peak`) | `--allow-rf --ambe`; skips only without the radio, the bench, DMR or `--allow-rf` |
| `test_am_rx.py` | the bench board's AM transmitter (tx op, mode am: a 1 kHz tone at depth 0.3 between bare carrier lead and tail) on `--am-mhz`; the radio's VFO tuned there is received in AM (`CONFIG_AM_RX`): the carrier opens the squelch (`state` rx, or `rssi_dbm` 10 dB over the floor read just before), it closes (`state` idle) after the carrier ends, and the bench did not cut. A board that only sends frequency skips | `--allow-rf --bench-api HOST --am-mhz MHz` |
| `test_dmr_outbound_voice.py` | a bench board sends a repeater's outbound (BS bursts with the CACH on both slots) with voice calls (a `bench_tx` `outbound --voice` job); the radio, on a duplex channel receiving it and never transmitting, shows the call of its own timeslot with the right talker and talkgroup: one call on TS1 or TS2 (the other slot's radio hears none), both slots at once, TS2 starting 1.5 s into a TS1 call, late entry at burst A, D and F with no headers (marked late), and no call when the outbound's color code is wrong; the board's `call` records match what was sent and report no late bursts. A board that can't push voice (`unsupported`) skips | `--allow-rf --bench-api HOST --outbound-mhz MHz` (a duplex channel on it in the codeplug) |
| `test_dmr_tx.py` | radio keys 3 s on DMR (debug-link PTT); the bench's decoded bursts and the bench modem's listener check src ID, TG, CC, headers / voice / terminator, timing, carrier offset (waveform checks reported as not judged when the board's receiver clips); our ETSI GPS Info from a fixed APRS position decoded by the bench modem's listener (`CONFIG_DMR_GPS` and `CONFIG_APRS`); privacy TX (Motorola BP key 1: service option 0x40 in every LC from us, FID 0x10 in the embedded LCs; a clear over has neither); talker alias TX (the channel's alias flag set over CPS, backed up and restored: the alias on air equals `dmr.tx_alias`); vocoder encode options (`test_dmr_tx_encode_options`, one over each for flags 0x1840, 0x800, 0x1800 and 0x3840, set over the debug link's SETTING 31: the same checks, `voice.enc_options` as set, `voice.enc_flags` after the over equal to the word, the options put back; AMBE parameter statistics of what the bench modem heard printed, not judged); repeater TX on a split channel (the channel's TX frequency set over CPS, backed up and restored; the bench modem as a duplex repeater: the radio wakes it, sees it up while talking, and the repeater takes the call) | `--allow-rf --allow-service-stop`; the alias test also `--allow-codeplug-write`; the split test also `--allow-codeplug-write --split-tx-mhz MHz` |
| `test_fm_tx.py` | radio FM VFO keys 3 s with CTCSS 100.0, DCS D023N, and at 12.5 / 25 kHz; the 1750 Hz burst, the roger beep (on and off), DTMF (digits, timing, deviation, twist; a held digit stopped by PTT release and by the time-out) and the three lowest power levels; one over each, measured by the bench board (`fm_over_result()`) | `--allow-rf --allow-service-stop` |
| `test_mic_uart_inject.py` | The DMR mic path without a person: keys a DMR channel on `--dmr-freq` (lowest power, the firmware's own PTT time-out), sends tone bursts on the radio's serial line (0x55 at 6000 baud, a 3 kHz square, 0.2 s on and off, 8 times), which reach the mic input on radios whose programming cable shares it, and checks the vocoder's mic statistics (`voice.tx_mic_level_db` / `tx_mic_spread_db`) against a quiet over: the bursts must stand out. Catches a mic route that never reaches the codec ADC, which the test signal tests can't. Also checks the debug link answers after the odd-baud bytes and the radio isn't left keyed | `--allow-rf --mic-inject`; a local serial port (not a network bridge), nothing else on it; the DM-1801 Pro, DM-32UV and DB25-D, others with `--mic-inject-any` |
| `test_mic_gain.py` | FM mic gain against deviation ([radio-features.md](../../docs/spec/radio-features.md) 3.6): per Mic gain step (`--mic-gain-steps`, default 3,5,7) at 12.5 and then 25 kHz, the step set over the debug link (SETTING 29), then a person unplugs the cable (on the DM-1801 Pro it cuts the microphone), keys, gives a steady tone or the same sentence, releases and plugs back in while the bench board measures (`--mic-gain-window` s); prints the voice peak / rms, the tone deviation and GCR_ADCL per over. Asserts only a keyed segment per over and the step read back ([dm1801pro-bringup.md](../../docs/testing/dm1801pro-bringup.md) 5.1). No injected tone stands in for the microphone: they all join after the codec ADC | `--manual -s --allow-rf`; a board with `hal_radio_t.mic_gain` (`set.mic_gain_board=1`) |
| `test_speaker_ear.py` | The speaker by ear (`--manual -s`): a beep, the FM receiver's noise with the squelch held open, a bench DMR call (`--allow-rf`, the bench modem) and the same call through the second receiver in dual watch (on a radio with one receiver: the call dual watch stops on). The person presses any key except PTT when the sound plays (`input.raw_keys`). Without `--manual`, `--microphone` listens instead (marker `by_ear`): the sound's loudest stretch 10 dB over the room just before it. The volume is set over the debug link and given back |
| `test_cps.py` | open-firmware CPS over USB with `tools/cps_client.py`: radio info (a radio type from the board's `board.toml` `[cps] radio_types`, struct 3), two reads of the qdmr regions identical, the current channel's record matches the view, a channel change saved by the settings store within 2 s (the `storage` view section); the write dry run (no write: backup file round trip, the rename touches only the name, the write and restore plans match and restore every byte); opt-in: one channel renamed, read back and the backup restored over what the radio then holds and verified; the channel position survives a restart | radio; `--allow-codeplug-write` (`--codeplug-backup-dir DIR` keeps the backups), `--allow-reboot` |
| `test_iddb.py` | the DMR ID database ([open-firmware-cps.md](../../docs/spec/open-firmware-cps.md) 7.5 format N, [iddb-compact.md](../../docs/spec/iddb-compact.md) format C): records from its start, middle and end read over the fast codeplug protocol and decoded by `tools/iddb_read.py`, each looked up on the radio (DL_CMD_IDDB) for the same text, IDs between them not found. The radio's own database is the test data when it is in the test's format; otherwise the test skips (`state`), or with `--allow-codeplug-write` the blocks a synthetic database of that format takes (600 made-up records, `tools/iddb_write.py`) are backed up to `--codeplug-backup-dir` (`iddb-N.bak`, `iddb-C.bak`), the synthetic one is written (header block erased first, written last), checked, and the backup written back in a `finally` block and read back byte for byte; after a run stopped midway, `python3 tools/iddb_write.py restore FILE` puts it back. `test_iddb_write.py` checks the encoders against `iddb_read.py` without a radio | radio, firmware with `CONFIG_CPS_FAST` (`CONFIG_IDDB_COMPACT` for format C); `--allow-codeplug-write` for the format the radio doesn't hold |
| `test_crash.py` | the crash log (`docs/spec/crash-log.md`): the `crash` view section, CRASH TEST refused without its token; opt-in: a trap instruction and a failed assertion each restart the radio and leave a record (kind, pc, the host task, the build's commit) the next start shows, then cleared | radio; `--allow-reboot` for the crash tests; firmware with `CONFIG_CRASH_LOG` |
| `test_dmr_call_features.py` | bench sends a private call to us and to another ID, an emergency call, a talker alias, an ETSI GPS Info (after an alias), a late entry (no header, 8 bursts left out), a call with the privacy service option (without and with a key), a call with the PI bit; plus the talk target / privacy key requests (radio only) | `--allow-rf --allow-service-stop` (the last test: radio) |
| `test_speaker.py` | the speaker by microphone ([Speaker tests](#speaker-tests)): every beep, a cold amplifier's key click, the sweep, FM receive tone pulses from the bench board, AM receive of a 1 kHz tone (`test_am_rx_speaker`) | `--microphone`; the FM and AM tests `--allow-rf` and a bench board (AM also `--am-mhz`) |
| `test_audio_check.py` | the speaker tests' analysis (`audio_check.py`) on synthetic recordings: cut starts and ends, a level step, a dropout, pops, a squelch tail, a filled silence, a sweep that breaks up, a broken capture must be found; a room's echo and swell must pass | numpy (tools/ci.sh runs it) |
| `test_fm_measure.py` | `fm_measure.py` on made-up records of a bench board's FM job: keyed segments and drop-outs, carrier, CTCSS and DCS, DTMF twist, tone runs and voice figures from the audio stream, lost samples as a bench fault | none: no radio (`tools/ci.sh` runs it when numpy is installed) |
| `test_radio_features.py` | the `features` view section, settings round trip, beep counters (does PCM play in this mode), zone scan + cancel, nuisance delete, VFO band scan, dual watch and its debug link controls (select A or B, stop), battery save cycling and MONITOR waking it (radio only); TX inhibit, TOT warning and time-out, roger beep, 1750 Hz burst, VOX quiet (no key-up with nobody talking) on the FM VFO at lowest power (`radio_tx`); the mic level (`vox.level`, and on the HR_C7000 radios the `mic lvl` diagnostics line: the meter counts PCM halves) with VOX off (radio only); the zone scan, the VFO scan and dual watch hold on a bench DMR call, battery save wakes for it (`rf`) | radio; `--allow-rf` (radio_tx); `--allow-rf --allow-service-stop` (the bench ones) |
| `test_dmr_emergency.py` | DMR emergency: the bench sends an emergency alarm CSBK and an emergency voice call to the channel's talkgroup (alerts raised); the radio sends its alarm (3 CSBKs) and an emergency call with the hot mic (the bench listens); the emergency settings request (radio only) | `--allow-rf --allow-service-stop` (the first test: radio), firmware with `CONFIG_DMR_EMERGENCY` |
| `test_split_duplex.py` | split duplex (`docs/spec/split-duplex.md` 10): the capability gate (both halves offered); 0xD6 INFO / SET_ROLE / STATUS, unknown sub-command and command answers, the SPLIT debug view (no RF); a standard host that never sends 0xD6 sees the simplex hotspot, the ` SPLIT1` suffix aside (no RF); a split host that dies without closing the port: the 10 s role lease gives the next standard host the normal hotspot (no RF); the TX half keyed past the TX time-out in effect (the radio's set to 5 s for it) with nothing queued, keyed with the CACH counter at the slot rate all along, unkeyed within 1 s of DMR_START 00 (STATUS, no SDR); the TX half keyed 10 s at the lowest power, the receive board's burst records and envelope (BS sync only, 30 ms slots, one key-up edge, CACH decode printed; variant from `$RFOUNDRY_SPLIT_VARIANT`); the RX half labeling bench calls and the label flipping with the epoch | firmware with `CONFIG_HOTSPOT_SPLIT`; the TX and RX tests `--allow-rf --allow-service-stop` (`test_split_tx_half_continuous` `--allow-rf` only) |
| `test_hotspot_cwid.py` | MMDVM SEND_CWID in hotspot mode: the radio keys the callsign in Morse on FM on the hotspot TX frequency (the SDR bench records), then the hotspot's DMR set-up is back | `--allow-rf --allow-service-stop` |
| `test_dmr_data.py` | DMR data: bench sends unconfirmed and confirmed data, a TMS and an ETSI text message, a call alert and radio checks to us (the radio's answers listened for); the radio sends a call alert, a radio check (Motorola and ETSI forms) and a text message (TMS and ETSI) to the bench (the bench modem listens); the data settings request (radio only) | `--allow-rf --allow-service-stop` (the first test: radio) |
| `test_dmr_ip.py` | IP over DMR ([dmr-data.md](../../docs/spec/dmr-data.md) 11, 12): the `dmr_ip` section and the IP settings request (radio only); a build with the gateway has a host netif (`net.netif=1`, radio only); a UDP datagram (50000 to 161) put into the IP gateway's air path by the debug link's IP_SEND, heard by the bench modem and decoded by `tools/bench/dmr_packet.py`: SAP 3 with the 9 octet header in auto, SAP 4 with compression off, SAP 4 in all for an IPv4 destination other than the ID's (IP_SEND's explicit address; the gateway ID round trip, radio only), confirmed to the bench (which doesn't answer) with 2 tries: two transmissions (F set, then clear, the same N(S)), the CRC-9s checked, then failed; a bench board with the confirmed data responder (the line protocol's `mssim` op) answering with an ACK 1, 2 and 6 slots (30, 60, 180 ms) after our data terminator: the radio takes it on the answered try (`ip.result` ok, no timeout on that try; a board without it skips); with `--allow-usb-switch` a datagram from this host over the USB network | `--allow-rf --allow-service-stop` (the first test: radio), firmware with `CONFIG_DMR_IP_GATEWAY`; the responder test `--allow-rf --bench-api HOST` on a board serving `mssim`, `--dmr-freq` one of its transmit channels; the last test also `--allow-usb-switch` |
| `test_dmr_ip_burst.py` | UDP over DMR in bursts and at the air MTU ([dmr-data.md](../../docs/spec/dmr-data.md) 12.6, 12.8; [usb-network.md](../../docs/spec/usb-network.md) 3.2): 5 datagrams back to back from the debug link's IP_SEND, from this host over the USB network (or PPP on the serial host port on a radio without USB networking: pppd, root or passwordless sudo), and from the bench to this host's socket (all sent, none dropped, in order); the view keys `net.mtu net.air_mtu` (radio only); DHCP option 26 on the host's adapter (a warning if the host ignores it; over PPP the interface MTU must be `ppp.mru`); the largest datagram each way (to the air: the link's MTU, SAP 3 and SAP 4, its block count checked; from it: 558 octets delivered, the receiver's 580 reassembled and dropped as too long for the link, `net.up_too_big`), one block more from the air (52 blocks dropped, the next one delivered); on firmware with an air MTU below the link's only, one octet more to the air (refused, counted, nothing on air, the link still works) and the ICMP fragmentation needed for a DF datagram (a raw socket, root) | `--allow-rf --allow-service-stop --allow-usb-switch` by test, firmware with `CONFIG_DMR_IP_GATEWAY`; the bench's UDP mode needs `tools/bench/dmr_bench.py sync`; the ICMP test runs as root |
| `test_roaming.py` | DMR repeater roaming: the `roam` view section, the roaming settings round trip (RAM), the zone flag toggled and back, roam now on the current zone (passive: listens only; the radio ends on one of the sites, no probe sent), lock and roam now ending it (radio only); the wake probe brings up the bench repeater (a `bench_repeater` job) as a second site and the radio moves there (`rf`, `radio_tx`, `bench_service`); the survey settings round trip; the bench repeater keys an idle beacon (a `bench_repeater` job with `--beacon`) and the radio, listening only, learns the beacon window from it as its own site and surveys the other sites in the window, or (slow) finds it from a silent site with a discovery visit and measures it (`rf`, `bench_service`). The zone flag and the roaming settings are put back | radio (the current zone with 2+ repeater channels on one talkgroup for roam now); `--allow-rf --allow-service-stop --roam-rx MHz --roam-tx MHz [--roam-beacon S --roam-beacon-len S]`, firmware with `CONFIG_DMR_ROAMING` |
| `test_aprs.py` | APRS (`docs/spec/aprs.md` 13): the `aprs` / `aprs_set` view sections and a setting round trip, the RAM test callsign and host NMEA as the position (radio only); on air at the lowest power on `--aprs-test-mhz` (never an APRS frequency, no path, the RAM test call `--aprs-call` with `--aprs-ssid`): a position (from the channel and from the FM VFO), a status and a message decoded off air, with deviation, twist and TX delay measured by the bench board; positions over DMR as LRRP and UDT decoded from the bench modem's listener; the debug link keeps answering (gaps under 0.5 s) while a frame goes out, sent over serial, over the USB network and from a KISS client on TCP. Every APRS setting touched is put back | radio; `--allow-rf --allow-service-stop --aprs-call CALL` (`--allow-usb-switch` for the network cases), firmware with `CONFIG_APRS` and the `aprs.tx_power` view key (the AFSK tests skip without it) |

### Speaker tests

`test_speaker.py` records the radio's speaker with a microphone placed at it (`--microphone DEVICE`, an ALSA capture
device for `arecord -D`, or `microphone` in the rig's file) and checks that each sound plays as it was made. Without a
microphone every test skips `opt_in`. The volume knob and the beep level must leave the tones at least 30 dB over the
room's noise; a recording too quiet throughout skips `state`.

| Test | Plays | Needs |
|---|---|---|
| `test_speaker_beeps` | every beep the debug link can ask for, one recording each; the patterns are read from `core/radio/beep.c` | radio |
| `test_speaker_beep_cold` | the key click after the amplifier hang ran out (`beep.warm` 0): the lead-in must cover the amplifier's start | a board with a speaker hang |
| `test_speaker_sweep` | the speaker test's sweep (`BEEP_SWEEP`: 150 Hz to 3.8 kHz over 4 s, after 400 ms of silence); prints the speaker's response in third octaves and fails when the sweep breaks up | radio |
| `test_fm_rx_speaker` | the bench board's FM transmitter on `--fm-mhz`: 0.5 s of carrier, then 1000 Hz 300 ms, 600 Hz 150 ms, 1500 Hz 80 ms and 1000 Hz 500 ms with silences of 200 to 300 ms, the carrier ending with the last tone | `--allow-rf`, a bench board with FM |

`audio_check.check_tones` judges each recording. A tone's edges are where its amplitude rises and falls fastest: the
direct sound's, since a room's echo builds up and dies away slower. Problems:

| Check | Fails when |
|---|---|
| Length | a tone is more than 10 ms shorter than asked (a lost start or end), or more than 40 ms longer (a speaker and a room ring on a little) |
| Level | a tone moves by more than 12 dB over its length, by more than 8 dB within a few ms (a step as an amplifier comes up, a dropout), or two tones at one frequency differ by 12 dB |
| Silence | a silence is more than 10 ms longer than asked, or less than 8 dB under its louder neighbor |
| Pops | broadband energy away from the tones' harmonics more than 20 dB over the recording's floor, or within 10 dB of a tone on it and in the 60 ms after it (at its edges: louder than the tone). In the FM test this includes the squelch opening and closing; a squelch tail after the carrier is named as one when it lasts longer than the core's squelch takes to close (one `RADIO_POLL_MS` poll to see the carrier gone, then `RADIO_SQL_CLOSE_POLLS` x `RADIO_POLL_MS`, plus one 10 ms frame: tail elimination is not done, radio-features.md 3.5). A tail's length is how long it stays within 10 dB of its peak (`audio_check.burst_ms`): a sound that stops falls about that far at once, and the room's echo after it fades for a time set by the room, the volume and the microphone's place, not the radio |

A microphone with its own processing can shorten a tone and let it back up in the silence after it, which no check
on the recording can tell from a fault. Give `--mic-processed 1` for one. `audio_check.check_onsets` then replaces
`check_tones`: the first tone must rise 10 dB out of silence, to 20 dB over its frequency's floor, and each later tone
must be sounding that far over its floor within 15 ms of its place in the sequence. The recording needs only 20 dB of
SNR. Lengths, silences and pops aren't
measured. For a beep, the radio's stream counters stand in: `audio.late` and `audio.dropped` must not grow and
`audio.blocks` must cover the pattern's length. The silences' content comes from the host tests of the beep player.
FM and AM receive have no such counters, so with this option they check starts only. The sweep is unchanged.

`audio_check.check_sweep` judges the sweep. A sweep played whole has its energy at its own frequency and the speaker's
harmonics of it; audio that jumps on its way to the speaker (samples lost, repeated or out of order) spreads energy
to both sides of it at every jump. The room's echoes arrive at the pitch the sweep had a moment ago, below it, so the
check looks above: a frame counts as broken when the energy 150-600 Hz above the sweep (harmonics excluded) is within
30 dB of the sweep's, counted only where the speaker plays the sweep within 25 dB of its loudest. More than 5 % of
the frames broken fails. A whole sweep measured 39-55 dB down, with the microphone close or farther away; a broken
one 5-33 dB. A recording with more than a quarter of its energy above 4.3 kHz, which the radio's 8 kHz audio can't
make, is reported as a capture fault. Steady tones can miss what the sweep finds: a jump every 4 ms makes no step at 250 Hz multiples.

The recordings stay in pytest's temporary directory (the test prints each path). By hand:
`python3 tests/hil/audio_check.py FILE.wav BEEP` (a beep name, `sweep`, or `HZ:MS,...` with `0:MS` a silence). Many
radios mute the speaker while a programming cable is in. On those, `rfoundry_debug.py radio beep sweep 15` (any beep
name; the second number a delay in seconds, 1-255) plays it 15 s later, so the cable can come out first; record across
that time and check the file.

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

`test_dmr_tx`, `test_fm_tx`, `test_radio_features`, `test_hotspot_sfr`, `test_sfr_standalone`: the criteria are in
each module's docstring (`dmr_bench.check_tx` for DMR TX; CTCSS within 0.5 Hz and 200-900 Hz deviation at 12.5
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
to 3 of them with a good CRC (the bench_tx listen job's `crc_ok`, after the FEC) and every one heard must carry exactly
the expected opcode, FID and addresses (fewer than 3 only warns: the bench modem sits next to the radio and is
overloaded even at 50 mW). The text message (no ACK asked) ends `ok` and its data header (to the bench, from us) is
heard; a send the bench didn't hear is repeated, up to 3 sends. An answer to the bench that the bench modem misses only warns: the radio sends it
within a few frames of the bench's last burst, inside the bench modem's TX to RX turnaround.
ETSI forms (`dmr_ext` section): an ETSI text message in: `data.sms_received` 1, `data.etsi_sms_received` 1,
`data.responses_sent` 1, no TMS ACK, inbox entry 0 with format `etsi`. ETSI text out (no response asked): `ok`,
`data.etsi_sms_sent` 1, a data header with DPF 13 and SAP 10 to the bench heard. Radio check in the ETSI form: 3
tries counted, 1 to 3 heard with a good CRC, each with FID 0x00. Voice PTT after an ETSI text message was answered
(`test_ptt_after_etsi_response`, `radio_tx`): `call.tx_calls` 1, `call.tx_denied` 0, `dmr.tx_error=ok`.

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

`test_hotspot_cwid` (DM-1701 or another board with `tx_tone_key`): SEND_CWID with the rig's `--callsign` ACKed, the
GET_STATUS TX flag set while keying and clear after, `hs.cw_sent` +1, `hs.cw_wpm=20`, `hs.cw_last_ms` within 10 %
of 150 + units x 60 + 150 ms (the call's Morse length in dot units at 20 wpm), one keyed segment off air.

Counter names: `Radio.counters()` merges the diagnostics lines (`modem.*` from the DM-1701's HR_C6000 lines, `call.*`
and `voice.*` short names, `audio.*`) with the `dmr_stats` and `hotspot` view sections (full names such as
`voice.rx_frames`, `hs.net_frames`).

### On this bench

The radio can overload the bench's receivers at bench distance, even at 50 mW: the receiver clips and the bench modem
misses bursts or hears them with bit errors. FM
demodulation and the decoded DMR content survive that; burst waveforms and levels don't. Hence: the DMR TX waveform
checks reported as not judged when the receiver clips, and the bench modem's tries counted with a minimum of one. An attenuator between the radio and the bench
(or more distance) would let those checks run fully.

## Writing a test

- Use the `radio` fixture (an `rfoundry_hil.Radio`); the autouse `restore_radio` fixture snapshots the radio's channel or
  VFO, zone, power, squelch and monitor before the test and puts them back afterwards, and releases PTT. A test that
  changes the VFO's frequency wraps that in `with radio.vfo_session():` (the VFO frequency isn't visible while on a
  channel). The session puts the VFO in FM and puts its frequency and mode back afterwards; `vfo_session(mode=None)`
  keeps the mode. The VFO's mode, color code, slot and talk target are its own settings and outlive a power cycle,
  so a test that changes them (Copy to VFO does) puts them back itself.
- Before the first test the session sets auto lock back to Off and the radio's language back to English
  (`settings.lang`, Settings > Display > Language) and says so: tests match the English text in the view (`ui.toast`,
  `ui.banner`, screen texts). A test that changes the language puts it back itself.
- Drive the radio with `radio.press("up")`, `radio.long_press("back")`, `radio.link.key_hold("monitor", 2000)`,
  radio requests (`radio.link.vfo_set(hz)`, `radio.link.zone_pos(n)`, ...) and `radio.ptt_for(seconds)` (at most 10 s;
  the firmware releases PTT by itself at its time-out). Call `radio.go_home()` first for UI tests.
- Check state with `radio.wait_for(lambda v: ..., timeout)` on the view (key=value strings, `docs/spec/debuglink.md`
  3.7), never with fixed sleeps alone; counters with `radio.reset_counters()` then `radio.counters()` deltas.
- Anything that transmits gets `@pytest.mark.rf` (the radio: `radio_tx`, also when it only replies or beacons by
  itself: [Transmit frequencies](#transmit-frequencies)); anything that uses the SDR bench's
  transmitter or receiver also gets `@pytest.mark.bench_service`, one of `sdr_rx` / `bench_modem` / `sdr_or_modem`
  (both of the first two for a test that needs both, see Bench hardware markers) and runs inside
  `with sdr_bench.lease():` (`transmit` / `voice_call`, `listen_start` / `listen_result`, `dmr_watch_start` /
  `dmr_watch_result` for DMR bursts, `fm_over_start` / `fm_over_result` for FM, `aprs_rx_start` / `aprs_rx_result`
  for APRS).
- A job's files go in a run directory on the bench (`sdr_bench.run_dir(tag)`). The `sdr_bench` fixture
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
