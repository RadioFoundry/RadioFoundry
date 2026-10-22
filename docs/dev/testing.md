<!-- SPDX-License-Identifier: MIT -->

# Testing

From a laptop with no radio to a radio on the SDR bench. The commands, flags and pass criteria are in
[docs/testing/README.md](../testing/README.md) and [tests/hil/README.md](../../tests/hil/README.md); this page says
what each level is for and where a new test goes.

| Level | Runs on | Command | Catches |
|---|---|---|---|
| Unit | the host | `make -C tests/host` | Wrong math, encoders, parsers, the settings format |
| Software in the loop | the host | `make -C tests/host` | Whole features against simulated chips: calls, data, hotspot, CPS, storage, the UI |
| Hardware in the loop | a radio on USB | `python3 -m pytest tests/hil -v -rs` | The real chips and timing; with flags, transmit and receive on air |
| Manual bench cases | the SDR bench and a person | `tools/bench/dmr_bench.py run CASE` | What can't be scripted yet ([dmr-bench.md](../testing/dmr-bench.md)) |
| Release gate | the host | `JOBS=8 tools/ci.sh` | Everything above that needs no radio, plus every board's feature matrix with zero warnings, doc links and the license checks |

`tools/ci.sh` must pass before every merge ([Contributing](contributing.md)); what it runs step by step is in
[docs/testing/README.md](../testing/README.md) section 1. The CI workflows run its pieces per push and nightly
([CI](ci.md)).

| Also | Command | Details |
|---|---|---|
| Coverage | `tools/coverage.sh` (needs gcovr) | `build/coverage/index.html`; [CI, Coverage](ci.md#coverage) |
| Sanitizers | `make -C tests/host clean && make -C tests/host SANITIZE=1` | ASan and UBSan; any report fails the test. `ODR_CI_SANITIZE=1 tools/ci.sh` adds it to the host step |
| Fuzzing | `ODR_FUZZ_ITERS=1000000 make -C tests/host SANITIZE=1` | Seeded mutation loops over the parsers of untrusted input (`tests/host/fuzz.h`); `ODR_FUZZ_SEED` varies the seed |

## Host tests

`tests/host/` holds one `*Test.c` per area and the simulators they share:

| File | Simulates |
|---|---|
| `sim/dmr_sim_modem.c` | A DMR modem: plays bursts into the call stack and records what it sends |
| `sim/sim_flash.c` | SPI flash, for the codeplug, the settings ring and the journal |
| `sim/ui_sim.c`, `sim/ui_png.c` | The UI's display and keys, with a script language and PNG output |
| `sim/split_sim_modem.c`, `sim/splitSim.c`, `sim/splitPairTest.py` | Two radios' cores in lockstep virtual time, for the split duplex repeater |
| `cpsSim.c` | The radio's side of the CPS protocol, for `tools/cps_client.py` and `cpsClientTest.py` |
| `snmpPeer.c` | An SNMP manager, for the agent's end-to-end test |
| `fuzz.h` | The seeded mutation fuzzer the parser tests use |
| `stubs/` | Stand-ins for services a test doesn't exercise |

The Makefile groups the tests into `run-*` targets (`run-core`, `run-ui`, `run-call`, `run-mmdvm`, `run-settings`,
`run-cps`, ...); `make -C tests/host` runs them all. Each test compiles the firmware's own sources with the host
compiler, so a test exercises the code that ships. Test vectors come from the standards or are recorded from real
radios and the stock codec, never from other firmware.

To add a test: write `tests/host/<area>Test.c`, add its rule and a `run-<area>` target to `tests/host/Makefile`, and
hook it into `run` (`run: run-<area>`). Groups are named by the area or directory they test, never by a radio or
platform. `make -C tests/host check-run` (part of `run`) fails when a test in `TESTS` has no `./<test>` line in any
group; a program a test script drives (a simulated peer) goes in `HELPERS` instead. Build without warnings; ci.sh
fails on any. A parser of bytes from outside (USB, the air, the network) also gets a fuzz loop with `fuzz.h`.

A test of one board's own code (its calibration reader, its pin tables, its first stage) lives with the board, in
`boards/<b>/tests/`. Its `host.mk` adds the test to `TESTS` and to the `run-boards::` target. `tests/host` includes
every `boards/*/tests/host.mk`; a fragment names its own files through `HERE := $(dir $(lastword $(MAKEFILE_LIST)))`
in its prerequisites, and its recipes use `$<` and `$^`, since `HERE` changes with the next fragment. The binaries are built in `tests/host/` like the rest.
The firmware build only compiles the `*.c` directly in a board's directory, so these never reach an image. A driver
or platform test stays in `tests/host/`, named by what it tests, even when one radio needed it first.

## The UI simulator and the gallery

The UI runs unchanged on the host. Three tools use it:

| Command | Does |
|---|---|
| `make -C tests/host run-ui` | `gfxTest` (drawing) and `uiFlowTest`: replays every task flow of [task-flows.md](../ux/task-flows.md) against the press budgets of [usability-tests.md](../ux/usability-tests.md) on four display and key map pairs, and crawls every screen for the rules in [ui.md](../spec/ui.md) 7 |
| `tests/host/uiSim --geom 160x120i5 --script "ok 4" --out f.png` | Renders one screen after a key script. The options are in the header comment of `tests/host/uiSim.c`, the script tokens in `tests/host/sim/ui_sim.c` |
| `make -C tests/host ui-gallery` | Renders every gallery screen at every display class into `docs/ux/screens/<class>/`, and one contact sheet per class into `docs/ux/screens/sheets/<class>.png` |
| `make -C tests/host ui-sheets [GEOM=128x64]` | The contact sheets only, into `tests/host/out/sheets/` (not committed): every gallery screen at 2x under its name on one page, to audit a layout change at a glance. `GEOM` takes any geometry `--geom` does |

The display classes are `160x120i5` (160x120 with 5 px hidden left and right, as on the DM-1701), `160x128`,
`240x320i10x5` (the DM-32: 16 colors, anti-aliased fonts one size up, 10 px hidden left and right and 5 px top and bottom),
`128x64` and `240x320`; [the wireframes README](../ux/wireframes/README.md) says which radio uses which.
The user manual uses the `160x120i5` images.

After a UI change, run `ui-gallery` and commit the PNGs with the change, so the review shows the difference. The
gallery target deletes and rewrites `docs/ux/screens/*/`; commit only what you meant to change. Commit the contact
sheet of each class whose screens changed. A new screen gets an
entry in the `gallery[]` table at the top of `tests/host/uiSim.c`.

## Hardware in the loop

The HIL tests drive a radio through the [debug link](debug-link.md): they press keys, read the radio's state and take
screenshots. Each test leaves the radio as it found it. Without a radio they all skip.

Opt-in flags allow what changes the radio or the air around it:

| Flag | Allows |
|---|---|
| none | Radio-only tests. Nothing transmits |
| `--allow-rf` | Transmitting, from the radio or the SDR bench |
| `--allow-service-stop` | Stopping the SDR bench's gateway service to borrow its modem and SDR (always restarted) |
| `--allow-usb-switch` | Switching the radio to USB network mode and back |
| `--allow-codeplug-write` | Writing the codeplug over CPS (backed up and restored) |
| `--allow-reboot` | Restarting the radio |

The full list, the options and the rig assumptions are in [tests/hil/README.md](../../tests/hil/README.md);
"Writing a test" there has the fixtures.

A skip is not a pass. Every skip names its kind, and a bring-up run (`tools/bringup.py run --radio <id>`) runs a
radio's feature checklist, fails a required test that skips for a reason its feature doesn't declare, and records the
result in `docs/testing/results/` ([tests/hil/README.md](../../tests/hil/README.md#bring-up-mode)).

## The SDR bench

A second station for on-air tests: a DMR modem, an RTL-SDR and the bench scripts in `tools/bench/`. It sends calls,
texts and data to the radio and checks what the radio transmits (IDs, color code, slot timing, ramps, carrier offset,
FM tones and deviation). Its host and paths come from `ODR_SDR_BENCH_*` environment variables, never from files in
the repository. Setup and the manual cases: [docs/testing/dmr-bench.md](../testing/dmr-bench.md).

A bug found on the bench should end as a host test, so it stays fixed without the bench.
