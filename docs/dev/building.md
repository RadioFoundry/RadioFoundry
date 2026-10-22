<!-- SPDX-License-Identifier: MIT -->

# Building

## Toolchains

A board's platform is `PLATFORM` in its `board.mk`; `tools/boards.py list --platform <p>` lists the boards on one. The
pinned version of each toolchain is in `platform/<p>/platform.toml` `[toolchain]` ([The board registry](board-registry.md#platformtoml)).

| Platform | Compiler | Where it comes from |
|---|---|---|
| `stm32f4` | `arm-none-eabi-gcc` | Arm GNU Toolchain; `tools/install-arm-toolchain.sh` installs the pinned release on Linux x86_64. The Makefile looks in `~/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/` first, then `/usr/bin/`; `CROSS=` overrides |
| `hrc7000` | `csky-elf-gcc` | Build it once with `tools/build-csky-toolchain.sh` (into `~/toolchains/csky-elf`, about 45 minutes), or use the Docker image: `tools/docker/csky-toolchain/run.sh make BOARD=<board>` ([CI](ci.md#the-csky-elf-image)). The Makefile looks in `~/toolchains/csky-elf/bin/`, then `PATH` |
| Host tests | the host's `cc` and zlib | Xcode command line tools or the distribution's compiler |

You also need make, git and Python 3. Clone with submodules: `git submodule update --init` fetches FreeRTOS, TinyUSB
and ST's HAL and CMSIS into `third_party/`.

## Make targets

`<b>` is the board directory under `boards/` (`dm1701`, `md380`, `dm32`, `db25d`).

| Command | Does |
|---|---|
| `make BOARD=<b>` | Builds `build/<b>/odr_<b>.{elf,bin,map}`, plus `odr_<b>_codec.bin` when `codecs/` holds the radio's stock file and the board has a codec tool (below). There is no default board: `make` alone lists them |
| `make BOARD=md380 BAND=vhf` | A build option (`board.toml` `[[build_options]]`): the MD-380 VHF image, in `build/md380-vhf` (default `uhf`; one image per band) |
| `make BOARD=<b> size` | Section sizes and the space left before the codec |
| `make BOARD=<b> stack-check` | Each task's worst-case stack depth against its stack ([stack-check.md](stack-check.md)) |
| `make test` | The host tests (the same as `make -C tests/host`); needs no `BOARD` |
| `make BOARD=<b> print-config` | The feature switches this board builds with, as `NAME=y/n` |
| `make BOARD=<b> print-cc` | The compiler the board uses (ci.sh checks it is installed) |
| `make BOARD=<b> print-unsupported`, `print-maximal-vars` | `BOARD_UNSUPPORTED` and `MAXIMAL_VARS` from the board's `config.mk`, for ci.sh's maximal build |
| `make BOARD=<b> clean` | Removes `build/<b>` |
| `make BOARD=<b> BUILD=dir` | Builds into another directory (ci.sh does this for each configuration) |
| `make ... OPT=-O0` | Changes the optimization level (default `-Os`) |
| `make ... LTO=1 [LTO_3P=1]` | Link time optimization, third-party code too with `LTO_3P` (on by default only on the MD-380; [build-config.md](../build-config.md#board-build-variables)) |

Every `*.c` and `*.S` file in the directories a build lists is compiled, so a new file needs no Makefile change. The
directories are the core's (some only with their switch), the platform's (`platform/<p>/platform.mk`) and the
board's (`BOARD_DIRS` in `boards/<b>/board.mk`). A few single files of `core/dmr` are added by their own switch.

The build stamps the image with the commit (`ODR_GIT`), plus `-dirty` when tracked files differ or untracked sources sit
in the built directories. Changing the compile flags (a switch, a value) rebuilds everything. See
[docs/testing/README.md](../testing/README.md), "The firmware version".

## Feature switches

Every switch, its default, what it depends on and what it costs is in [docs/build-config.md](../build-config.md).
In short:

- `config/features.mk` lists every switch with its project default. Sub-features follow their parent
  (`CONFIG_DMR=n` turns off every DMR feature); asking for a sub-feature without its parent is a build error.
- `platform/<p>/config.mk` changes the defaults for every radio on a platform, and `boards/<b>/config.mk` for one
  radio, both with `?=` so the command line still wins. The board's file is read first, so it wins over the platform's.
- The command line has the last word: `make BOARD=dm1701 CONFIG_HOTSPOT=n CONFIG_SFR=y`.
- Each switch reaches C as `CONFIG_X=0/1` (`core/config.h` gives defaults for the host tests). Test it with
  `#if CONFIG_X`, never `#ifdef`.
- A new feature that costs flash gets a switch: the steps are in [build-config.md, "Adding a feature"](../build-config.md#adding-a-feature).
  ci.sh then builds each board with it off alone (when on by default) and with every switch on.

## Hardware build options

Some radios come in hardware versions that one image can't serve, such as the MD-380's UHF and VHF boards. Each such
choice is a build option:

| Where | What it holds |
|---|---|
| `boards/<b>/board.mk` | `BUILD_OPTIONS` names the options; `<NAME>_VALUES` lists each one's values, the default first (`BAND_VALUES := uhf vhf`) |
| `boards/<b>/board.toml` `[[build_options]]` | The words people read: a title and help for the option, a title, a text and the reported board name for each value. `tools/boards.py check` fails when its values differ from `board.mk` |
| C | `ODR_<NAME>_<VALUE>` is 1 for the chosen value and 0 for the others (`#if ODR_BAND_VHF`) |

`make BOARD=md380 BAND=vhf` builds the VHF image. A value the board doesn't list stops the build with the list. A
non-default value is part of the build directory and the image name (`build/md380-vhf/odr_md380-vhf.bin`), and of the
release file (`odr-md380-vhf-<version>.bin`). `tools/ci.sh` builds every value. The release manifest records the
values (`build_options`). odrflash takes them from the board name a running radio reports (`MD-380 VHF`), or from
`--option BAND=vhf`, and refuses an image built for other hardware than the radio's.

Prefer one image that detects the hardware at run time when that is possible. Build options are separate from release
variants (`VARIANT=`, [build-config.md](../build-config.md)), which choose features, not hardware.

## Image size

Each board's code space and the codec sectors that end it are in [release.md, "Sector maps"](release.md#sector-maps).

The linker's application region ends before the codec, so an image that grows into it fails to link. The current figures and each feature's cost are in
[feature-matrix.md](../feature-matrix.md). `tools/ci.sh` writes every configuration's size and free space to
`build/ci/size-report.md`.

## Flashing

The user manual covers it for users: [Installing](../user/02-installing.md). In short:

| Radio | Flash |
|---|---|
| DM-1701 | `odrflash flash build/dm1701/odr_dm1701.bin --board dm1701 [--stock FILE]` ([tools/odrflash](../../tools/odrflash/README.md)): keeps the radio's codec or merges it from the stock file, erases only the sectors it needs. The older two-step route still works: `tools/codec_extract.py --stock FILE` (key derived from the file, or `--key`), then `tools/stm32_dfu.py IMAGE --model dm1701 --key dm1701.key`, which erases every application sector |
| MD-380 | `tools/md380_migrate.py flash --image IMAGE --stock FILE` (merges the codec), or `tools/codec_extract.py` then `tools/stm32_dfu.py --model md380`. Moving a radio from stock with its codeplug: [md380-migration.md](../spec/md380-migration.md) |
| DM-32UV | `tools/hrc7000_codec.py` merges the codec; `tools/serial_loader.py IMAGE` writes it through the Baofeng bootloader over the programming cable (or `tools/serial_loader.py --wrap` makes the vendor file for Baofeng's updater). Moving a radio from stock with its codeplug: [dm32-migration.md](../spec/dm32-migration.md) |

Without a codec the firmware runs, with FM only. See [The codec](codec.md).

With the radio's stock firmware file in `codecs/`, `make` also writes `<image>_codec.bin`, the image with the codec
merged in (`tools/codec_auto.py` picks the stock file by SHA-256 and runs the board's codec tool). Flash that file with
any route above. Git ignores everything in `codecs/` except its [README](../../codecs/README.md).
