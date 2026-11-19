<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Building

This page covers the toolchains, the make targets, and how to flash your own build. For the feature switches, the
release variants, and the hardware build options, see [Build configuration](../build-config.md).

## Toolchains

A board's platform is `PLATFORM` in its `board.mk`. To list the boards on one platform, run
`tools/boards.py list --platform <p>`. The pinned version of each toolchain is in `platform/<p>/platform.toml`
`[toolchain]` (see [Board registry](board-registry.md#platformtoml)).

| Platform | Compiler | Where it comes from |
|---|---|---|
| `stm32f4` | `arm-none-eabi-gcc` | The Arm GNU Toolchain. `tools/install-arm-toolchain.sh` installs the pinned release on Linux x86_64. The Makefile looks in `~/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/` first, then `/usr/bin/`; `CROSS=` overrides both |
| `hrc7000` | `csky-elf-gcc` | Build it once with `tools/build-csky-toolchain.sh` (into `~/toolchains/csky-elf`, about 45 minutes), or use the container image: `tools/docker/csky-toolchain/run.sh make BOARD=<board>` (see [CI runners](ci-runners.md#the-csky-elf-image)). The Makefile looks in `~/toolchains/csky-elf/bin/`, then `PATH` |
| Host tests | The host's `cc` and zlib | The Xcode command line tools or the distribution's compiler |

You also need make, git, and Python 3. Clone with submodules: `git submodule update --init` fetches FreeRTOS,
TinyUSB, and ST's HAL and CMSIS into `third_party/`.

The images in `tools/docker/toolchains/` hold a whole build environment, so you can build without installing a
toolchain (see [Toolchain images](ci-runners.md#toolchain-images)).

## Make targets

In the following table, `<b>` is the board directory under `boards/`. To list the boards, run `tools/boards.py list`.

| Command | Does |
|---|---|
| `make BOARD=<b>` | Builds `build/<b>/rfoundry_<b>.{elf,bin,map}`, plus `rfoundry_<b>_codec.bin` when `codecs/` holds the radio's manufacturer firmware file and the board has a codec tool (see [Flashing your build](#flashing-your-build)). There is no default board: `make` alone lists them |
| `make BOARD=<b> BAND=vhf` | Builds a hardware build option on a board that has one, here the VHF image in `build/<b>-vhf` (see [Hardware build options](../build-config.md#hardware-build-options)) |
| `make BOARD=<b> VARIANT=amateur` | Builds a release variant (see [Variants](../build-config.md#variants)) |
| `make BOARD=<b> size` | Prints the section sizes and the space left before the codec |
| `make BOARD=<b> stack-check` | Checks each task's worst-case stack depth against its stack (see [Stack check](stack-check.md)) |
| `make test` | Runs the host tests (the same as `make -C tests/host`); needs no `BOARD` |
| `make BOARD=<b> print-config` | Prints the feature switches this board builds with, as `NAME=y/n` |
| `make BOARD=<b> print-cc` | Prints the compiler the board uses (ci.sh checks that it's installed) |
| `make BOARD=<b> print-unsupported`, `print-not-written`, `print-maximal-vars` | Prints `BOARD_UNSUPPORTED`, `BOARD_NOT_WRITTEN`, and `MAXIMAL_VARS` from the board's `config.mk`, for ci.sh's maximal build |
| `make BOARD=<b> clean` | Removes `build/<b>` |
| `make BOARD=<b> BUILD=dir` | Builds into another directory (ci.sh does this for each configuration) |
| `make ... OPT=-O0` | Changes the optimization level (default `-Os`) |
| `make ... LTO=1 [LTO_3P=1]` | Turns on link time optimization, and with `LTO_3P` for third-party code too. A board's `config.mk` or `board.mk` turns it on (see [Board build variables](../build-config.md#board-build-variables)) |

Every `*.c` and `*.S` file in the directories a build lists is compiled, so a new file needs no Makefile change. The
directories are the core's (some only with their switch), the platform's (`platform/<p>/platform.mk`), and the
board's (`BOARD_DIRS` in `boards/<b>/board.mk`). A few single files of `core/dmr` are added by their own switch.
Changing the compile flags (a switch or a value) rebuilds everything.

### The firmware version

The build stamps the image with the commit it was built from (`RFOUNDRY_GIT`), with `-dirty` appended when tracked
files differ from that commit or untracked sources sit in the built directories. The splash screen, the debug link's
VERSION reply, and the MMDVM GET_VERSION description show it, and the build prints it on the image line
(`image ...: N bytes, version c1544f29-dirty`). Build an image for anyone else from a clean, committed tree.

A clean build of a release tag also carries the release name (`RFOUNDRY_RELEASE`, see
[Release names](release.md#release-names)). The splash screen shows it in place of the commit, and About and the debug
link's VERSION reply (`release=`) show both. When the version changes, only the few files that use it are rebuilt.

## Image size

The linker's application region ends before the codec, so an image that grows into it fails to link. Each board's code
space and the codec sectors that end it are in the board's codec maps (see [The AMBE codec](codec.md)). Each feature's
cost and the free space on each radio are in the [feature matrix](../feature-matrix.md). `tools/ci.sh` writes every
configuration's size and free space to `build/ci/size-report.md`.

## Flashing your build

Users flash with the web installer or `rfoundry` (see [Installing](../user/02-installing.md)). To flash from a build
tree, use the route for the radio's platform:

| Platform | Flash |
|---|---|
| `stm32f4` | `rfoundry flash build/<b>/rfoundry_<b>.bin --board <b> [--stock FILE]` (see [tools/rfoundry](../../tools/rfoundry/README.md)). It keeps the radio's codec or merges it from the manufacturer firmware file, and erases only the sectors it needs. The two-step route also works: `tools/codec_extract.py --stock FILE`, then `tools/stm32_dfu.py IMAGE --model <b> --key <key file>`, which erases every application sector. To move a radio to RadioFoundry with its codeplug, use `tools/<id>_migrate.py` where a board has one |
| `hrc7000` | `tools/hrc7000_codec.py` merges the codec. `tools/serial_loader.py IMAGE` writes the image through the radio's bootloader over the programming cable (`--wrap` writes the update file for the manufacturer's updater). `rfoundry` uses the serial route (see [tools/rfoundry](../../tools/rfoundry/README.md#the-serial-route)) |

Without a codec, the firmware runs with FM only (see [The AMBE codec](codec.md)).

With the radio's manufacturer firmware file in `codecs/`, `make` also writes `<image>_codec.bin`, the image with the
codec merged in. `tools/codec_auto.py` picks the file by SHA-256 and runs the board's codec tool. Flash that file with
any route in the table. Git ignores everything in `codecs/` except its [README](../../codecs/README.md).
