<!-- SPDX-License-Identifier: MIT -->

# Testing

Four levels, from a laptop with no radio to a radio on the SDR bench. The commands, flags and pass criteria are in
[docs/testing/README.md](../testing/README.md) and [tests/hil/README.md](../../tests/hil/README.md); this page says
what each level is for and where a new test goes.

| Level | Runs on | Command | Catches |
|---|---|---|---|
| Unit | the host | `make -C tests/host` | Wrong math, encoders, parsers, the settings format |
| Software in the loop | the host | `make -C tests/host` | Whole features against simulated chips: calls, data, hotspot, CPS, storage, the UI |
| Hardware in the loop | a radio on USB | `python3 -m pytest tests/hil -v -rs` | The real chips and timing; with flags, transmit and receive on air |
| Manual bench cases | the SDR bench and a person | `tools/bench/dmr_bench.py run CASE` | What can't be scripted yet ([dmr-bench.md](../testing/dmr-bench.md)) |

`tools/ci.sh` runs the host tests, checks the Python, builds every board's feature matrix with zero warnings and runs
the license checks. It must pass before every merge. See [Contributing](contributing.md).

## Host tests

`tests/host/` holds one `*Test.c` per area and the simulators they share:

| File | Simulates |
|---|---|
| `sim/dmr_sim_modem.c` | A DMR modem: plays bursts into the call stack and records what it sends |
| `sim/sim_flash.c` | SPI flash, for the codeplug, the settings ring and the journal |
| `sim/ui_sim.c`, `sim/ui_png.c` | The UI's display and keys, with a script language and PNG output |
| `cpsSim.c` | The radio's side of the CPS protocol, for `tools/cps_client.py` and `cpsClientTest.py` |
| `stubs/` | Stand-ins for services a test doesn't exercise |

The Makefile groups the tests into `run-*` targets (`run-core`, `run-ui`, `run-call`, `run-mmdvm`, `run-settings`,
`run-cps`, ...); `make -C tests/host` runs them all. Each test compiles the firmware's own sources with the host
compiler, so a test exercises the code that ships. Test vectors come from the standards or are recorded from real
radios and the stock codec, never from other firmware.

To add a test: write `tests/host/<area>Test.c`, add its rule and a `run-<area>` target to `tests/host/Makefile`, and
hook it into `run`. Build without warnings; ci.sh fails on any.

## The UI simulator and the gallery

The UI runs unchanged on the host. Three tools use it:

| Command | Does |
|---|---|
| `make -C tests/host run-ui` | `gfxTest` (drawing) and `uiFlowTest`: replays every task flow of [task-flows.md](../ux/task-flows.md) against the press budgets of [usability-tests.md](../ux/usability-tests.md) on four display and key map pairs, and crawls every screen for the rules in [ui.md](../spec/ui.md) 7 |
| `tests/host/uiSim --geom 160x120i5 --script "ok 4" --out f.png` | Renders one screen after a key script. The options are in the header comment of `tests/host/uiSim.c`, the script tokens in `tests/host/sim/ui_sim.c` |
| `make -C tests/host ui-gallery` | Renders every gallery screen at every display class into `docs/ux/screens/<class>/` |

The display classes are `160x120i5` (DM-1701, 5 px inset), `160x128`, `128x64` and `240x320`. The user manual uses
the `160x120i5` images.

After a UI change, run `ui-gallery` and commit the PNGs with the change, so the review shows the difference. The
gallery target deletes and rewrites `docs/ux/screens/*/`; commit only what you meant to change. A new screen gets an
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
| `--allow-radiodesk` | Publishing to the RadioDesk sandbox broker |

The full list, the options and the rig assumptions are in [tests/hil/README.md](../../tests/hil/README.md);
"Writing a test" there has the fixtures.

## The SDR bench

A second station for on-air tests: a DMR modem, an RTL-SDR and the bench scripts in `tools/bench/`. It sends calls,
texts and data to the radio and checks what the radio transmits (IDs, color code, slot timing, ramps, carrier offset,
FM tones and deviation). Its host and paths come from `ODR_SDR_BENCH_*` environment variables, never from files in
the repository. Setup and the manual cases: [docs/testing/dmr-bench.md](../testing/dmr-bench.md).

A bug found on the bench should end as a host test, so it stays fixed without the bench.
