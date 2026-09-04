<!-- SPDX-License-Identifier: MIT -->

# Notes for coding agents

OpenDigitalRadio (ODR) is MIT-licensed firmware for DMR radios: one tree, many radios. The DM-1701 (STM32F405 +
HR_C6000 + AT1846S) is the radio on the bench. The DM-32UV (CK803S/HR_C7000) builds but hasn't been brought up.
Human contributors have the same rules in [docs/dev/contributing.md](docs/dev/contributing.md). This file is the
short version plus what an agent tends to get wrong.

## Hard rules

1. **Clean room.** Never open, read, search or copy another radio firmware's source (the legacy OpenGD77-family trees,
   OpenRTX, MMDVMHost or any GPL code), vendor SDKs under restrictive licenses, or disassembly. Work from `docs/spec/`,
   datasheets, public standards (ETSI TS 102 361-1..4, the MMDVM protocol) and permissively licensed code. The full
   rules are in [docs/CLEANROOM.md](docs/CLEANROOM.md). If a task seems to need foreign code, stop and ask.
2. **Never commit** the AMBE codec, stock firmware images, bootloader keys, memory or flash dumps, or audio recordings.
   `codecs/` and `build/` are ignored for that reason. Don't weaken those ignore rules.
3. **SPDX header on every new file**, in the style of its neighbors:
   `// SPDX-License-Identifier: MIT` and `// Copyright (c) 2026 The OpenDigitalRadio Authors` (`#` for scripts and
   makefiles, `<!-- -->` for Markdown).
4. **Generic first.** `core/` never includes a board or platform header and never tests a radio define. Radio
   differences go through `board_t` and the `hal/` interfaces ([docs/dev/architecture.md](docs/dev/architecture.md)).
   Chip drivers go in `drivers/`, SoC code in `platform/`, a radio's wiring in `boards/<radio>/`.
5. **Flash costs go behind a switch.** A feature that adds real code size gets a `CONFIG_` switch in
   `config/features.mk` and `core/config.h` ([docs/build-config.md](docs/build-config.md)). The build must stay
   clean with the switch off.
6. **Zero warnings** in every configuration `tools/ci.sh` builds.
7. **US English** in code, comments, docs and commit messages (color, center, behavior).
8. **No hardware without the owner.** Don't touch a connected radio (debug link, DFU, MMDVM port), the SDR bench or any
   service unless the task says so. Nothing transmits unless the test run passes `--allow-rf`
   ([tests/hil/README.md](tests/hil/README.md)).

## Build and test

```
make BOARD=dm1701                    # build/dm1701/odr_dm1701.bin (+ _codec.bin if codecs/ has the stock file)
make BOARD=dm1701 CONFIG_X=n BUILD=build/t-x   # a configuration with a switch off
make -C tests/host                   # host unit and SIL tests
make -C tests/host ui-gallery        # re-render docs/ux/screens after a UI change
JOBS=8 tools/ci.sh                   # everything a pull request must pass; ends with "all checks passed"
python3 -m pytest tests/hil -v -rs   # HIL, radio only; every test skips without a radio
```

The DM-32 needs `csky-elf-gcc` (`tools/build-csky-toolchain.sh`). ci.sh reports it as skipped when it's missing.

## Where things are

| Path | What |
|---|---|
| `core/` | Radio-independent code: DMR stack (`core/dmr`), radio state machine (`core/radio`), UI (`core/ui`), codeplug, settings, CPS, hotspot, debug link |
| `hal/` | The interfaces the core calls (`hal_dmr_modem.h`, `hal_vocoder.h`, `hal_display.h`, ...) |
| `drivers/`, `platform/`, `boards/` | Chip drivers, SoC support, one directory per radio |
| `docs/spec/` | Behavior, hardware facts and protocols; `gotchas.md` lists past bugs by subsystem |
| `docs/ux/` | UI principles, flows, the screenshot gallery |
| `docs/user/`, `docs/dev/` | User manual (built to PDF on release tags), developer docs |
| `tests/host/` | Host tests, the UI simulator (`tests/host/sim`) |
| `tests/hil/` | pytest against a real radio and the SDR bench |
| `tools/` | odr_debug.py (debug link client), odrflash, codec tools, CI, bench tools |
| `third_party/` | Git submodules (FreeRTOS, ST HAL/CMSIS, TinyUSB, ...) |

## How to work here

- Read the spec for the subsystem first, and its section of [docs/spec/gotchas.md](docs/spec/gotchas.md).
- New behavior gets a host test. A bug fix gets the test that would have caught it. If a HIL test can check it on
  the radio, add one, opt-in as the README describes. Anything a HIL test needs to see goes in a debug link view key
  (`core/debuglink/debuglink_view*.c`).
- A behavior change updates its spec in `docs/spec/`. If users see it, it also updates `docs/user/`. Run
  `python3 tools/check_doc_links.py` after editing docs.
- A UI change regenerates the gallery and commits only the PNGs it meant to change.
- Match the file you're editing: tabs, naming, comment density. Comments say why.
- Keep RTOS calls and blocking waits out of protocol engines and UI behavior code, so the host tests can run them.
- Docs read like technical documentation: short sentences, tables, no filler.

## Git

- Small commits that say what changed and why.
- `third_party/*` are submodules (mode 160000). In a worktree where they're symlinked to another checkout, never
  stage them: check `git diff --cached --stat -- third_party` before every commit. ci.sh fails if one became a
  symlink or a plain directory.
- Never force-push, rewrite published history or run destructive cleanup on a shared checkout.
- **Clean up after yourself.** When a worktree's branch is merged, remove the worktree and its `build/` directories.
  Builds take about 1.5 GB per worktree, and stale ones have filled the disk before.
