<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Support info

`CONFIG_SUPPORT_INFO` (default on). What a support request needs to tell one radio from another: where the firmware
was built, which chips the radio has (a model can ship with more than one), and whether the image is one of ours.
About shows it ([17-settings.md](../user/17-settings.md)); the debug link's SUPPORT command gives it to a host
([debuglink.md](debuglink.md) 3.29), and the web site's support download collects it. Code: `core/support/`,
`core/debuglink/debuglink_support.c`, `tools/build_source.py`. Off: VERSION keeps `source=` and `repo=` (1), About shows none of it, and
SUPPORT answers status 4.

Nothing is read at start-up. The radio does the work only when asked: About when it opens, a host when it sends
SUPPORT. Every value but the image hash is one the board already holds, so neither waits for a chip.

## 1. Source

`tools/build_source.py` stamps each build with where it came from. The Makefile passes it as `RFOUNDRY_GIT_SOURCE`
and `RFOUNDRY_GIT_REPO`. About shows the source as "Source", without the repository (a fork's build says "Fork", so the plain words say
whose build it is); VERSION
and SUPPORT give `source=` and `repo=`.

| `source` | When | About |
|---|---|---|
| `official` | Our CI (GitHub Actions in RadioFoundry/RadioFoundry), not a pull request: main, nightly and release builds alike; only the image hash (3) tells a release | CI build |
| `service` | Our build service (`make BUILD_SOURCE=service`) | Build service |
| `pr` | Our CI building a pull request | Pull request |
| `local` | A clone whose `origin` is our repository | Local build |
| `fork` | Another repository's CI, or a clone of one | Fork |
| `unknown` | No git, or no `origin` | Unknown |

`repo` is GitHub's owner/name, or host/path for another host, never with credentials. `BUILD_SOURCE=` and
`BUILD_SOURCE_REPO=` override both. The C-SKY container forwards the `GITHUB_*` variables the tool reads.

The source is a hint. Anyone can build with `BUILD_SOURCE=official`. The image hash (3) is the proof.

## 2. Hardware lines

One line per part the radio has, in this order. A part the board doesn't report is left out.

| Key | About label | Value | From |
|---|---|---|---|
| `mcu` | MCU | The SoC family; on the STM32F4, the DBGMCU device and revision (left out when IDCODE reads 0) and the flash size | `board.system->chip` |
| `flash` | Flash | The external flash's JEDEC ID, 6 hex digits; `-` when nothing answered at start-up | `board.flash->jedec` |
| `display` | Display | `[controller] [ID] t<type> [f<factory byte>]`: the controller, its ID when read, the panel type the driver uses, and the factory byte that chose it | `board.display->panel` |
| `dmr` | DMR chip | The DMR baseband chip | `board.hw_line`, `BOARD_HW_DMR` |
| `rf` | RF chip | An RF chip with its ID or set-up; a board with two has two lines | `board.hw_line`, `BOARD_HW_RF` |
| `board` | Hardware | A board revision, batch or variant mark the board detected | `board.hw_line`, `BOARD_HW_BOARD` |
| (option) | The option's name | Each hardware option's stored choice, or "not set" ([debuglink.md](debuglink.md) 3.25) | `board.hwopts` |

`board.hw_line(i, &role, buf, len)` returns the board's i-th line from values it cached when it set the chips up. It
never starts a bus transfer. A board without it shows only the MCU, flash and display lines. Which radios have which
parts: [feature-matrix.md](../feature-matrix.md).

## 3. Image hash

The SHA-256 (FIPS 180-4) of the firmware image as it sits in program flash: from the image's first byte to the end of
its initialized data, the same bytes as the `.bin` a release ships (`_install.bin` for a board with a first stage).
The platform reports the span with `board.system->image` (the linker's `__app_base` and `__image_end`) and reads it
with `board.system->read`.

The hash runs in steps so the radio never stalls. About hashes up to 64 KB a redraw and redraws at its fast rate until
it is done; it shows "checking" until then, then the first 8 hex digits, enough to tell a release image. SUPPORT HASH hashes up to 32 KB a request.
Either step also ends after 5 ms, so a slow flash read or a slow core makes the hash take longer, not the UI stall.
About and SUPPORT step the same hash: each carries on where the other stopped, and a step that would run while the
other's is running does nothing. The result is kept until the next start-up. Closing About mid-way stops the work.

A host matches the hash against the `sha256` of every image in the site's `releases.json` for that board. A match
means the radio runs one of our release images, whatever its source line says. No match means a fork, a local build,
a build-service build (its hashes are not in `releases.json`), or a release image changed after the build.
