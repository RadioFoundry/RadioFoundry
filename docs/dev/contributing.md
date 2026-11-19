<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Contributing

Issues and pull requests are welcome. This page is the one list of rules for every change. Read it and the
[clean-room rules](../CLEANROOM.md) before you start.

- To report a bug, ask for a feature, or ask for a radio, use the issue forms in `.github/ISSUE_TEMPLATE/`.
- To open a pull request, fill in the checklist in `.github/PULL_REQUEST_TEMPLATE.md`.
- To use RadioFoundry code in another project, see [Using RadioFoundry in your project](../../FORKING.md).
- To report a security problem, follow [SECURITY.md](../../.github/SECURITY.md), not a public issue.

## Rules

Every change follows these rules. The pull request template and the review checks refer to them by number.

1. **Clean room.** Write code from the specs in `docs/spec/`, datasheets, public standards (ETSI TS 102 361, the
   MMDVM protocol), and permissively licensed code. Never open, copy, or paraphrase another radio firmware's source,
   GPL code, vendor SDKs under restrictive licenses, or disassembly. For the full process, see
   [Clean-room rules](../CLEANROOM.md).
2. **No restricted files.** Never add the AMBE codec, a manufacturer firmware image, a bootloader key, a flash or
   memory dump, or an audio recording to the repository. Git ignores `codecs/` and `build/` for this reason; don't
   weaken those ignore rules.
3. **SPDX header.** Every new file starts with `SPDX-License-Identifier: MIT` and the copyright line
   `Copyright (c) 2026 The RadioFoundry Authors`, in the comment style of its neighbors. `reuse lint` must not report
   your files.
4. **Generic core.** `core/` never includes a board or platform header and never tests a radio define. Radio
   differences reach it through `board_t` and the `hal/` interfaces (see [Architecture](architecture.md)). A fact
   about a radio that the build doesn't need goes in its `board.toml`, once (see
   [Board registry](board-registry.md)). Tools, tests, and docs read the boards from the registry; none keeps a list
   of radios.
5. **Flash costs go behind a switch.** A feature that adds real code size gets a `CONFIG_` switch, and the build
   stays clean with the switch off. For the steps and for how a board marks a feature it lacks, see
   [Build configuration](../build-config.md#adding-a-feature).
6. **Zero warnings.** Code is C11 (`gnu11`) with `-Wall -Wextra -Wshadow -Wdouble-promotion`, and builds with zero
   warnings in every configuration `tools/ci.sh` builds.
7. **US English** in code, comments, docs, and commit messages: color, center, behavior.
8. **Tests with every code change.** New behavior gets a host test in `tests/host/`, and a bug fix gets the test that
   fails without it. For the details, see [Tests](#tests).
9. **Docs with every behavior change.** A behavior change updates its spec in `docs/spec/`, and the user manual in
   `docs/user/` if users see it. For the details, see [Documentation](#documentation).
10. **Translatable text.** Text a user reads is marked for translation. For the details, see
    [Text the radio or the site shows](#text-the-radio-or-the-site-shows).
11. **One topic, all checks passing.** A pull request covers one topic, and `JOBS=8 tools/ci.sh` ends with
    "all checks passed".

## Code

- Follow the style of the file you edit: tabs, naming, comment density. Comments say why, not what.
- Keep RTOS calls and blocking waits out of the UI's behavior code and the protocol engines, so the host tests can
  run them.
- Before you change a subsystem, read its spec and its section of [Gotchas](../spec/gotchas.md).
- Guard a definition with the same condition as its callers, not a narrower one. Some switches are turned on by more
  than one feature. After you touch guarded code, build with each switch you touched turned off:
  `make BOARD=<b> CONFIG_X=n BUILD=build/t-x`.

## Tests

- Every pull request that changes code includes host tests in `tests/host/` that exercise the change. The pull
  request names the tests. Only pull requests that change nothing but documentation are exempt.
- If the host tests can't reach a change (a register write, a board's wiring), say so in the pull request, cover the
  logic around it with a host test, and add a HIL test when one can be scripted.
- A UI change regenerates the gallery (`make -C tests/host ui-gallery`) and commits only the PNGs it meant to change.
- A new screen gets a `uiFlowTest` flow that opens it and checks `screen=<name>`.
- A parser of outside input gets a fuzz loop (`tests/host/fuzz.h`). Run it under `SANITIZE=1` (see
  [Testing](testing.md)).
- A HIL test skips with `rfoundry_hil.skip(kind, reason)`, never `pytest.skip`. The kind says why (`not_built`,
  `no_hw`, `state`, `opt_in`, `bench`, `other_board`; see [Skip kinds](../../tests/hil/README.md#skip-kinds)).
  `tools/check_hil_skips.py` fails on a skip without a kind.

### The feature checklist

`tests/hil/features.toml` has one entry per feature, with no radio columns and no test lists. Each HIL test names its
feature with `@pytest.mark.feature("<id>")` (see [Bring-up mode](../../tests/hil/README.md#bring-up-mode)).
`tools/bringup.py check`, which `tools/ci.sh` runs, fails when an entry is missing or when a test marks a feature that
has no entry.

| Change | What to do |
|---|---|
| A new switch or feature | Add a `[feature.<id>]` table with `switch`, `spec`, and `source` (when the Makefile's files for the switch aren't the right ones). A feature can have no tests; `tools/bringup.py status` then shows "built, not yet recorded" |
| A new HIL test | Mark it `@pytest.mark.feature("<id>")`. Add `optional=True` when it doesn't gate, `owner="..."` when it asks a person for something, and `gate=True` on the one test that checks the radio offers a feature that needs hardware glue (its capability gate). Its needs come from its markers and fixtures. A required test that isn't written goes in the feature's `planned_tests` |
| A radio lacks a feature | Mark it in the board's `config.mk` or `board.toml` as [Build configuration](../build-config.md#unsupported-not-written-unconfirmed) describes |
| A new radio | Nothing: its states resolve from its build data |

To record a bring-up run, someone at the bench runs `tools/bringup.py list --radio <id>` for the checklist and the
owner actions, then `tools/bringup.py run --radio <id> -- <HIL options>` with the radio on the debug link. The run
takes the radio lock, counts skips without an excuse as failures, and writes a record to
`docs/testing/results/<id>/`. Commit the record. A record can hold failures: they're the to-do list.

## Text the radio or the site shows

The radio and the site are translated. Mark text a user reads: `TR(KEY, "...")` on the radio, `TRN(KEY, "...")` in a
static table, and a catalog key on the site. Code must work when a translation is missing; the English shows. For key
names, catalogs, and checks, see [Translating](translating.md).

- Mark new text, run `python3 tools/i18n.py --sync`, and commit the catalogs with the code.
- Translate new and rewritten strings into every language when you can (`core/ui/lang/*.po` for the radio,
  `site/i18n/*.json` for the site). When you can't, list them as an open task in the pull request
  (`- [ ] translations: es, it`). CI fails on a missing translation, so the translations are done before the merge.
- If you change English text and a translation stays the same, CI warns. Check each language and update it when the
  meaning changed.

## Documentation

- A behavior change updates its spec in `docs/spec/`. If users see it, it also updates the user manual in
  `docs/user/`.
- A new setting goes into the [settings reference](../user/17-settings.md) with its default.
- A new feature switch goes into [Build configuration](../build-config.md) and the
  [feature matrix](../feature-matrix.md).
- Write docs as [Documentation style](doc-style.md) describes.
- `python3 tools/check_doc_links.py` checks every relative link, image, and anchor, and the documentation map.
- `python3 tools/check_keys.py` checks that each codeplug settings area key is listed the same way in the spec, the
  firmware, and the Codeplug Editor (see [codeplug-settings.md](../spec/codeplug-settings.md) 3).

`tools/ci.sh` runs both checks.

## Branches and pull requests

All changes reach `main` through a pull request; nobody pushes to `main` directly. Every pull request goes from a
feature branch into `main`. There are no release or develop branches.

### Repository members

1. Update `main`: `git switch main && git pull`.
2. Create a branch named as [Branch names](#branch-names) describes: `git switch -c feat/aprs-path-menu`.
3. Commit, then push the branch to this repository: `git push -u origin feat/aprs-path-menu`.
4. Open the pull request into `main`: `gh pr create --base main`.
5. After the merge, delete the branch.

### Everyone else

1. Fork the repository on GitHub.
2. In your fork, create a branch from an up-to-date `main`, named as [Branch names](#branch-names) describes.
3. Push the branch to your fork, and open the pull request against `main` of this repository.
4. To keep the branch current, merge or rebase on this repository's `main`. Add it as the `upstream` remote.

### Branch names

Name a branch `<type>/<area>-<short-description>`: lowercase, words joined by hyphens, under about 40 characters, with
no personal names or dates.

| Type | For | Example |
|---|---|---|
| `feat` | New behavior | `feat/aprs-path-menu` |
| `fix` | A bug fix; add the issue number when there is one | `fix/123-dmr-late-entry` |
| `radio` | A new radio, or the bring-up of a radio that builds | `radio/<id>-bringup` |
| `test` | Tests only (host or HIL) | `test/sfr-dedup-cases` |
| `docs` | Documentation only | `docs/contributing-branches` |
| `refactor` | No change in behavior | `refactor/ui-svc-storage` |
| `tools` | Host tools, CPS, scripts | `tools/rfoundry-retry` |
| `ci` | The CI workflows and `tools/ci.sh` | `ci/nightly-matrix` |

The area is the subsystem, as in commit subjects: `aprs`, `dmr`, `fm`, `sfr`, `ui`, `hotspot`, `net`, `cps`, or a
board ID for one radio's code.

### Pull request titles

Pull requests are squash merged, so the title becomes the commit subject on `main`. Write it as
`<Area>: <what changed>`, in the present tense, with no closing period:

- `APRS: a Path row on the APRS page`
- `<Model>: FM receive audio for the AFSK demodulator` (one radio's change, under its `board.toml` model)
- `Fix: SFR repeats a late-entry call's first voice burst`

### Before you open a pull request

1. Commit in small steps, with messages that say what changed and why.
2. Run the whole gate:

   ```sh
   JOBS=8 tools/ci.sh
   ```

   It must end with "all checks passed". The script reports what it skips, such as a missing compiler or `reuse`;
   install what you can. CI runs the quick part on every push and the feature matrix nightly (see
   [Continuous integration](ci.md)), so run the whole gate yourself first.
3. Open the pull request into `main` and fill in the template.

## License of contributions

RadioFoundry has no contributor license agreement, and you keep the copyright in what you write. By submitting a
contribution (a pull request, a patch, or text or code that ends up in the tree), you do the following:

- You license it irrevocably under the project's MIT license ([LICENSE](../../LICENSE)), the same terms everyone
  receives it under.
- You confirm that you wrote it, or otherwise have the right to submit it under that license (with your employer's
  permission, if your employer owns your work), and that it meets the [clean-room rules](../CLEANROOM.md).
- You agree that it's credited to "The RadioFoundry Authors". Your name is in the git history from the merge and in
  [CONTRIBUTORS](../../CONTRIBUTORS) from the next release. [AUTHORS](../../AUTHORS) lists the maintainers only. To
  change how your name appears, add a line to `.mailmap`.

Your copyright covers the code you wrote. It doesn't extend to the rest of the project, to other contributors' work,
or to the RadioFoundry name. The maintainers decide what is merged and released.

## How you can help besides code

- **Test and report.** Run RadioFoundry on your radio and
  [report what breaks](https://github.com/radiofoundry/radiofoundry/issues/new?template=bug_report.yml). If the radio
  restarted, include the crash log from **Settings > Advanced > Crash log** (see
  [Settings](../user/17-settings.md#crash-log)).
- **Ask.** [Request a feature](https://github.com/radiofoundry/radiofoundry/issues/new?template=feature_request.yml)
  or [a radio](https://github.com/radiofoundry/radiofoundry/issues/new?template=radio_request.yml).
- **Donate or lend a radio.** Every port needs the hardware on the bench, so this helps the most, especially with
  older models that are hard to find.
