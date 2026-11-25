<!-- SPDX-License-Identifier: MIT -->

# Building

## Toolchains

A board's platform is `PLATFORM` in its `board.mk`; `tools/boards.py list --platform <p>` lists the boards on one. The
pinned version of each toolchain is in `platform/<p>/platform.toml` `[toolchain]` ([The board registry](board-registry.md#platformtoml)).

| Platform | Compiler | Where it comes from |
|---|---|---|
| `stm32f4` | `arm-none-eabi-gcc` | Arm GNU Toolchain; `tools/install-arm-toolchain.sh` installs the pinned release on Linux x86_64. The Makefile looks in `~/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/` first, then `/usr/bin/`; `CROSS=` overrides |
| `hrc7000` | `csky-elf-gcc` | Build it once with `tools/build-csky-toolchain.sh` (into `~/toolchains/csky-elf`, about 45 minutes), or use the Docker image: `tools/docker/csky-toolchain/run.sh make BOARD=<board>` ([CI](ci-runners.md#the-csky-elf-image)). The Makefile looks in `~/toolchains/csky-elf/bin/`, then `PATH` |
| Host tests | the host's `cc` and zlib | Xcode command line tools or the distribution's compiler |

You also need make, git and Python 3. Clone with submodules: `git submodule update --init` fetches FreeRTOS, TinyUSB
and ST's HAL and CMSIS into `third_party/`.

## Worktrees

A `git worktree` does not need its own copy of the submodules (about 100 MB of files). Leave them uninitialized and
set `RFOUNDRY_THIRD_PARTY` to the `third_party/` of a checkout that has them, as an absolute path.
`tools/worktree.sh` creates such a worktree:

```
export RFOUNDRY_THIRD_PARTY=$HOME/src/RadioFoundry/third_party    # once, in the shell profile
tools/worktree.sh add ../rf-topic -b topic origin/main
make -C ../rf-topic BOARD=<b>
```

| Command | Does |
|---|---|
| `tools/worktree.sh add <path> [options] [commit]` | `git worktree add`, then `setup` in the new worktree |
| `tools/worktree.sh setup` | In the current worktree, marks each submodule not checked out inactive for this worktree only, and turns symlinks to another checkout back into empty directories |
| `tools/worktree.sh own <name>...` | Checks out these submodules in the current worktree, with the objects borrowed from the main checkout's clone |

`setup` matters with `submodule.recurse=true`. Without it, `git checkout` and `git reset` try to update the
submodules the main checkout marks active, and they leave a `.git` file in the empty directory that breaks every git
command there.

`platform/third_party.mk` picks, for each submodule, the directory a build reads:

| The submodule in this tree | `RFOUNDRY_THIRD_PARTY` | The build reads |
|---|---|---|
| Checked out | Any | This tree's copy, as before |
| Empty | Set | The copy in that directory, after the pin check |
| Empty | Not set | Nothing: make stops and says what to do |

- The pin check stops a build when the shared copy is at another commit than this tree pins or has local changes.
  A branch that moves a submodule runs `tools/worktree.sh own <name>`: this tree's copy then wins, and the others
  still come from the shared checkout.
- Git sees an uninitialized submodule as unchanged. `git status` stays clean, nothing under `third_party/` can be
  staged by accident, and `git worktree remove` works without `--force` (a worktree that owns a submodule still
  needs it).
- The image is the same as one built from this tree's copies. `-ffile-prefix-map` records the shared files as
  `third_party/...` in debug info and `__FILE__`, and `make stack-check` reads them by those names too.
- `RFOUNDRY_THIRD_PARTY` must be an absolute path that ends in `third_party`. Makefiles name third-party files by
  their path in this tree: sources are found through `vpath`, and the `-I` flags in `INCLUDES` are mapped.
  `$(wildcard)` sees only this tree's copies.
- `tools/docker/toolchains/run.sh` and `tools/docker/csky-toolchain/run.sh` mount the other checkout and its git
  directory, and pass the variable into the container.
- The query targets (`print-config`, `print-vars` and the rest) need no submodules.
- A worktree whose `third_party/*` are symlinks to another checkout still builds: the links count as checked out.
  Git trips on them, though: `tools/worktree.sh setup` switches one over.
- `tools/ci.sh` builds from a scratch worktree of this kind in the firmware step.

## Make targets

`<b>` is the board directory under `boards/` (`tools/boards.py list` lists them).

| Command | Does |
|---|---|
| `make BOARD=<b>` | Builds `build/<b>/rfoundry_<b>.{elf,bin,map}`, plus `rfoundry_<b>_codec.bin` when `codecs/` holds the radio's stock file and the board has a codec tool (below). There is no default board: `make` alone lists them |
| `make BOARD=<b> BAND=vhf` | A build option (`board.toml` `[[build_options]]`) on a board that has one: the VHF image, in `build/<b>-vhf` (one image per band) |
| `make BOARD=<b> size` | Section sizes and the space left before the codec |
| `make BOARD=<b> stack-check` | Each task's worst-case stack depth against its stack ([stack-check.md](stack-check.md)) |
| `make test` | The host tests (the same as `make -C tests/host`); needs no `BOARD` |
| `make BOARD=<b> print-config` | The feature switches this board builds with, as `NAME=y/n` |
| `make BOARD=<b> print-cc` | The compiler the board uses (ci.sh checks it is installed) |
| `make BOARD=<b> print-unsupported`, `print-not-written`, `print-maximal-vars` | `BOARD_UNSUPPORTED`, `BOARD_NOT_WRITTEN` and `MAXIMAL_VARS` from the board's `config.mk`, for ci.sh's maximal build |
| `make BOARD=<b> clean` | Removes `build/<b>` |
| `make BOARD=<b> BUILD=dir` | Builds into another directory (ci.sh does this for each configuration) |
| `make ... OPT=-O0` | Changes the optimization level (default `-Os`) |
| `make ... LTO=1 [LTO_3P=1]` | Link time optimization, third-party code too with `LTO_3P` (a board's `config.mk` or `board.mk` turns it on; [build-config.md](../build-config.md#board-build-variables)) |

Every `*.c` and `*.S` file in the directories a build lists is compiled, so a new file needs no Makefile change. The
directories are the core's (some only with their switch), the platform's (`platform/<p>/platform.mk`) and the
board's (`BOARD_DIRS` in `boards/<b>/board.mk`). A few single files of `core/dmr` are added by their own switch.

The build stamps the image with the commit (`RFOUNDRY_GIT`), plus `-dirty` when tracked files differ or untracked sources sit
in the built directories, and with the release name when the commit carries a release tag (`RFOUNDRY_RELEASE`,
[release.md](release.md#release-names)). Changing the compile flags (a switch, a value) rebuilds everything. See
[docs/testing/README.md](../testing/README.md), "The firmware version".

## Feature switches

Every switch, its default, what it depends on and what it costs is in [docs/build-config.md](../build-config.md).
In short:

- `config/features.mk` lists every switch with its project default. Sub-features follow their parent
  (`CONFIG_DMR=n` turns off every DMR feature); asking for a sub-feature without its parent is a build error.
- `platform/<p>/config.mk` changes the defaults for every radio on a platform, and `boards/<b>/config.mk` for one
  radio, both with `?=` so the command line still wins. The board's file is read first, so it wins over the platform's.
- The command line has the last word: `make BOARD=<b> CONFIG_HOTSPOT=n CONFIG_SFR=y`.
- Each switch reaches C as `CONFIG_X=0/1` (`core/config.h` gives defaults for the host tests). Test it with
  `#if CONFIG_X`, never `#ifdef`.
- A new feature that costs flash gets a switch: the steps are in [build-config.md, "Adding a feature"](../build-config.md#adding-a-feature).
  ci.sh then builds each board with it off alone (when on by default) and with every switch on.

## Hardware build options

Some radios come in hardware versions that one image can't serve, such as a UHF and a VHF board. Each such
choice is a build option:

| Where | What it holds |
|---|---|
| `boards/<b>/board.mk` | `BUILD_OPTIONS` names the options; `<NAME>_VALUES` lists each one's values, the default first (`BAND_VALUES := uhf vhf`) |
| `boards/<b>/board.toml` `[[build_options]]` | The words people read: a title and help for the option, a title, a text and the reported board name for each value. `tools/boards.py check` fails when its values differ from `board.mk` |
| C | `RFOUNDRY_<NAME>_<VALUE>` is 1 for the chosen value and 0 for the others (`#if RFOUNDRY_BAND_VHF`) |

`make BOARD=<b> BAND=vhf` builds the VHF image. A value the board doesn't list stops the build with the list. A
non-default value is part of the build directory and the image name (`build/<b>-vhf/rfoundry_<b>-vhf.bin`), and of the
release file (`rfoundry-<b>-vhf-<version>.bin`). `tools/ci.sh` builds every value. The release manifest records the
values (`build_options`). rfoundry takes them from the board name a running radio reports (`<name> VHF`), or from
`--option BAND=vhf`, and refuses an image built for other hardware than the radio's.

Prefer one image that detects the hardware at run time when that is possible. Build options are separate from release
variants (`VARIANT=`, [build-config.md](../build-config.md)), which choose features, not hardware.

## Image size

Each board's code space and the codec sectors that end it are in [release.md, "Sector maps"](release.md#sector-maps).

The linker's application region ends before the codec, so an image that grows into it fails to link. The current figures and each feature's cost are in
[feature-matrix.md](../feature-matrix.md). `tools/ci.sh` writes every configuration's size and free space to
`build/ci/size-report.md`.

## Flashing

Users flash with the web installer or `rfoundry`: [Installing](../user/02-installing.md). From a build tree:

| Platform | Flash |
|---|---|
| `stm32f4` | `rfoundry flash build/<b>/rfoundry_<b>.bin --board <b> [--stock FILE]` ([tools/rfoundry](../../tools/rfoundry/README.md)): keeps the radio's codec or merges it from the stock file, erases only the sectors it needs. The older two-step route still works: `tools/codec_extract.py --stock FILE`, then `tools/stm32_dfu.py IMAGE --model <b> --key <key file>`, which erases every application sector. Moving a radio from stock with its codeplug: `tools/<id>_migrate.py` where a board has one ([the migration specs](../spec/README.md#migrating-from-stock)) |
| `hrc7000` | `tools/hrc7000_codec.py` merges the codec; `tools/serial_loader.py IMAGE` writes it through the vendor bootloader over the programming cable (`--wrap` makes the vendor file for the vendor's updater), or `rfoundry` over the serial route ([release.md](release.md#the-serial-route-hr_c7000-radios)) |

Without a codec the firmware runs, with FM only. See [The codec](codec.md).

With the radio's stock firmware file in `codecs/`, `make` also writes `<image>_codec.bin`, the image with the codec
merged in (`tools/codec_auto.py` picks the stock file by SHA-256 and runs the board's codec tool). Flash that file with
any route above. Git ignores everything in `codecs/` except its [README](../../codecs/README.md).
