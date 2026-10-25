<!-- SPDX-License-Identifier: MIT -->

# The Board Registry

The registry holds every fact about a radio that the build itself doesn't need: its identity, status, build options,
flashing route, codec route and the web site's data. The tools, CI, the docs and the site read it. The build reads no
TOML: make keeps its own facts in `board.mk`, `config.mk` and `memory.ld`, and the registry reads those through
make.

`tools/boards.py` loads and checks the registry. It needs Python 3.11 or later (`tomllib`); scripts find one with
`tools/rfoundry_python.sh`.

## Where each fact lives

Each fact is in one file. A value another file already holds is derived from it, never typed twice.

| File | Holds |
|---|---|
| `boards/<id>/board.toml` | Identity, status, build option text, CI extras, flashing family, bootloader and stub data, codec route, the site's data |
| `platform/<p>/platform.toml` | ISA, CPU, toolchain, platform stubs, flash base, sector geometries, codec symbols |
| `boards/<id>/board.mk` | `PLATFORM`, `APP_BASE` (the image address), `HSE_VALUE` (the crystal, for the STM32F4 stub), `BOOT_*`, `BOARD_DIRS`, `BUILD_OPTIONS` and each option's `<NAME>_VALUES`, read with `make print-vars` so the board's and the platform's `config.mk` and their conditionals count. `LDSCRIPT` defaults to `memory.ld` |
| `boards/<id>/memory.ld` | The application region (`MEMORY FLASH`, from `__app_start`) and the codec symbols (`__codec_*`). Every `*.ld` in the board directory is read for the codec symbols |
| `boards/<id>/*.c` | The `board_t` `.name` values: the names the debug link reports |
| `boards/<id>/pins.h` | The pins the stub uses: the power latch and key (HR_C7000), the SPI flash's `PIN_FLASH_SCK`, `_MISO`, `_MOSI`, `_CS` and the `idle_cs` pins (STM32F4), each as `GPIOx, GPIO_PIN_n` on STM32F4 |

Derived values:

| Value | From |
|---|---|
| Flashing link (USB, serial cable) | The family |
| CPU | The platform, unless `[hardware] cpu` names another |
| Protected range | The platform's `flash_base` up to the application base |
| Erasable sectors | The platform geometry from the application base up |
| Stub NAND flag | `drivers/flash/spi_nand` in `BOARD_DIRS` |
| Stub power pins | `PIN_POWER_LATCH` and `PIN_POWER_KEY` in `pins.h`, when `hold_power` is set |
| Stub board record (STM32F4) | `spi` and `idle_cs`, the `PIN_FLASH_*` pins, the bus's alternate function from the platform's `board_stub.spi_af`, `HSE_VALUE` and `APP_BASE` |
| Debug link names | The `board_t` names, each build option value's `board_name`, `debuglink_aliases`, the id |
| Codec windows of a linker slot | The `__codec_slot` symbols |
| Codec windows of a stock file | The stock file's profile in `tools/rfoundry/rfoundry/stockfw.py` |

## board.toml

The schema in `tools/boards.py` (`BOARD_SCHEMA`) lists every key; `check` refuses an unknown or missing one. Copy a
neighbor on the same platform.

### Identity

| Key | Type | Meaning |
|---|---|---|
| `id` | string | The directory name; `make BOARD=<id>` |
| `name` | string | Maker and model, as the site and the tools show it |
| `maker`, `model` | string | The two parts of the name |
| `also` | list of strings | The same radio sold under other names |
| `kind` | `handheld` or `mobile` | |
| `part_of` | string, optional | Another board this one is a build of |
| `debuglink_aliases` | list, optional | Names older firmware reported on the debug link |

### [docs]

| Key | Meaning |
|---|---|
| `hardware` | The hardware spec, `docs/spec/hardware-<id>.md` |
| `codec` | Optional: the codec spec |
| `install` | Optional: the user manual's page for moving from stock firmware |
| `bringup` | Optional: the bring-up checklist |

`check` fails when a page doesn't exist. The README's radio table links the board to its `bringup` page, else its
`hardware` page.

### [hardware]

One-line summaries for the site and the generated tables; the facts stay in `docs.hardware`.

| Key | Meaning |
|---|---|
| `cpu` | Optional: overrides the platform's CPU in the tables |
| `bands`, `power`, `display` | Short text |
| `gallery` | The display class (`160x128`); `check` requires the UI gallery to have it, and the frame buffer in `board.mk` (`GFX_FB_BYTES`, `GFX_MAX_ROWS`, `GFX_INDEXED`) to hold it |
| `unconfirmed` | Optional: hardware switches (`hardware` in `tests/hil/features.toml`) the board neither builds nor lists in `BOARD_UNSUPPORTED` or `BOARD_NOT_WRITTEN` ([the rule](../build-config.md#unsupported-not-written-unconfirmed)), because no source says yet whether the radio has the part: switch to what is unknown. `check` fails on any other hardware switch that is off by default, and on an entry for a switch the board builds or lists. The feature matrix shows it as ? |
| `clock` | `{ rtc, note }`: the real-time clock. `rtc` is `backed` (keeps time through power-off), `unbacked` (only while on), `none` or `unknown`; `note` gives the source. The feature matrix and the site show it |

### [status]

| Key | Values | Meaning |
|---|---|---|
| `level` | `bring-up`, `testing`, `supported` | How far the port is |
| `summary` | string | What works, then what is open, in one sentence or two; Markdown links allowed. The README, the user manual and the web site show it |
| `release` | `stable`, `testing` | `stable`: release images are built and rfoundry fetches them |

### [[build_options]]

A hardware build option: one radio model sold in versions that need different images (one band per radio, for
example). `board.mk` holds the build's side: `BUILD_OPTIONS := BAND` and `BAND_VALUES := uhf vhf`, the default first.
`board.toml` holds the words. `check` requires the same names and values in the same order.

| Key | Meaning |
|---|---|
| `name` | The make variable (`BAND`) |
| `title`, `help` | The question the site and rfoundry ask, and how to tell the answer |
| `values` | A list of `{ id, title, text, board_name }`: the make value, its label and description, the name the radio reports on the debug link |

The Makefile defines `RFOUNDRY_<NAME>_<VALUE>` as 1 or 0 for every value. A value other than the default adds `-<value>`
to the build directory and the image: `make BOARD=<id> BAND=vhf` writes `build/<id>-vhf/rfoundry_<id>-vhf.bin`.
[Building](building.md#hardware-build-options) has the user side.

### [ci]

| Key | Meaning |
|---|---|
| `extra` | A list of `{ name, make }`: more configurations ci.sh builds for this board, with their make arguments |

### [flash]

| Key | Meaning |
|---|---|
| `family` | The flashing route, by protocol: `usb-dfu`, `serial-loader` or `serial-iap` |
| `keep_codec` | True when an update leaves the codec in place |
| `sectors` | Optional: a geometry of the platform other than its `default_sectors` |
| `stock_download` | Optional: where the stock firmware can be downloaded |
| `stock_versions` | The stock versions the tools know |
| `backup` | `{ stock, rfoundry, spi_size }`: what a backup reads from stock firmware and from RadioFoundry (`none` or `full`), and the SPI flash size |

`[flash.bootloader]`:

| Key | Meaning |
|---|---|
| `steps` | How to enter the bootloader, one step per item, for the web flasher and the manual |
| `keys` | Optional: the short form the tools print |
| `id` | Optional: the string the loader answers, also the magic of its files (`serial-loader`) |
| `sign` | Optional: what the radio shows in the bootloader |

`[flash.dfu]` (`usb-dfu`): `vid`, `pid`, `string1`, `product` (the USB identity) and `key_sha256`, `key_prefix` (the
per-model XOR key's hash and first bytes, never the key).

`[flash.stub]` (a board on a platform with `board_stub`): `id` (the board id the stub echoes, unique per stub; 0xFE is reserved for the stub tools' test profiles) and
`tested` (flashed through the stub on a radio), then the keys the platform's `board_stub` names:

| Platform stub | Required | Optional |
|---|---|---|
| `stubtool` (HR_C7000) | `hold_power` (the stub drives the power latch), `progmem` (program flash size) | `stock_console` (the stock firmware answers a console the web flasher reads), `read_rate` |
| `f4stub` (STM32F4) | `spi = { bus, mode, hz }` (the SPI flash's bus 1-3, SPI mode 0-3, fastest clock) | `idle_cs` (up to two `pins.h` names of other chip selects on that bus, held high) |

A key of another platform's stub fails the check.

### [codec]

| Key | Meaning |
|---|---|
| `tool` | The tool that extracts or merges it: `codec_extract` or `hrc7000_codec` |
| `stock` | A list of SHA-256 digests: the stock files rfoundry merges the codec from (stock-file placement) |
| `profile` | The codec id the firmware reports, for a codec in a linker slot |
| `sources` | Where a slot codec comes from, for messages |
| `installed` | Stock versions that leave exactly this codec in place |

A board has either `stock`, or `profile` with a linker codec slot. With `stock`, the profile and sources come from the
stock files' profiles. The files must hold the same codec; each may put its tables at other addresses. The board's
codec windows are the union of every file's layout. The image must stay out of all of them, and keeping the codec keeps
all of them. A merge writes and checks the layout of the file it merges
([codec-placement.md](../spec/codec-placement.md)).

### [frame]

How the site draws the radio: `shape`, `body` and `accent` colors, `lens` size.

## platform.toml

| Key | Meaning |
|---|---|
| `id`, `isa`, `cpu` | The platform directory, the ISA, the SoC |
| `stubs` | Platform stubs ci.sh builds with the platform's compiler |
| `board_stub` | The stub its boards' `[flash.stub]` is for: `tool`, the required `keys`, the `optional` ones and, for SPI wiring, `spi_af` (the alternate function per bus). Absent: no `[flash.stub]` |
| `codec_symbols` | The linker symbols every board's scripts must define |
| `flash_base` | Where program flash starts; the factory loader holds it up to the image |
| `default_sectors`, `[sectors]` | The flash geometries, `[index, address, size]` in the bootloader's erase order |

`[toolchain]`:

| Key | Meaning |
|---|---|
| `id`, `name` | The toolchain (`arm`, `csky`) and its display name. CI has one firmware job per toolchain (`firmware (Arm)`), and `.github/actions/toolchain` sets it up by id. Platforms with the same id build in the same job and must have the same `[toolchain]` table |
| `cc` | The compiler `make print-cc` names |
| `build` | The script that installs or builds it |
| `version` | The release CI and releases build with |
| `sha256` | Optional: the SHA-256 of the archive the script downloads |
| `image` | Optional: the container image that holds the toolchain (`RFOUNDRY_CSKY_IMAGE` overrides it) |

The toolchain scripts read these values with `tools/platform_toolchain.sh`, which needs no Python. `check` fails when
a script, Dockerfile or workflow under `tools/` or `.github/` sets `<ID>_GCC_VERSION` or `<ID>_GCC_SHA256` (for
example `ARM_GCC_VERSION`) to a literal other than the pin: take it from `platform.toml`.

## Commands

| Command | Output |
|---|---|
| `tools/boards.py list [--platform P[,Q]] [--toolchain T[,U]] [--release R] [--comma]` | Board ids; an unknown platform or toolchain fails. `tools/ci.sh --toolchain` and the CI workflows select boards with it |
| `tools/boards.py get <id> [key.path]` | One value; a list prints one item per line |
| `tools/boards.py platform <p> [key.path]` | One platform value |
| `tools/boards.py builds <id> [--extra]` | The board's builds besides the default, or its `[ci]` extras |
| `tools/boards.py json [--for all\|rfoundry\|build]` | A merged view, below |
| `tools/boards.py check` | Every inconsistency, and every radio named outside its own places ([Radio names](#radio-names)); ci.sh's registry step runs it |
| `tools/boards.py docs [--check] [--floor REF]` | Writes the generated blocks of the README, the user manual's radio pages, the feature matrix (its Built by default table, and its Checked on each radio table from the bring-up records in `docs/testing/results/`), the bug report form's and the pull request template's radio lists, and the survey's Port cells (`tools/registry_docs.py`). `--check` lists the stale ones instead; `--floor REF` fails when a radio's marks or status in the README read lower than at commit `REF`. ci.sh runs `--check --floor origin/main` |
| `tools/boards.py self-test` | The loader's own tests |

`make registry` regenerates `tools/rfoundry/rfoundry/registry.json` when a source changed. `make` does too, before
every build.

## The views

| View | Format | For |
|---|---|---|
| `--for all` | none (the TOML with a `derived` table) | Scripts in this tree |
| `--for rfoundry` | `rfoundry-board-registry/2` | rfoundry, the release tool and the site. Committed as `registry.json`, so they run on any Python 3 and inside the rfoundry executable |
| `--for build` | `rfoundry-build-matrix/1` | Tools that build from a checkout |

### The build view

`tools/boards.py json --for build` gives a tool that builds images from a checkout everything it needs, with nothing
from outside the tree. It is a stable interface: a change that breaks a reader changes `format`.

| Key | Contents |
|---|---|
| `format` | `rfoundry-build-matrix/1` |
| `variants` | The release variants: `full`, then each `config/variants/<name>.mk` |
| `platforms.<p>` | `isa`, `cpu`, `toolchain` (the id), `cc`, `version`, `image` (null: no container), `install`, `sha256` (null when none) |
| `boards.<id>` | `name`, `platform`, `release`, `released` (true for `stable`), `first_stage`, `build_options`, `builds` |
| `boards.<id>.build_options` | A list of `{ name, default, values }` |
| `boards.<id>.builds` | One entry per combination of option values, the all-default one first |

Each build:

| Key | Example (a board `<id>` with `BAND=vhf`) |
|---|---|
| `options` | `{"BAND": "vhf"}` |
| `make` | `["BOARD=<id>", "BAND=vhf"]`: the make arguments; add `VARIANT=<name>` for a variant |
| `release` | `["--board", "<id>", "--build-option", "BAND=vhf"]`: the `tools/release.py` arguments; add `--variant <name>` |
| `image_id` | `<id>-vhf` |
| `build_dir` | `build/<id>{variant}-vhf` |
| `image` | `build/<id>{variant}-vhf/rfoundry_<id>-vhf.bin`: what make writes and the release carries |
| `release_image`, `release_manifest` | `rfoundry-<id>{variant}-vhf-{version}.bin` and `.json`: what `tools/release.py` writes |

In the names, `{variant}` is empty for the full build and `-<name>` for a variant; `{version}` is the release tag. One
rule makes all of these names: `tools/rfoundry/rfoundry/names.py`. The Makefile follows it too, and `check` compares
make's `BUILD` and `TARGET` with the view for every build.

## Radio names

AGENTS.md rule 4 says only a radio's own files carry its name. `tools/check_generic.py` (run by
`tools/boards.py check`) enforces it. The names come from the registry: each board's id, `model` and `also`, matched
without regard to case, hyphens or a letter suffix (`XR-100UV`, `XR100`, `xr100`). A maker's word in front is dropped.

| Allowed place | What it covers |
|---|---|
| `boards/<id>/` | The radio's own code, tests and data |
| The radio's pages | A `docs/` path with the id in it (`hardware-<id>.md`, `<id>-bringup.md`, `results/<id>/`), a `[docs]` page no other board shares, and any directory named `<id>` (`codec_maps/<id>/`) |
| List pages | `docs/spec/radios.md`, `docs/user/23-radios.md`, `docs/feature-matrix.md` |
| Generated blocks | Between the markers `tools/boards.py docs` writes, in every marker style, and `registry.json` |
| File references | A path or a file name in the text points to the radio's own page |
| `tools/generic_allow.txt` | A glob and its reason. An entry no file needs is an error |

Everything else fails, tests included, and so does a file whose own path names a radio. Fix it by naming the
function, the chip or the platform, or by reading the value from the board. The allowlist holds the debt from before
the check; don't add new code to it.

## Changing the registry

- A new key goes in the schema, in this page and in the reader that needs it. If the build already knows the value,
  derive it instead.
- Run `tools/boards.py check` and `make registry`, and commit the regenerated `registry.json` with the change.
- A new board: [Adding a radio](adding-a-radio.md).
