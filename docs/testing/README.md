<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Testing overview

This page tells you how to run each level of RadioFoundry testing, from the host tests to a radio on the SDR bench.
Each level is repeatable, and each skips what it can't reach. For what each level is for and where a new test goes,
see [Testing](../dev/testing.md).

| Level | Needs | Command | Details |
|---|---|---|---|
| Release gate | A Mac or Linux host with the Arm toolchain | `JOBS=8 tools/ci.sh` | [Run the release gate](#run-the-release-gate) |
| Coverage, sanitizers, fuzzing | The host; gcovr for coverage | `tools/coverage.sh`, `make -C tests/host SANITIZE=1` | [Coverage](../dev/ci.md#coverage) |
| User interface | The host | `make -C tests/host run-ui` | [Test the user interface without a radio](#test-the-user-interface-without-a-radio) |
| Hardware in the loop, radio only | A radio on USB | `python3 -m pytest tests/hil -v -rs` | [Test on a radio](#test-on-a-radio) |
| Hardware in the loop, on air | A radio and the SDR bench | The same command with opt-in flags | [Test on the air](#test-on-the-air) |
| Manual bench cases | The bench board and a person at the radio | `tools/bench/bench_line.py HOST run bench_tx ARGS` | [DMR bench cases](dmr-bench.md) |
| Transmit power | A radio, a wattmeter, and a dummy load | `tools/power_cal.py` | [Power calibration](power-calibration.md) |
| Backlight | A radio built with `CONFIG_BACKLIGHT_TEST=y` | On the radio | [Backlight curve](backlight-curve.md) |

## Run the release gate

`tools/ci.sh` runs everything a pull request must pass that needs no radio: the host tests, the Python checks, every
board's feature matrix with zero warnings, and the license checks.

1. Run the script from the top of the tree:

   ```sh
   JOBS=8 tools/ci.sh
   ```

2. Read the last line. A clean run ends with `all checks passed`.
3. Remove `build/ci` when you move on to other work.

For the steps, the options, and the build configurations, see
[Running the pieces locally](../dev/ci.md#running-the-pieces-locally). For the version string that each image
carries, see [The firmware version](../dev/building.md#the-firmware-version).

## Test the user interface without a radio

The UI runs unchanged on the host. `make -C tests/host run-ui` replays every task flow against its press budget and
crawls every screen. After a UI change, run `make -C tests/host ui-gallery` and commit the PNGs you meant to change.
For the simulator, the gallery, and the display classes, see
[The UI simulator and the gallery](../dev/testing.md#the-ui-simulator-and-the-gallery).

## Test on a radio

The hardware-in-the-loop (HIL) tests drive a radio over its debug link. Without flags, nothing transmits.

1. Flash firmware built with `CONFIG_DEBUGLINK`, which is on by default.
2. Connect the radio to the host over USB.
3. Run the radio-only tests:

   ```sh
   python3 -m pytest tests/hil -v -rs
   ```

4. Read the skip reasons. Each starts with its kind, such as `[opt_in]` or `[bench]`. A skip isn't a pass.

For the flags that let a test change the radio (`--allow-codeplug-write`, `--allow-reboot`, `--allow-usb-switch`),
the options, and every test file, see [Hardware-in-the-loop tests](../../tests/hil/README.md).

## Test on the air

On-air tests need the SDR bench and `--allow-rf`. A test runs only when the run allows everything it needs.

1. Give the rig's values (test frequencies, color code, talkgroup, callsign, the transmit frequency list) on the
   command line or in the rig's `hil.toml` file. For the keys, see
   [The rig's configuration](../../tests/hil/README.md#the-rigs-configuration).
2. Give the bench board with `--bench-api HOST[:PORT]` or `RFOUNDRY_BENCH_API`.
3. Run the default bench pass:

   ```sh
   python3 -m pytest tests/hil -v -rs -s --allow-rf --ambe SPEECH.ambe --dmr-freq MHZ
   ```

4. Check that the output has no `DMR AUDIO NOT CHECKED` skip.

For when to run the default bench pass, the opt-in flags, and the pass criteria, see
[Default bench pass](../../tests/hil/README.md#default-bench-pass).

## Find what was checked on each radio

These sources say which features have HIL tests and what the last run found:

| Source | Holds |
|---|---|
| [Feature matrix, Checked on each radio](../feature-matrix.md#checked-on-each-radio) | Each feature's status on each radio, generated from the bring-up records |
| [`tests/hil/features.toml`](../../tests/hil/features.toml) | The feature checklist: each feature's switch, spec, and manual steps. Each test names its feature with a marker |
| [Bring-up records](results/README.md) | The run records, the seed records, and how a status is derived |
| `tools/bringup.py status` | Every radio and feature, with the status of each |

To record a run on a radio, use `tools/bringup.py run --radio <id>`. For details, see
[Bring-up mode](../../tests/hil/README.md#bring-up-mode).

## Split duplex without hardware

`make -C tests/host` runs the split duplex repeater's host tests:

| Test | Checks |
|---|---|
| `splitDuplexTest` | The CACH and short LC against the standard's tables, the outbound buffer, the epoch, and both roles through the MMDVM core |
| `hrc6000BsTest` | The HR_C6000 BS outbound engine on a simulated chip |
| `sim/splitPairTest.py` | Two radios' cores in lockstep virtual time with drift, a clock wrap, and USB jitter, and a repeated call end to end |

For the bench steps, see [Split duplex, Tests](../spec/split-duplex.md#10-tests).
