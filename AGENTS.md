<!-- SPDX-License-Identifier: MIT -->

# Notes for coding agents

ODR / OpenDigitalRadio is a legacy name for this project (now RadioFoundry).

RadioFoundry is MIT-licensed firmware for DMR radios: one tree, many radios. The radios and their chips
(MCU, DMR chip, RF) are in [docs/spec/radios.md](docs/spec/radios.md); a board's platform is `PLATFORM` in
`boards/<radio>/board.mk`. What each radio supports is in [docs/feature-matrix.md](docs/feature-matrix.md); check it
before claiming a radio lacks a feature. Human contributors have the same rules in
[docs/dev/contributing.md](docs/dev/contributing.md). This file is the short version plus what an agent tends to get
wrong.

## Hard rules

1. **Clean room.** Never open, read, search or copy another radio firmware's source (the aftermarket firmware,
   OpenRTX, MMDVMHost or any GPL code), vendor SDKs under restrictive licenses, or disassembly. Work from `docs/spec/`,
   datasheets, public standards (ETSI TS 102 361-1..4, the MMDVM protocol) and permissively licensed code. The full
   rules are in [docs/CLEANROOM.md](docs/CLEANROOM.md). If a task seems to need foreign code, stop and ask.
2. **Never commit** the AMBE codec, stock firmware images, bootloader keys, memory or flash dumps, or audio recordings.
   `codecs/` and `build/` are ignored for that reason. Don't weaken those ignore rules.
3. **SPDX header on every new file**, in the style of its neighbors:
   `// SPDX-License-Identifier: MIT` and `// Copyright (c) 2026 The RadioFoundry Authors` (`#` for scripts and
   makefiles, `<!-- -->` for Markdown).
4. **Generic first.** `core/` never includes a board or platform header and never tests a radio define. Radio
   differences go through `board_t` and the `hal/` interfaces ([docs/dev/architecture.md](docs/dev/architecture.md)).
   Chip drivers go in `drivers/`, SoC code in `platform/`, a radio's wiring in `boards/<radio>/`. The same holds
   outside `core/`: radios are alike, so assume any tool, protocol module, test or doc written for one radio will
   serve another. Name it by what it does or the protocol it speaks, never by the radio that first needed it, and
   don't add a list of radios anywhere: read them from `boards/`.

   **Generic docs** never name a radio or its pins. They are `docs/spec/*` other than `hardware-*`, `docs/user/*`
   other than `23-radios.md`, `docs/build-config.md` and `docs/dev/*`. Describe what a board provides (`CAP_*`,
   `board_t` fields) and link [docs/feature-matrix.md](docs/feature-matrix.md) for which radios have it. A radio's
   facts go only on its own hardware, bring-up or radio-reference page, and those pages link to the shared spec, not
   to another radio's page. `tools/check_generic.py` checks this. Its allowlist names the generic docs not yet
   rewritten, one path each; don't add one.
5. **Flash costs go behind a switch.** A feature that adds real code size gets a `CONFIG_` switch in
   `config/features.mk` and `core/config.h` ([docs/build-config.md](docs/build-config.md)). The build must stay
   clean with the switch off. `BOARD_UNSUPPORTED` only for what the hardware can't do; code not written yet is
   `BOARD_NOT_WRITTEN`. Written but not yet checked on the radio stays on and in neither list: say "untested" in the
   bring-up records and PRs, never in `config.mk` ([the rule](docs/build-config.md#unsupported-not-written-unconfirmed)).
6. **Zero warnings** in every configuration `tools/ci.sh` builds.
7. **US English** in code, comments, docs and commit messages (color, center, behavior).
8. **No hardware without the owner.** Don't touch a connected radio (debug link, DFU, MMDVM port), the SDR bench or any
   service unless the task says so. Nothing transmits unless the test run passes `--allow-rf`
   ([tests/hil/README.md](tests/hil/README.md)).

## Build and test

```
make BOARD=dm1701                    # build/dm1701/rfoundry_dm1701.bin (+ _codec.bin if codecs/ has the stock file)
make BOARD=dm1701 CONFIG_X=n BUILD=build/t-x   # a configuration with a switch off
make -C tests/host                   # host unit and SIL tests
make -C tests/host ui-gallery        # re-render docs/ux/screens after a UI change
JOBS=8 tools/ci.sh                   # everything a pull request must pass; ends with "all checks passed"
python3 -m pytest tests/hil -v -rs   # HIL, radio only; every test skips without a radio
```

Boards on the `hrc7000` platform (C-SKY) need `csky-elf-gcc` (`tools/build-csky-toolchain.sh`); ci.sh reports them
as skipped when it's missing.

## Where things are

The source tree is in [README.md, Layout](README.md#layout). The layers and the core's directories are in
[docs/dev/architecture.md](docs/dev/architecture.md).

## How to work here

- Read the spec for the subsystem first, and its section of [docs/spec/gotchas.md](docs/spec/gotchas.md).
- New behavior gets a host test. A bug fix gets the test that would have caught it. If a HIL test can check it on
  the radio, add one, opt-in as the README describes. Anything a HIL test needs to see goes in a debug link view key
  (`core/debuglink/debuglink_view*.c`). HIL skips use `rfoundry_hil.skip(kind, reason)`, a new feature gets an entry
  in `tests/hil/features.toml`, and its HIL tests a `@pytest.mark.feature("<id>")` marker.
- A behavior change updates its spec in `docs/spec/`. If users see it, it also updates `docs/user/`. Run
  `python3 tools/check_doc_links.py` after editing docs.
- A UI change regenerates the gallery and commits only the PNGs it meant to change.
- A new screen gets a `uiFlowTest` flow that opens it and checks `screen=<name>`. The crawl doesn't reach every
  screen, and one never drawn in a test can crash the radio. A screen without its own `draw` is drawn as a list and
  needs `count` and `item`.
- Guard a definition with the same condition as its callers, not a narrower one. Some switches are turned on by more
  than one feature (`CONFIG_ZONE_WRITE` is zone editing or zone sharing), so a caller under one can outlive the block a
  definition sits in, and the build fails only with one switch off. After touching guarded code, build each switch you
  touched off: `make BOARD=<b> CONFIG_X=n BUILD=build/t-x` (its sub-features follow it, as in `tools/ci.sh`), or
  `tools/ci.sh --board <b> --matrix --steps firmware` for all of them. Remove the `build/t-*` trees afterwards.
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
- **Remove `build/ci` when you're done with CI.** A full `tools/ci.sh` run leaves 5-6 GB there. Keep it while you
  fix a failure and rerun; delete it (`rm -rf build/ci`) once the run passes and you move on to other work.
