<!-- SPDX-License-Identifier: MIT -->

# Adding a radio

A new radio is a new `boards/<b>/` directory, drivers for any chip the tree doesn't have yet, and a platform if its SoC
is new. The core doesn't change. If it seems to need a change, the difference belongs in `board_t` or a `hal_*`
interface instead, used through a field that other radios leave at its default.

Before writing code, the facts go into a hardware spec, `docs/spec/hardware-<b>.md`, written by the spec side under
the [clean-room rules](contributing.md#clean-room). Use [hardware-dm1701.md](../spec/hardware-dm1701.md) and
[hardware-dm32.md](../spec/hardware-dm32.md) as the model: MCU and memory map, pins, chips, keys, display, power,
bootloader and image format, calibration, codec location. Start from the radio's entry in
[radios.md](../spec/radios.md): its chip list, sources, confidence tags and which RadioFoundry drivers it can reuse.

## The files

A board's files list only how it differs from its platform. The platform's defaults are in `platform/<p>/`:
`platform.mk` (compiler, part defines, HAL modules), `config.mk` (feature defaults), `common.ld` (the sections) and
`FreeRTOSConfig.h`. Glue that every radio with the same part on that SoC shares is in a subdirectory the board lists
in `BOARD_DIRS`. The board then keeps only its wiring and what its chips leave to it.

| Subdirectory | For |
|---|---|
| `platform/stm32f4/hrc` | an HR_C6000 or HR_C5000: DMR glue, I2S audio, the chip's ports, the stock codec host |
| `platform/stm32f4/spi_flash` | the data flash on SPI NOR, with its security registers. A flash that shares its bus with another device takes the board's bus lock (`bus_take`, `bus_give`) instead of the driver's own mutex |
| `platform/stm32f4/fsmc_keypad` | a key matrix on the LCD data lines |
| `platform/stm32f4/gps_uart` | a GPS module on a USART |

The board's own files:

| File | Contents |
|---|---|
| `boards/<b>/board.mk` | `PLATFORM`, `APP_BASE` (where the image starts: the first byte the bootloader runs), `BOARD_DIRS` (the board directory and each driver it uses) and any `MCU_DEFS` of its own. STM32: `MCU` (the part, which picks the CMSIS define and start-up file) and `HSE_VALUE` (the crystal in Hz); optional `BOARD_HAL_MODULES` (HAL modules beyond the platform's, such as `hal_i2c`) |
| `boards/<b>/board.toml` | The board registry entry: identity, status, flashing, codec route, the site's data. `tools/boards.py check` validates it against `board.mk`, the linker script and `board_t`. Every key is in [The board registry](board-registry.md). Copy a neighbor on the same platform |
| `boards/<b>/config.mk` | Feature defaults where this radio differs from `platform/<p>/config.mk`, with `?=` (for example `CONFIG_USB ?= n` on a radio without USB, `CONFIG_DISPLAY_INSET`), each with the reason. Also `BOARD_UNSUPPORTED` (switches its hardware can't do, one a line with the reason: `BOARD_UNSUPPORTED += AM_RX  # no air band receiver`), `BOARD_NOT_WRITTEN` (the same form: the hardware can, the driver or glue isn't written yet, with `CONFIG_X ?= n`; ci.sh's maximal build leaves both lists off, [the rule](../build-config.md#unsupported-not-written-unconfirmed)), `BOARD_REQUIRED` (switches it can't build without; ci.sh skips their `no-` build) and `MAXIMAL_VARS` (make variables for that build, e.g. `LTO=1`) |
| `boards/<b>/memory.ld` | The memory map: the codec symbols, then `MEMORY` (the application's `FLASH` region from `__app_start`, which the Makefile defines from `APP_BASE`, and the RAM regions), then `INCLUDE common.ld`, the platform's sections. Keep the region's end below the codec. Keep the application out of every flash sector the codec touches (not only the codec bytes), so rfoundry can update the firmware without erasing the codec. The flashing tools read the region and the codec symbols from it, and derive the erasable sectors and the protected range ([release.md](release.md#sector-maps)) |
| `boards/<b>/freertos_board.h` | Optional: RTOS settings that differ from `platform/<p>/FreeRTOSConfig.h` (the heap size, for example), read before its defaults |
| `boards/<b>/board.c` | The `board_t` descriptor, the key map, bands, power level names |
| `boards/<b>/pins.h`, `<b>.h` | Pin assignments and the board's private declarations. The registry reads the pins the flash stub uses from `pins.h` (`[flash.stub]` in [The board registry](board-registry.md#boardtoml)) |
| `boards/<b>/*_<b>.c` | One file per interface: radio, DMR, audio, vocoder, display, input, flash, power, calibration, diagnostics, USB |

Look at `boards/dm1701/` (STM32F405, HR_C6000, AT1846S, USB), `boards/md380/` (the same platform with an HR_C5000,
GPS, link time optimization to fit), `boards/dm32/` (HR_C7000 SoC, FD6818, UART host link) and `boards/db25d/` (the
same SoC with two BK4819).

## board_t

[hal/board.h](../../hal/board.h) defines it. The fields:

| Field | Fill with |
|---|---|
| `name` | The model as shown in About and the hotspot's description |
| `caps` | `CAP_KEYPAD`, `CAP_ENCODER`, `CAP_GPS`, `CAP_DMR`, `CAP_VOLUME_POT`, `CAP_AM_RX`, `CAP_BCAST_FM`, `CAP_NO_BATTERY` (a mobile on a fixed supply: no battery UI, no battery monitor; leave `battery_mv` and `battery_pct` NULL, and set `hal_power_t.supply_mv` if the radio measures its DC input: [radio-features.md](../spec/radio-features.md), battery) |
| `bands`, `band_count` | Receive ranges, each with `tx` true where transmit is allowed |
| `power_count`, `power_names`, `power_default` | Power levels as short labels, lowest first, and the default index |
| `display`, `input`, `keymap`, `keymap_len` | Display and key interfaces, and the key map (below) |
| `radio`, `dmr`, `audio`, `vocoder` | RF chip, DMR modem (NULL on an FM-only radio), audio path, codec hosting |
| `power`, `flash` | Battery and power switch, external flash |
| `host_port`, `cps`, `netif` | The link to a computer (NULL if none), CPS and USB network hooks |
| `system`, `diag_line`, `diag_reset` | Optional: chip identity and reboot modes, a diagnostics page |
| `battery_low_mv`, `battery_critical_mv` | Battery warning levels; 0 for none |
| `audio_tap`, `bcast`, `afsk` | Optional: audio path taps, a broadcast FM receiver, APRS AFSK transmit and receive audio |
| `gps_present` | Optional, with `CAP_GPS`: whether the GPS module is fitted (radios sold with and without one) |

Optional hooks may be NULL; the core checks them.

## The key map

Each `keymap_entry_t` (`hal/hal_input.h`) maps one bit of the board's key scan to logical actions:

| Field | Meaning |
|---|---|
| `bit` | The key's bit in `hal_input_t.scan()` |
| `short_action`, `long_action` | `input_action_t` for a press and a hold |
| `raw_edges` | Report down and up instead of press and hold (PTT, monitor) |
| `repeat` | Repeat while held (Up, Down) |

Map keys to the logical actions of `input_action_t` (`ACT_SELECT`, `ACT_BACK`, `ACT_QUICK`, `ACT_CONTEXT`, `ACT_USER_1` to `ACT_USER_4`, `ACT_MONITOR`, `ACT_TALK_TARGET`, ...)
so the five key rules of [docs/ux/principles.md](../ux/principles.md) hold on the new radio. A radio without a keypad
or with fewer keys must still reach every task; [docs/spec/ui.md](../spec/ui.md) section 3 says how. Give some key
`ACT_EMERGENCY` if the radio has one: no current board does.

## The display

The UI draws for a display class (size, color depth, insets). If the new display matches an existing class, nothing
changes in the UI. A new class needs a gallery entry and a key map pair in the UI host tests (see
[Testing](testing.md)); ask before adding one, since every class multiplies the screens to review.

The frame buffer is sized to the panel in `board.mk` `MCU_DEFS`: `GFX_FB_BYTES` (width times height times bits per pixel,
over 8), `GFX_MAX_ROWS` (the height) and `GFX_INDEXED=1` for a 16-color panel. Without them the build reserves the
40 KB RGB565 default. `boards.py check` requires the buffer to hold the board's `hardware.gallery` class.

## Bring-up order

1. Platform: start-up, clocks, a time base, a UART or USB for logging. Blink something.
2. `board.c` with the display and input interfaces. The UI comes up with the built-in channels.
3. The debug link on the host port, so screenshots and key injection work from here on ([Debug link](debug-link.md)).
   If the host port is a two-pin K-type programming jack, check whether PTT shares the line that carries the host's
   data: send back-to-back 0x00 bytes and watch the raw PTT pin. If it goes low, gate PTT on UART traffic
   (`drivers/input/sharedptt`, [gotchas.md](../spec/gotchas.md) 16.2) before anything transmits. Then run
   `tests/hil/test_ptt_host_traffic.py`: host polling must make no PTT press.
4. Flash, settings and calibration reads. Check the foreign-region guard on a radio from stock firmware
   ([settings-store.md](../spec/settings-store.md) 5).
5. RF chip: FM receive, then FM transmit into a dummy load.
6. DMR modem and the codec: receive, then transmit, checked on the SDR bench.
7. CPS, so qdmr can program it.

[docs/testing/dm32-bringup.md](../testing/dm32-bringup.md) is this list made concrete for one radio.

## Bring-up records

What the docs claim for a radio comes from its records, so each step above ends with a recorded run.

1. Add `boards/<b>/board.toml` with `[status] level = "bring-up"`, a `summary` and `[docs] hardware`. A bring-up page
   (`docs/testing/<b>-bringup.md`, `[docs] bringup`) is optional; with one, the README links the radio to it.
2. `tools/bringup.py list --radio <b>` shows the checklist: every feature of
   [tests/hil/features.toml](../../tests/hil/features.toml) the board builds, `n/a` for its `BOARD_UNSUPPORTED`
   switches and `not written` for its `BOARD_NOT_WRITTEN` ones. Each needs a reason in `config.mk`; a feature built
   but known not to work yet gets a `[features]` entry in `board.toml`.
3. As a step works, `tools/bringup.py run --radio <b> --feature F -- <pytest options>` runs its tests under the
   radio lock and writes `docs/testing/results/<b>/<date>-<commit>.json`. Commit the record with the change it
   checks; a record with failures is the to-do list ([HIL bring-up mode](../../tests/hil/README.md#bring-up-mode)).
4. A check made before the radio had records (a bench log, a hand test written up in the hardware page) can go in
   the board's seed record, `docs/testing/results/<b>/seed.json`
   ([format](../testing/results/README.md#seed-record)). A run replaces a seed row.
5. Raise `[status] level` to `testing`, then `supported`, as the records cover the radio, update `summary`, and run
   `tools/boards.py docs`. `--floor` in ci.sh fails a change that makes any radio read lower than on `main`.

## Tests and CI

- Add host tests for each new driver against a simulated chip (`tests/host/*Test.c`; see the existing driver tests).
  Tests of the board's own code go in `boards/<b>/tests/` with a `host.mk` ([Testing](testing.md#host-tests)).
- HIL tests ([tests/hil](../../tests/hil/README.md#tests)): the health checks' numbers go in `board.toml` `[hil]`, the
  CPS radio types in `[cps]`, and a check only this radio can use in `boards/<b>/hil/test_*.py`.
- `tools/ci.sh` finds the board by its `board.mk` and builds its feature matrix when the compiler is installed. A
  board without its compiler is skipped with a notice.
- Run `tools/boards.py docs`: it writes the radio into the README's radio table, the user manual's radio index
  ([23-radios.md](../user/23-radios.md)), the feature matrix's "Built by default" table and the Port cell of the
  radio's row in the [radio survey](../spec/radios.md). `tools/ci.sh` fails when that wasn't run. Remove the radio's
  planned row from the survey, or make it the board's row: a planned row that names a board fails `tools/boards.py
  check`. Add the radio to [the user manual's introduction](../user/01-introduction.md).
- The web site lists the radio from `board.toml`: `order` places it, `[status] level` and `summary` give its badge and
  its line for visitors. A survey row that names a board is the board's row, so the site doesn't list it as planned.
- Before it can be released: its `board.toml` `[flash]` and `[codec]` tables, `[status] release = "stable"` and the
  regenerated `tools/rfoundry/rfoundry/registry.json` (`tools/boards.py json --for rfoundry --out ...`). The release
  workflow's job for its toolchain then builds and packages it, with each build option value
  ([release.md](release.md#the-release-workflow)); CI and the nightly pick it up from the registry too
  ([ci.md](ci.md)). The jobs are grouped by toolchain (`platform.toml` `[toolchain] id`), so neither a new radio nor
  a new platform on a known toolchain (another Arm SoC, say) needs a workflow edit. Only a new toolchain does.
