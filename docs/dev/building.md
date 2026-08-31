<!-- SPDX-License-Identifier: MIT -->

# Building

## Toolchains

| Radio | Compiler | Where it comes from |
|---|---|---|
| DM-1701 (`BOARD=dm1701`) | `arm-none-eabi-gcc` | Arm GNU Toolchain. The Makefile looks in `~/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/` first, then `/usr/bin/`; set `CROSS=` to override |
| DM-32UV (`BOARD=dm32`) | `csky-elf-gcc` (GCC 14) | Build it once with `tools/build-csky-toolchain.sh` |
| Host tests | the host's `cc` and zlib | Xcode command line tools or the distribution's compiler |

You also need make, git and Python 3. Clone with submodules: `git submodule update --init` fetches FreeRTOS, TinyUSB
and ST's HAL and CMSIS into `third_party/`.

## Make targets

| Command | Does |
|---|---|
| `make BOARD=dm1701` | Builds `build/dm1701/odr_dm1701.{elf,bin,map}`. `BOARD` defaults to `dm1701` |
| `make BOARD=dm1701 size` | Section sizes and the space left before the codec |
| `make test` | The host tests (the same as `make -C tests/host`) |
| `make BOARD=dm1701 print-config` | The feature switches this board builds with, as `NAME=y/n` |
| `make BOARD=dm1701 print-cc` | The compiler the board uses (ci.sh checks it is installed) |
| `make BOARD=dm1701 clean` | Removes `build/dm1701` |
| `make BOARD=dm1701 BUILD=dir` | Builds into another directory (ci.sh does this for each configuration) |
| `make ... OPT=-O0` | Changes the optimization level (default `-Os`) |

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
- `boards/<b>/config.mk` changes the defaults for one radio, with `?=` so the command line still wins.
- The command line has the last word: `make BOARD=dm1701 CONFIG_HOTSPOT=n CONFIG_SFR=y`.
- Each switch reaches C as `CONFIG_X=0/1` (`core/config.h` gives defaults for the host tests). Test it with
  `#if CONFIG_X`, never `#ifdef`.
- A new feature that costs flash gets a switch. Add it to `config/features.mk`, `core/config.h` and
  `docs/build-config.md`. ci.sh then builds each board with it off alone (when on by default) and with every switch on.

## Image size

The DM-1701 has 418 KB of code space before the codec window, the DM-32UV 912 KB before its codec slot (the board's
`config.mk` header). The linker's application region ends where the codec window starts, so an image that grows into
it fails to link. `tools/ci.sh` writes every configuration's size and free space to `build/ci/size-report.md`.

## Flashing

The user manual covers it for users: [Installing](../user/02-installing.md). In short:

| Radio | Merge the codec | Flash |
|---|---|---|
| DM-1701 | `tools/codec_extract.py --stock FILE --key dm1701.key` | `tools/stm32_dfu.py IMAGE --model dm1701 --key dm1701.key` in the bootloader |
| DM-32UV | `tools/dm32_codec.py` | `tools/dm32_pack.py` wraps it for Baofeng's updater |

Without a codec the firmware runs, with FM only. See [The codec](codec.md).
