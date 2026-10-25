<!-- SPDX-License-Identifier: MIT -->

# Continuous Integration

What runs automatically, when, and how to run each piece by hand. The runners and the container image are in
[ci-runners.md](ci-runners.md). Every CI job calls the same
scripts a developer runs locally (`tools/ci.sh`, `tools/coverage.sh`), so the YAML holds only setup and triggers.
The workflows are in `.github/workflows/`; Gitea Actions runs them from there too (there is no `.gitea/workflows`),
so one set serves the self-hosted Gitea runner and GitHub.

| Workflow | Runs on | When | What |
|---|---|---|---|
| `ci.yml` | `ubuntu-24.04` | every push to `main` that changes source (below); by hand on any branch | the unit tests (once, with coverage and sanitizers), Python and license checks, every board's default build |
| `pr-tests.yml` | `ubuntu-24.04` | every pull request, from `main`'s copy of the workflow | `ci.yml`'s jobs and the manual check, for the files the pull request changes; `PR checks` gates the merge. A branch's only CI |
| `nightly.yml` | `ubuntu-24.04` | 07:17 UTC daily when a file the firmware builds read changed since the last pass; by hand | the feature matrix of every board, `reuse lint` |
| `site.yml` | `ubuntu-24.04` | a published release; by hand | asks Cloudflare Pages to build `main` again, for the new release's images. Pages itself builds every push ([website.md](website.md)); pull requests also build the site in the unit tests job |
| `hil.yml` | `rfoundry-hil` (the bench Mac) | by hand; nightly only when `RFOUNDRY_HIL_SCHEDULE=true` | `tests/hil` against the radio and the SDR bench |
| `ui-gallery.yml` | `ubuntu-24.04` | a pull request that changes the UI (`core/ui/`, the simulator, `uiSim`) | renders the screen gallery at the base and at the pull request, and makes the side-by-side artifact |
| `ui-gallery-comment.yml` | `ubuntu-24.04` | when `ui-gallery.yml` finishes on a pull request | posts the pull request's gallery comment with the changed screens attached |
| `manual.yml` | `ubuntu-24.04` | docs changes pushed to `main`, `v*` tags | the user manual PDF ([manual-pdf.md](manual-pdf.md)) |
| `release.yml` | `ubuntu-24.04`, and for rfoundry the hosted macOS / Windows runners | `v*` tags; pull requests that change the release tooling (packaging only, no release gate) | release images and rfoundry ([release.md](release.md)) |

On GitHub, `ci.yml` and `nightly.yml` run only when the repository variable `RFOUNDRY_CI_GITHUB` is `true` (on a
private copy, hosted runners use paid minutes). On Gitea they always run.

The `ubuntu-24.04` in the table is the default. When the repository variable `RFOUNDRY_CI_RUNNER` is set (to `rfoundry-ci`),
every Linux job goes to the self-hosted runner with that label instead
([runners](ci-runners.md#the-github-self-hosted-runner)). The runner takes only `main`'s and release tags' workflows
([which jobs it takes](ci-runners.md#which-jobs-it-takes)), so a pull request's `pull_request` jobs and a fork's
`pr-tests.yml` jobs stay on GitHub's runners.

Each change is checked once. A branch is tested through its pull request (`pr-tests.yml`), and `main` again after
the merge (`ci.yml`). The unit tests run once per check, with the coverage counters and the sanitizers in the same
build. The unit tests job's Python step builds the site and runs the flasher's tests, and the release gate runs for a tag.

## On main: `ci.yml`

Triggered by changes under `boards/`, `config/`, `core/`, `drivers/`, `hal/`, `platform/`, `tests/`, `tools/`,
`third_party/` (submodule bumps), the `Makefile`, `AUTHORS` (the About page), `.gitmodules`, `LICENSES/`,
`REUSE.toml`, `.github/actions/` or the workflow itself, pushed to `main`; by hand (Actions, CI, Run workflow) on any branch. A docs-only
push runs only the manual workflow. A newer push cancels the run in progress (`concurrency`).

| Job | Command |
|---|---|
| `unit tests` | `SANITIZE=1 tools/coverage.sh` (the host tests with the tshark pcap checks, built once with `--coverage` and ASan / UBSan, and gcovr's report), then the UI screen gallery (`uiSim --gallery`) |
| `checks` | `tools/ci.sh --steps registry,python,licenses` (doc links among the Python checks) |
| `firmware (Arm)` | `tools/ci.sh --quick --steps firmware --toolchain arm`: every board built with the Arm toolchain, in one job |
| `firmware (C-SKY)` | `tools/ci.sh --quick --steps firmware --toolchain csky`, in the csky-elf image (`tools/docker/csky-toolchain/run.sh`) unless the runner has csky-elf GCC; skipped where the image isn't available |

There is one firmware job per toolchain, not one per radio or per platform: a job's checkout and start take longer
than a radio's build, and a runner has 4 cores, so the two firmware jobs side by side keep an 8-core host busy while
leaving runners free for other pull requests. The two are one job with a `toolchain` matrix: the `[toolchain] id` of
`platform/*/platform.toml` (`arm`, `csky`) and its `name` for the job's name. `--toolchain` takes the boards from the
registry (`tools/boards.py list --toolchain`), so a new radio, or a new platform on a toolchain already listed (a
second Arm SoC, for example), joins CI, the nightly and the release with no workflow edit. Only a new toolchain
goes in the matrices of `ci.yml`, `pr-tests.yml`, `nightly.yml` and `release.yml` and gets its set-up in the
toolchain action ([below](#toolchains)). The jobs run in parallel.

Each firmware job builds with zero warnings, writes the size report (image bytes and free space in the linker's
application region, also put in the job summary on GitHub) and fails when the application region overflows. The
images and logs are workflow artifacts (`firmware-arm`, `firmware-csky`), as are the unit test logs with the screen gallery
(`host-logs`) and the coverage report (`coverage-report`).

`reuse lint` is part of the `checks` job only when the repository variable `RFOUNDRY_CI_REUSE` is `true`; it doesn't pass on
the current tree yet (run it to see the files), and the nightly reports it as a warning meanwhile.

## Submodules

The jobs that build firmware check out without submodules, then run `tools/checkout-submodules.sh`. The self-hosted
runner image keeps bare clones of the `third_party/` repositories in `/opt/rfoundry-git` (`RFOUNDRY_GIT_CACHE`), and the script
borrows their objects (`git submodule update --reference`), so a job fetches from GitHub only the commits newer than
the image. A submodule the image lacks, or a runner without the image, takes a plain network checkout. Each submodule
gets three tries, so one dropped connection to GitHub no longer fails the job.

The nested submodules of FreeRTOS-Kernel (its partner and community ports) aren't checked out: no build uses them,
since the CK803 port is in `platform/hrc7000/freertos/`. After a submodule bump, rebuild the runner image to bring the
clones up to date ([tools/ci-runner/README.md](../../tools/ci-runner/README.md)); until then the new commits come over
the network. A new submodule also goes in the Dockerfile's `SUBMODULE_URLS`.

## Pull request checks: `pr-tests.yml`

Every pull request runs `ci.yml`'s jobs (`unit tests`, `firmware (Arm)` and `firmware (C-SKY)` for every board
built with each toolchain) and `checks` on its merge commit. The first job,
`changes`, looks at the files the pull request changes and skips what they don't touch: source changes (`ci.yml`'s
paths) run the build and test jobs. `checks` runs on every pull request: the Python and license checks (doc links
among them) always, and the PDF manual build (as `manual.yml` does) when docs change (`manual.yml`'s paths). The last job always runs and sets the
`PR checks` commit status on the pull request's head commit: failure when any job failed or was canceled, success
otherwise. It is the one status check the `main` ruleset requires, so a skipped job never holds a pull request up,
and a new job only has to be added to its `needs` list. The job posts the status itself because a
`pull_request_target` run's own checks belong to `main`'s commit, where the ruleset doesn't look.

The workflow runs on `pull_request_target`, so GitHub takes it from `main`, never from the pull request: a
contributor's edited workflow doesn't run here, and the checks run on every pull request without anyone approving
them. `ci.yml` and `manual.yml` have no `pull_request` trigger for the same reason, and `ci.yml` doesn't run on a
branch's pushes either, so a pull request is checked once.

The pull request's code runs with a read-only token that isn't stored in the checkout, no secrets and no cache, so
it gets nothing a fork couldn't have. The image's submodule clones aren't a cache it could write to: each job gets a
new container, so whatever a job changes is gone when it ends. The firmware jobs download the toolchain each time (checksummed by
`tools/install-arm-toolchain.sh`, about 50 s, as every workflow does). They run `main`'s copy of
the toolchain action, checked out from the workflow's own commit into `.ci-main/`, never the pull request's. It runs
before any of the pull request's code, so the C-SKY job logs in to the image registry with the job's own token
(`packages: read`), pulls the image and logs out before a script from the pull request runs. The status job is the only one with a write
permission (`statuses: write`), and it checks out nothing. Keep it that way: never add secrets, another write
permission, `actions/cache` or the pull request's text (title, body, branch name) in a run step to `pr-tests.yml`.
Changes to `.github/` need the owner's review (`.github/CODEOWNERS`, with code owner review required in the `main`
ruleset), since `main`'s workflows are the ones that run.

"No cache" means the jobs restore none. The pull request's code can still write one: a `pull_request_target` run
has `main`'s cache scope, and its runner holds the token for it. A restore unpacks with absolute paths, so a poisoned
entry can write any file the job's user can, the job's actions and scripts included. So nothing that builds firmware,
packages a release or holds a write permission restores a cache. The toolchains are downloaded and checked against
the pin every time ([Toolchains](#toolchains)). The one cache left is the nightly run's `nightly-ok-*` marker, in a
job with a read-only token that builds nothing and only decides whether the night's matrix runs. A forged entry can
skip a night or run code with what the pull request's code already had.

Every action is pinned to a commit, with its release in a comment. Dependabot (`.github/dependabot.yml`) proposes
newer commits monthly, as pull requests for the owner's review. Use `actions/*` only, pinned the same way: the
repository's Actions settings allow GitHub's own actions and require full-length commit SHAs.

None of `main`'s workflows runs on a branch's pushes, but a branch that gives its own copy of a workflow a `push`
trigger runs it. Only the owner's review keeps a changed workflow off `main`; it doesn't stop it from running on that
branch.

## UI gallery: `ui-gallery.yml` and `ui-gallery-comment.yml`

A pull request that touches the UI (`core/ui/`, `core/radio/tones.c`, `core/codeplug/codeplug_talk.c`,
`tests/host/uiSim.c`, `tests/host/sim/`, `AUTHORS`, the gallery's own tools) gets a comment with its changed screens,
before and after, side by side. It's advisory: no required check waits on it.

1. `ui-gallery.yml` (`pull_request`) builds `uiSim` from the base commit and from the pull request's merge commit,
   renders `--gallery` for both, and writes `tools/ui_gallery_compare.py`'s output: for each display class with a changed
   screen, one composite per eight rows, stacked as before | after pairs (2x, or 1x for a class whose widest row would
   pass 1800 px; labels with both commit ids; a screen that's new or gone gets a gray panel), plus `manifest.json`. A class with no change
   gets no picture. Classes are ordered monochrome first, then color, each by resolution, and the comment groups them
   under those headings. The render is deterministic, so an unchanged screen never shows up.

   The gallery's color pages are drawn once more in each highlight color the workflow's `UI_GALLERY_COLORS` names
   (`env` at the top of `ui-gallery.yml`: one line, in double quotes, any of `red orange yellow teal light_blue purple
   pink`; three for now). Theme pages are also drawn in the light and high-contrast themes (`<screen>__light`,
   `<screen>__high_contrast`). When a color page or any of its color or theme shots changed, the comparison shows the
   whole set as one row of the composite: a column per shot, the page first, then its colors in the list's order and
   its themes, before above after, the unchanged ones marked so. Such a row counts as two of the eight.
   `make -C tests/host ui-gallery` reads the same line, so the committed gallery shows the same colors. Changing the
   list is a pull request that changes the workflow and regenerates the gallery.
2. `ui-gallery-comment.yml` (`workflow_run`, from main's copy) downloads that artifact and checks it as data (no links,
   file names and sizes, PNG signatures, the pull request and commit it names). It pushes each
   composite to the `ui-previews` branch (made on first use, one `pr-N/` folder per pull request holding its newest commit) and
   linked with `blob/...?raw=true` URLs, which render for signed-in viewers of a private repository (`gh --attach` needs a
   user session, not the job's token). The branch never grows: every run rebuilds it as one parentless commit and force-pushes it (it holds only
   pictures that can be regenerated), keeping the open pull requests, the newest 5 closed without a merge and the newest
   20 merged, among those that have pictures; older folders are dropped, and GitHub collects their objects. A pull
   request's folder holds only its newest commit: the pictures of its earlier commits are removed. It then edits the
   pull request's comment in place, so it stays where it was near the top of the conversation; only if the edit fails
   does it post a new comment and delete the old one. Pictures of a pull request that fell out of the window no longer
   load. A composite of more than 3 rows is folded in a `<details>` block under its list of changed screens. The
   comment is found by its marker line (`<!-- rfoundry-ui-gallery -->`) and carries the commit, the UTC time of the update
   and the base commit. A run for a commit the pull request has already moved past posts nothing.

Only the comment job has write permissions (`pull-requests: write`, and `contents: write` for `ui-previews` alone). It runs no code from the pull request or the
artifact, the same split `pr-tests.yml` keeps for its status job. An artifact with more than 40 pictures fails the check.

Nothing else is needed: no branch, no host. Attachments on a private repository are visible to the people who can
see the pull request; the first run on a UI pull request confirms that and that `gh` accepts the attachments with the
job's token. `workflow_run` runs only from the default branch's copy of the workflow, and Gitea Actions may not
support it, so check the first run on the Gitea runner before relying on the comment there.

## Nightly: `nightly.yml`

| Job | Command |
|---|---|
| `matrix (Arm)` | `RFOUNDRY_CI_PRUNE=1 tools/ci.sh --matrix --steps firmware --toolchain arm`: for each board, each switch off alone, fm-minimal, maximal |
| `matrix (C-SKY)` | the same for every board built with csky-elf GCC, in the csky-elf image unless the runner has csky-elf GCC |
| `reuse` | `reuse lint`, a warning until it passes |

The maximal build turns on every switch except the board's `BOARD_UNSUPPORTED` (the hardware can't),
`BOARD_NOT_WRITTEN` (the code isn't written for the radio yet) and `MAXIMAL_SKIP` lists, and `CONFIG_AUDIO_PROBE`
([build-config.md](../build-config.md#unsupported-not-written-unconfirmed)). Not-written switches stay off because
they add code that doesn't run on the radio but still takes its flash and RAM, so a shared addition could push one
radio over and fail CI on unrelated pull requests.

One job per toolchain runs its boards one after another (artifacts `matrix-arm`, `matrix-csky`). A job per board would finish each board sooner, but it
repeats the checkout, the submodules and the toolchain set-up for every board, and a 4-core runner is kept busy by
one board's builds anyway. Nobody waits on the nightly, so the fewer jobs win.

`--matrix` leaves out the default builds the per-push run already made; `RFOUNDRY_CI_PRUNE=1` removes each build tree
once its size is recorded (the logs and images stay), which keeps the runner's disk from filling with a lot of disk
per configuration.

**Only when something changed.** A successful run saves a cache entry named `nightly-ok-<commit>` that holds its
commit. A scheduled run restores the newest such entry and diffs the default branch against that commit:

| Changed since the last pass | Matrix | `reuse lint` |
|---|---|---|
| Nothing (or this commit already passed) | skipped | skipped |
| Only files the builds don't read (docs, site, tests) | skipped; this commit is saved as passed | runs |
| A file the builds read: `Makefile`, `.gitmodules`, `boards/`, `boot/`, `config/`, `core/`, `drivers/`, `hal/`, `platform/`, `third_party/`, `tools/`, `.github/actions/`, `nightly.yml` | runs | runs |

The check costs one short job. It works the same on Gitea and GitHub without an API token or comparing commit
times against the schedule, and catches up after nights the runner was off. A failing commit is never saved, so
the next night still diffs against the last pass and builds again until the failure is fixed. If the cache evicts
an entry, or holds only entries from before this scheme, the cost is one repeated run. A manual run (Actions,
Nightly, Run workflow, or `gh workflow run nightly.yml`) always builds.

## Running the pieces locally

```
tools/ci.sh                                   # everything (the release gate)
tools/ci.sh --quick                           # what CI runs, without coverage and sanitizers
SANITIZE=1 tools/coverage.sh                  # the unit tests job's test run
tools/ci.sh --steps registry,python,licenses  # the checks job
tools/ci.sh --quick --steps firmware --board <id>
tools/ci.sh --quick --steps firmware --toolchain arm         # a firmware CI job: every board on the toolchain
tools/ci.sh --quick --steps firmware --platform stm32f4      # every board on one platform
make BOARD=<id> stack-check                  # the stack check alone, after a build (stack-check.md)
RFOUNDRY_CI_PRUNE=1 tools/ci.sh --matrix --steps firmware --toolchain arm    # a nightly matrix job
tools/coverage.sh                             # build/coverage/index.html
tools/docker/csky-toolchain/run.sh tools/ci.sh --quick --steps firmware --board <id>   # on a host with the image
```

`--steps` takes any of these, comma separated (default all):

| Step | What it checks |
|---|---|
| `registry` | Every `board.toml` and `platform.toml` is well formed and agrees with `board.mk`, the linker scripts and `board_t`; `tools/rfoundry/rfoundry/registry.json` is current; the loader's self-test (`tools/boards.py`) |
| `host` | `make -C tests/host`, with each board's own tests (`boards/<b>/tests/host.mk`) |
| `python` | Every Python file parses, the HIL test collection, rfoundry's tests, every tool's `--self-test` (found by searching `tools/` and `stub/`, so a new tool needs no ci.sh change), doc links, the glossary, the web site build and the flasher's tests |
| `firmware` | Every board whose compiler is installed: the default build, each build option value (`board.toml` `[[build_options]]`, such as a band), each `[ci] extra` configuration, the feature matrix, and the flash stubs of each built platform (`platform.toml` `stubs`) |
| `licenses` | `tools/license_grep.sh` and `reuse lint` |
| `similarity` | `tools/similarity_scan.py`, only with `RFOUNDRY_SIMILARITY_REFS` set |

Every step but `host` needs Python 3.11 or later for the registry's TOML. `tools/rfoundry_python.sh` finds it:
`$RFOUNDRY_PYTHON`, else the newest `python3.N` on `PATH`. A board whose compiler is missing is skipped with the command
that installs it (its platform's `toolchain.build`). `--quick` and `--matrix` choose the firmware configurations.

The `python` step also checks the HIL tests without a radio:

| Check | Fails when |
|---|---|
| HIL test collection | pytest can't collect `tests/hil` (skipped without pytest) |
| HIL skips (`tools/check_hil_skips.py`) | a skip in `tests/hil` doesn't name its kind (`rfoundry_hil.skip(kind, reason)`) |
| HIL skip helper tests (`tests/hil/test_skip_kinds.py`) | the kinds don't reach the report and the JUnit XML |
| Feature checklist (`tools/bringup.py check`) | a switch has no entry in `tests/hil/features.toml`, a test's feature marker names a feature that has none, a `manual` test has no owner action, an `n/a` or `unsupported` state has no reason, a record in `docs/testing/results/` doesn't parse or lacks a key (a missing or stale record never fails) |
| Bring-up mode tests (`tests/hil/test_bringup.py`) | a fake radio lacking a feature doesn't fail a bring-up run |

`tools/bringup.py check` finds its Python through `tools/rfoundry_python.sh` like the other steps. Comparing the docs'
support claims with the records is not checked yet.

`--board` takes board ids, `--platform` platform ids and `--toolchain` toolchain ids (each a comma separated list).
`--board` excludes the other two; `--platform` and `--toolchain` together select the boards that match both. An
unknown id fails with exit status 2, so a typo in a workflow never passes with nothing built.

`tools/install-arm-toolchain.sh` installs the pinned Arm toolchain on Linux x86_64 the way CI does, trimmed to the
Cortex-M4 multilib (215 MB instead of 1 GB; `FULL=1` for all of it).

## Toolchains

Every firmware job (`ci.yml`, `pr-tests.yml`, `nightly.yml`, `release.yml`) sets up its compiler with the composite
action `.github/actions/toolchain`, given the job's toolchain id. A composite action runs as steps of the job, so it
adds no job. It reads the pin from the `[toolchain]` table of the `platform/*/platform.toml` with that `id`; no
workflow types a version. Platforms that share a toolchain id must pin the same toolchain (`tools/boards.py check`).

| Toolchain | On the self-hosted runner | Elsewhere |
|---|---|---|
| `arm` | `tools/install-arm-toolchain.sh` finds the image's toolchain and does nothing | the same script downloads the pinned, checksummed release into `~/opt`, every time (no cache) |
| `csky` | the image's csky-elf GCC on `PATH` (`/opt/csky-elf`) | the csky-elf image: log in (when `RFOUNDRY_CSKY_REGISTRY` is set), pull, log out, then the job runs its build through `tools/docker/csky-toolchain/run.sh` (the action's `run` output) |

No workflow builds csky-elf GCC from source. The image is built by `tools/docker/csky-toolchain/build.sh`
([The csky-elf image](ci-runners.md#the-csky-elf-image)). Neither is cached: `pr-tests.yml` runs a pull request's
code with `main`'s cache scope, so a cached compiler could be one the pull request planted
(Pull request checks, above). When the image isn't available (`run.sh` exit status 3), a CI or nightly job ends with a notice
and the release job fails. A new toolchain gets a branch in the action's first step and an entry in each firmware
matrix.

## Coverage

`tools/coverage.sh` rebuilds the host tests with `--coverage` (`make -C tests/host COVERAGE=1`), runs them and
reads the counters with [gcovr](https://gcovr.com): `build/coverage/index.html` (every file with its source
annotated), `coverage.xml` (Cobertura), `summary.txt` and `summary.md` (the totals and the files with the most
uncovered lines; CI adds it to the job summary). It works with GCC (`gcov`) and with clang, Apple's included
(`xcrun llvm-cov gcov`, picked automatically). Install gcovr with `pip install gcovr`, `pipx install gcovr` or the
`gcovr` package (Ubuntu 24.04's 7.0 is new enough). `COVERAGE_FAIL_UNDER=85 tools/coverage.sh` also fails below a
line percentage; CI doesn't set a threshold yet.

Counted are the `core/`, `drivers/`, `hal/`, `boards/` and `platform/` sources a host test compiles; the tests,
`third_party/` and generated headers are not. A file no host test builds isn't in the report at all, so the
percentage is of the code under host test, not of the whole firmware. The same file built into several tests with
different switches is merged (`--merge-mode-functions=separate`).

The report lists the files with the most uncovered lines. Not built into any host test: the debug link commands
(except the protocol and split parts), `hostlink.c`, `net_services.c`, the RTOS task wrappers, the USB device and NCM drivers,
the display drivers and the SFR glue. The HIL tests cover most of these on the radio.

Coverage on the radio itself (gcov counters read out over the debug link after a HIL run) is not set up. Hosted coverage
services (Codecov, Coveralls) are not used: the repository is private, and the HTML artifact serves the same purpose.

### Sanitizers and fuzzing

`make -C tests/host SANITIZE=1` builds every host test with AddressSanitizer and UndefinedBehaviorSanitizer (Apple
clang and GCC; UBSan stops at the first report). CI's unit tests job builds the tests this way and with coverage at
once (`SANITIZE=1 tools/coverage.sh`; leak checks off: job containers may not allow ptrace), `RFOUNDRY_CI_SANITIZE=1
tools/ci.sh` runs it in the host step; build from clean when
switching (`make -C tests/host clean`). It isn't optimized, like the plain build: with `__OPTIMIZE__` glibc's headers
switch on extern inline and fortified functions, which don't build with the host flags' `__attribute__` override.
The parsers of untrusted input (SNMP, TFTP, mDNS, the gateway's Ethernet
input, KISS / HTTP / rigctld, LRRP, AX.25 / APRS, DMR data) have seeded mutation fuzz loops (`tests/host/fuzz.h`)
that feed exact-size heap buffers, so a read past the end is an ASan report; `RFOUNDRY_FUZZ_ITERS=<n>` replaces the small
default counts and `RFOUNDRY_FUZZ_SEED=<n>` varies the seed, e.g. `RFOUNDRY_FUZZ_ITERS=1000000 make -C tests/host SANITIZE=1`
for a long run by hand.
