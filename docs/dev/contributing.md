<!-- SPDX-License-Identifier: MIT -->

# Contributing

Issues and pull requests are welcome. Read this page and [docs/CLEANROOM.md](../CLEANROOM.md) first. New issues use
the forms in `.github/ISSUE_TEMPLATE/` (bug, feature, new radio), and pull requests open with the checklist in
`.github/PULL_REQUEST_TEMPLATE.md`.

## Branches and pull requests

All changes reach `main` through a pull request. Nobody pushes to `main` directly. For now every pull request goes
from a feature branch into `main`; there are no release or develop branches.

### Repository members

1. Update `main`: `git switch main && git pull`.
2. Make a branch named as below: `git switch -c feat/aprs-path-menu`.
3. Commit, then push the branch to this repository: `git push -u origin feat/aprs-path-menu`.
4. Open the pull request into `main` (`gh pr create --base main`).
5. When it is merged, delete the branch.

### Everyone else

1. Fork the repository on GitHub.
2. In your fork, branch from an up to date `main` with the same naming rules.
3. Push the branch to your fork and open the pull request against `main` of this repository.
4. Keep the branch current by merging or rebasing on this repository's `main` (add it as the `upstream` remote).

### Branch names

`<type>/<area>-<short-description>`: lower case, words joined by hyphens, under about 40 characters, no personal
names or dates.

| Type | For | Example |
|---|---|---|
| `feat` | new behavior | `feat/aprs-path-menu` |
| `fix` | a bug fix; add the issue number when there is one | `fix/123-dmr-late-entry` |
| `radio` | a new radio, or bring-up of a radio that builds | `radio/md9600-bringup` |
| `test` | tests only (host or HIL) | `test/sfr-dedup-cases` |
| `docs` | documentation only | `docs/contributing-branches` |
| `refactor` | no change in behavior | `refactor/ui-svc-storage` |
| `tools` | host tools, CPS, scripts | `tools/odrflash-retry` |
| `ci` | the CI workflows and `tools/ci.sh` | `ci/nightly-dm32` |

The area is the subsystem, matching the commit subjects: `aprs`, `dmr`, `fm`, `sfr`, `ui`, `hotspot`, `net`, `cps`,
`dm1701`, and so on.

### Pull request titles

Pull requests are squash merged, so the title becomes the commit subject on `main`. Write it like a commit
subject: `<Area>: <what changed>`, in the present tense, no trailing period.

- `APRS: a Path row on the APRS page`
- `DM-1701: FM receive audio for the AFSK demodulator`
- `Fix: SFR repeats a late-entry call's first voice burst`

### What a pull request must have

- **Unit test coverage.** Every pull request that changes code includes host tests in `tests/host/` that exercise
  the change: new behavior gets new tests, a bug fix gets the test that fails without it. The pull request names
  the tests. A change the host tests can't reach (a register write, a board's wiring) says so, covers the logic
  around it with a host test, and adds a HIL test when one can be scripted. Pull requests that change only
  documentation are the only exception.
- `JOBS=8 tools/ci.sh` ending with "all checks passed".
- One topic. Split unrelated changes into separate pull requests.
- The filled-in template (`.github/PULL_REQUEST_TEMPLATE.md`).

## Clean room

ODR is MIT licensed and must stay free of code it may not use. The rules are in [docs/CLEANROOM.md](../CLEANROOM.md).
The short version:

- Write code from the prose specs in `docs/spec/`, datasheets, public standards (ETSI TS 102 361, the MMDVM protocol)
  and permissively licensed libraries. Never from another radio firmware's source.
- Don't open, copy or paraphrase GPL or non-commercial firmware, vendor SDKs or disassembly while writing code
  for this repository. If you have read them, write specs (facts, not code) and let someone else implement.
- Never add the AMBE codec, a stock firmware image, a bootloader key or a memory dump to the repository.
- Every file carries an SPDX header (`SPDX-License-Identifier: MIT` and the copyright line, in the style of its
  neighbors). `reuse lint` must not report your files (the whole tree doesn't pass yet, [CI](ci.md)).
- Test vectors come from standards, from your own captures, or from the stock codec's output on your own radio.

The checks, all in `tools/ci.sh`: `tools/license_grep.sh` (markers of foreign code), `reuse lint` (when installed),
and before a release a similarity scan against the reference trees (only when `ODR_SIMILARITY_REFS` is set; run by
the project lead, never in public CI).

## Code

- `core/` stays radio independent. Radio differences go through `board_t` and the `hal_*` interfaces
  ([Architecture](architecture.md)).
- Anything that costs flash goes behind a `CONFIG_` switch ([Building](building.md)).
- C11 (gnu11) with `-Wall -Wextra -Wshadow -Wdouble-promotion`. Zero warnings in every configuration.
- Follow the style of the file you edit. Comments say why, not what.
- Keep RTOS calls and blocking waits out of the UI's behavior code and the protocol engines, so the host tests can run them.
- Read the section of [gotchas.md](../spec/gotchas.md) for the subsystem you change.

## Tests

- Every pull request that changes code includes host unit tests for it ([What a pull request must have](#what-a-pull-request-must-have)).
- New behavior gets a host test. A bug fix gets the test that would have caught it.
- A UI change updates the gallery (`make -C tests/host ui-gallery`) and commits the PNGs it meant to change.
- A parser of outside input gets a fuzz loop (`tests/host/fuzz.h`); run it under `SANITIZE=1` ([Testing](testing.md)).
- A change that touches hardware gets a HIL test when it can be scripted ([Testing](testing.md)).
- A HIL test skips with `odr_hil.skip(kind, reason)`, never `pytest.skip`. The kind says why (`not_built`, `no_hw`,
  `state`, `opt_in`, `bench`, `other_board`; [tests/hil/README.md](../../tests/hil/README.md#skip-kinds)), and
  `tools/check_hil_skips.py` fails on a skip without one.

### The feature checklist

`tests/hil/features.toml` has one entry per feature, no radio columns and no test lists: each HIL test names its
feature with `@pytest.mark.feature("<id>")` ([tests/hil/README.md](../../tests/hil/README.md#bring-up-mode)).
`tools/bringup.py check` (in `tools/ci.sh`) fails when an entry is missing or a test marks a feature that has none.

| Change | What to do |
|---|---|
| A new switch or feature | Add a `[feature.<id>]` table: `switch`, `spec`, and `source` when the Makefile's files for the switch aren't the right ones. A feature may have no tests yet; `tools/bringup.py status` then shows "built, not yet recorded" |
| A new HIL test | Mark it `@pytest.mark.feature("<id>")`, with `optional=True` when it shouldn't gate, `owner="..."` when it asks a person for something, and `gate=True` on the one test that checks the radio offers a feature that needs hardware glue (its capability gate). Its needs come from its markers and fixtures. A required test not written yet goes in the feature's `planned_tests` |
| A radio lacks a feature | Add the switch to the board's `BOARD_UNSUPPORTED` in `config.mk`, one a line with the reason: `BOARD_UNSUPPORTED += AM_RX  # no air band receiver` (state `n/a`). Planned but not working yet, where the hardware could: `board.toml` `[features] <id> = { state = "unsupported", reason = "..." }` |
| A new radio | Nothing here: its states resolve from its build data |

To record a bring-up run, the owner (or someone at the bench) runs `tools/bringup.py list --radio <id>` for the
checklist and owner actions, then `tools/bringup.py run --radio <id> -- <HIL options>` with the radio on the debug
link. The run takes the radio lock, counts unexcused skips as failures and writes a record to
`docs/testing/results/<id>/`. Commit the record. Failures are allowed in a record: they are the to-do list.

## Documentation

- A behavior change updates its spec in `docs/spec/` and, if users see it, the user manual in `docs/user/`.
- A new setting goes into [Settings](../user/12-settings.md) with its default and status.
- A new feature switch goes into [build-config.md](../build-config.md) and [feature-matrix.md](../feature-matrix.md).
- Docs use US English, short sentences and tables. Links are relative so they work on GitHub and in the PDF.
- `python3 tools/check_doc_links.py` checks every relative link, image and anchor in the docs (ci.sh runs it).

## Before you open a pull request

```
JOBS=8 tools/ci.sh
```

It must end with "all checks passed". Skips (a missing compiler, `reuse` not installed) are reported; install what you
can. CI runs the quick part on every push and the feature matrix nightly ([CI](ci.md)); run the whole gate before you
open the pull request. Commit in small steps with messages that say what changed and why. Then open the pull request
into `main` as [Branches and pull requests](#branches-and-pull-requests) describes.
