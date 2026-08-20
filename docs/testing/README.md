<!-- SPDX-License-Identifier: MIT -->

# Testing

How to run every test of OpenDigitalRadio, from the host tests to the radio on the SDR bench. Three tiers, each
repeatable and each skipping cleanly what it can't reach:

| Tier | Needs | Command | Where it is described |
|---|---|---|---|
| Release gate (host tests, build matrix, license checks) | a Mac or Linux host with the ARM toolchain | `tools/ci.sh` | this page, section 1 |
| Hardware in the loop, radio only | the radio on USB | `python3 -m pytest tests/hil -v -rs` | `tests/hil/README.md` |
| Hardware in the loop, on air | the radio and the SDR bench (and the RadioDesk sandbox for one test) | the same with opt-in flags | `tests/hil/README.md`, section 3 here |
| Manual bench cases | the SDR bench, a person at the radio | `tools/bench/dmr_bench.py run CASE` | `docs/testing/dmr-bench.md` |

## 1. The release gate: `tools/ci.sh`

```
tools/ci.sh                    # everything below
tools/ci.sh --quick            # each board's default build only (no feature matrix)
tools/ci.sh --board dm1701     # one board
JOBS=8 tools/ci.sh             # parallel make jobs (default: the CPU count)
```

It runs, and reports each as ok / FAIL / skip (exit status 0 only when nothing failed):

1. **Host tests**: `make -C tests/host` (the same as `make test`), built with no warnings. They need only the host's
   C compiler. Their binaries, `.dSYM` bundles and outputs stay in `tests/host/` and are ignored by git
   (`tests/host/.gitignore` tracks only sources); `make -C tests/host clean` removes them.
2. **Python**: every tool and HIL test parses, and pytest collects `tests/hil` (skipped without pytest:
   `pip install pytest pyserial`).
3. **Firmware, every board with the feature matrix**. For each `boards/<b>/board.mk` whose compiler is installed:
   - `default`: the board's defaults;
   - `no-<FEATURE>`: every switch that is on by default, turned off alone (its sub-features follow it,
     `docs/build-config.md`);
   - `fm-minimal`: FM only, without DMR, USB, VFO, diagnostics and sub-audio tones;
   - `maximal`: every switch on.

   Every configuration must build with zero compiler and linker warnings. A board directory without `board.mk` (the
   DM-32 until its platform lands) or without its compiler (the DM-32's csky GCC comes from
   `tools/build-csky-toolchain.sh`) is skipped with a notice. Build trees are `build/ci/<board>-<config>/`, each
   with its log next to it. The sizes go to `build/ci/size-report.md`: image bytes, the change against the board's
   default, and the space left in the linker's application region (the codec windows lie outside it, so "free" is
   what code can still grow into).
4. **Licenses**: `tools/license_grep.sh` (markers of code that must not be here, `docs/CLEANROOM.md`) and
   `reuse lint`. Without `reuse` installed the lint is skipped with a notice; install it with `pipx install reuse`
   (or `pip install --user reuse`).
5. **Similarity scan**, only when `ODR_SIMILARITY_REFS` names the reference trees (space separated). The reference
   trees are never part of this repository or of public CI; the project lead runs it against the legacy trees
   before a release:

   ```
   ODR_SIMILARITY_REFS="/path/to/legacy-tree /path/to/other-tree" tools/ci.sh
   tools/similarity_scan.py /path/to/legacy-tree                    # the scan alone, every region with lines
   tools/similarity_scan.py --ids --report scan.txt /path/to/tree   # also finds copies with renamed identifiers
   ```

   Each reported region gives our file and lines and the reference file and line it shares a run of 25 tokens with.
   Every region is reviewed by hand; `#include` blocks and generic idioms are expected noise. Options:
   `tools/similarity_scan.py --help`.

### The firmware version

The image carries the commit it was built from (`ODR_GIT`, shown on the splash screen, in the debug link's VERSION
and in the MMDVM GET_VERSION description) with `-dirty` appended when tracked files differ from that commit or untracked
sources sit in the built directories. The build prints it on the image line (`image ...: N bytes, version
c1544f29-dirty`). An image for anyone else is built from a clean, committed tree.

### The UI without a radio

The UI's host tests (`make -C tests/host run-ui`, part of the host tests above) replay every task flow of
`docs/ux/task-flows.md` against the press budgets of `docs/ux/usability-tests.md` on four display and key map pairs,
and crawl every screen for the rules that hold everywhere (`docs/spec/ui.md` 7). `make -C tests/host ui-gallery`
renders every screen at every display class into `docs/ux/screens/`; run it after a UI change and commit the PNGs
with it, so the review shows what changed. `tests/host/uiSim` renders one screen after a key script, to PNG.

## 2. Hardware in the loop, radio only

```
python3 -m pytest tests/hil -v -rs
```

The radio on USB with firmware built with `CONFIG_DEBUGLINK` (the default). Nothing transmits: version and features,
screenshots, injected keys, FM receive on NOAA weather, MMDVM requests. Without a radio every test is skipped with
the reason. Details, rig assumptions and pass criteria: `tests/hil/README.md`.

CPS and settings (`test_cps.py`): the radio-only tests read the codeplug twice over the legacy CPS protocol and
compare, and check a channel change is saved. Two opt-in flags change the radio but don't transmit:

```
python3 -m pytest tests/hil -v -rs -s -k cps --allow-codeplug-write   # backs up, renames a channel, restores
python3 -m pytest tests/hil -v -rs -s -k cps --allow-reboot           # channel position survives a restart
```

The write test saves the backup in its tmp directory and prints the path. If the restore fails, the path and the
`tools/cps_client.py write` command are in the failure message.

## 3. Hardware in the loop, on air (opt-in)

Each flag allows one more kind of action, and a test runs only when all it needs is allowed:

| Flag | Allows |
|---|---|
| `--allow-rf` | anything that transmits: the SDR bench's modem, the radio itself, RadioDesk's gateway |
| `--allow-service-stop` | stopping the SDR bench's gateway service (always restarted by a bench-side guard) to use its modem and RTL-SDR |
| `--allow-radiodesk` | publishing to the RadioDesk sandbox broker |

```
python3 -m pytest tests/hil -v -rs -s --allow-rf --allow-service-stop                  # DMR RX / voice, DMR TX, FM TX
python3 -m pytest tests/hil -v -rs -s --allow-rf --allow-service-stop -k "dmr_tx or fm_tx" --sdr-ppm 0.7
python3 -m pytest tests/hil -v -rs -s --allow-rf --allow-service-stop -k dmr_data   # DMR data services
python3 -m pytest tests/hil -v -rs -s --allow-rf --allow-radiodesk -k radiodesk         # needs $ODR_RADIODESK_MQTT_*
```

| Test | What is checked |
|---|---|
| `test_dmr_rx`, `test_dmr_voice` | the bench modem sends calls; the radio's counters show every burst, every voice frame reaches the speaker path |
| `test_dmr_tx` | the radio keys 3 s; an SDR capture (`sdrmon.py`) and the bench modem's listener check source ID, talkgroup, color code, headers / voice / terminator, slot timing, ramps, carrier offset |
| `test_fm_tx` | the radio's FM VFO keys with CTCSS 100.0 Hz, DCS D023N, at 12.5 and 25 kHz; `tools/sdr_fm_check.py` decodes the tones and compares the deviations |
| `test_dmr_data` | the bench sends data, a text message, a call alert and radio checks to the radio, which answers (its `dmr_data` counters, the answers heard by the bench modem); the radio sends a call alert, a radio check and a text message the bench modem hears (`docs/spec/dmr-data.md`) |
| `test_radiodesk_alert` | a sandbox alert (MQTT `ESS/RadioDesk/Alert/<radio id>/voice`) becomes a private call; the radio shows and plays it; RadioDesk's `AlertStatus` reports |

The SDR bench's names come from the environment: `ODR_SDR_BENCH_HOST` (ssh host or alias, default `sdr-bench`),
`ODR_SDR_BENCH_SERVICE`, `ODR_SDR_BENCH_DIR`, `ODR_SDR_BENCH_PY` (`docs/testing/dmr-bench.md` section 1). The
RadioDesk sandbox credentials come only from `ODR_RADIODESK_MQTT_HOST`, `_PORT`, `_USER`, `_PASSWORD` and `_TLS`;
they are never written into the repository.

## 4. Manual bench cases

`docs/testing/dmr-bench.md`: the DMR test cases that need a person at the radio (talking, reading the display), run
end to end by `tools/bench/dmr_bench.py` with automatic checks where possible; `--dry-run` prints every command
first. `docs/testing/mmdvm-audit.md`: the MMDVM protocol audit of the hotspot.
