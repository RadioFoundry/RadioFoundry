<!-- SPDX-License-Identifier: MIT -->

# Continuous integration

What runs automatically, when, how long it takes, and how to run each piece by hand. Every CI job calls the same
scripts a developer runs locally (`tools/ci.sh`, `tools/coverage.sh`), so the YAML holds only setup and triggers.
The workflows are in `.github/workflows/`; Gitea Actions runs them from there too (there is no `.gitea/workflows`),
so one set serves the self-hosted Gitea runner and GitHub.

| Workflow | Runs on | When | What |
|---|---|---|---|
| `ci.yml` | `ubuntu-24.04` | every push to `main` that changes source (below); by hand on any branch | the unit tests (once, with coverage and sanitizers), Python and license checks, every board's default build |
| `pr-tests.yml` | `ubuntu-24.04` | every pull request, from `main`'s copy of the workflow | `ci.yml`'s jobs and the manual check, for the files the pull request changes; `PR checks` gates the merge. A branch's only CI |
| `nightly.yml` | `ubuntu-24.04` | 07:17 UTC daily when the default branch has an untested commit; by hand | the feature matrix of every board, `reuse lint` |
| `site.yml` | `ubuntu-24.04` | 07:17 UTC daily when the site's inputs changed or a release came out; by hand | builds and deploys the web site ([website.md](website.md)); pull requests build it in the unit tests job instead |
| `hil.yml` | `odr-hil` (the bench Mac) | by hand; nightly only when `ODR_HIL_SCHEDULE=true` | `tests/hil` against the radio and the SDR bench |
| `ui-gallery.yml` | `ubuntu-24.04` | a pull request that changes the UI (`core/ui/`, the simulator, `uiSim`) | renders the screen gallery at the base and at the pull request, and makes the side-by-side artifact |
| `ui-gallery-comment.yml` | `ubuntu-24.04` | when `ui-gallery.yml` finishes on a pull request | posts the pull request's gallery comment with the changed screens attached |
| `manual.yml` | `ubuntu-24.04` | docs changes pushed to `main`, `v*` tags | the user manual PDF ([release.md](release.md)) |
| `release.yml` | `ubuntu-24.04`, and for odrflash the hosted macOS / Windows runners | `v*` tags; pull requests that change the release tooling (packaging only, no release gate) | release images and odrflash ([release.md](release.md)) |

On GitHub, `ci.yml` and `nightly.yml` run only when the repository variable `ODR_CI_GITHUB` is `true`: the
repository is private, so hosted runners use paid minutes (about 6 per push). On Gitea they always run.

The `ubuntu-24.04` in the table is the default. When the repository variable `ODR_CI_RUNNER` is set (to `odr-ci`),
every Linux job goes to the self-hosted runner with that label instead
([below](#the-github-self-hosted-runner)). Pull request code from a fork always stays on `ubuntu-24.04`.

Each change is checked once. A branch is tested through its pull request (`pr-tests.yml`), and `main` again after
the merge (`ci.yml`). The unit tests run once per check, with the coverage counters and the sanitizers in the same
build. The site's pull request build and the release gate on pull requests are gone: the unit tests job's Python step
builds the site and runs the flasher's tests, and the release gate runs for a tag.

## On main: `ci.yml`

Triggered by changes under `boards/`, `config/`, `core/`, `drivers/`, `hal/`, `platform/`, `tests/`, `tools/`,
`third_party/` (submodule bumps), the `Makefile`, `AUTHORS` (the About page), `.gitmodules`, `LICENSES/`,
`REUSE.toml` or the workflow itself, pushed to `main`; by hand (Actions, CI, Run workflow) on any branch. A docs-only
push runs only the manual workflow. A newer push cancels the run in progress (`concurrency`).

| Job | Command | Time on the Gitea runner |
|---|---|---|
| `unit tests` | `SANITIZE=1 tools/coverage.sh` (the host tests, 64 suites with the tshark pcap checks, built once with `--coverage` and ASan / UBSan, and gcovr's report), then the UI screen gallery (`uiSim --gallery`) | about 3 min on the self-hosted runner: the unit tests 2.8 min on 4 CPUs, gallery 2 s |
| `checks` | `tools/ci.sh --steps registry,python,licenses` (doc links among the Python checks) | about 1.5 min |
| `firmware (Arm)` | `tools/ci.sh --quick --steps firmware --board dm1701,md380`: every Arm radio in one job | about 1 min with the checkout; the toolchain 50 s on a cache miss |
| `firmware (C-SKY)` | `tools/ci.sh --quick --steps firmware --board dm32,dm1801pro,db25d`, in the csky-elf image (`tools/docker/csky-toolchain/run.sh`) | about 1 min with the checkout; skipped where the image isn't available |

There is one firmware job per toolchain, not one per radio: a job's checkout and start take longer than a radio's
build, and a runner has 4 cores, so the two firmware jobs side by side keep an 8-core host busy while leaving runners
free for other pull requests. The jobs run in parallel, so a push is checked in about three minutes plus the
runner's image start. On an 8-core
Mac the plain host tests take 43 s.

Each firmware job builds with zero warnings, writes the size report (image bytes and free space in the linker's
application region, also put in the job summary on GitHub) and fails when the application region overflows. The
images and logs are workflow artifacts (`firmware-arm`, `firmware-csky`), as are the unit test logs with the screen gallery
(`host-logs`) and the coverage report (`coverage-report`).

`reuse lint` is part of the `checks` job only when the repository variable `ODR_CI_REUSE` is `true`; it doesn't pass on
the current tree yet (run it to see the files), and the nightly reports it as a warning meanwhile.

## Submodules

The jobs that build firmware check out without submodules, then run `tools/checkout-submodules.sh`. The self-hosted
runner image keeps bare clones of the `third_party/` repositories in `/opt/odr-git` (`ODR_GIT_CACHE`), and the script
borrows their objects (`git submodule update --reference`), so a job fetches from GitHub only the commits newer than
the image. A submodule the image lacks, or a runner without the image, takes a plain network checkout. Each submodule
gets three tries, so one dropped connection to GitHub no longer fails the job.

The nested submodules of FreeRTOS-Kernel (its partner and community ports) aren't checked out: no build uses them,
since the CK803 port is in `platform/hrc7000/freertos/`. After a submodule bump, rebuild the runner image to bring the
clones up to date ([tools/ci-runner/README.md](../../tools/ci-runner/README.md)); until then the new commits come over
the network. A new submodule also goes in the Dockerfile's `SUBMODULE_URLS`.

## Pull request checks: `pr-tests.yml`

Every pull request runs `ci.yml`'s jobs (`unit tests`, `firmware (Arm)` for the DM-1701 and MD-380,
`firmware (C-SKY)` for the DM-32UV, DM-1801 Pro and DB25-D) and `checks` on its merge commit. The first job,
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
`tools/install-arm-toolchain.sh`, about 50 s). The `csky` jobs log in to the image registry with the job's own token
(`packages: read`), pulls the image and logs out before the build runs. The status job is the only one with a write
permission (`statuses: write`), and it checks out nothing. Keep it that way: never add secrets, another write
permission, `actions/cache` or the pull request's text (title, body, branch name) in a run step to `pr-tests.yml`.
Changes to `.github/` need the owner's review (`.github/CODEOWNERS`, with code owner review required in the `main`
ruleset), since `main`'s workflows are the ones that run.

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
   comment is found by its marker line (`<!-- odr-ui-gallery -->`) and carries the commit, the UTC time of the update
   and the base commit. A run for a commit the pull request has already moved past posts nothing.

Only the comment job has write permissions (`pull-requests: write`, and `contents: write` for `ui-previews` alone). It runs no code from the pull request or the
artifact, the same split `pr-tests.yml` keeps for its status job. An artifact with more than 40 pictures fails the check.

Nothing else is needed: no branch, no host. Attachments on a private repository are visible to the people who can
see the pull request; the first run on a UI pull request confirms that and that `gh` accepts the attachments with the
job's token. `workflow_run` runs only from the default branch's copy of the workflow, and Gitea Actions may not
support it, so check the first run on the Gitea runner before relying on the comment there.

## Nightly: `nightly.yml`

| Job | Command | Time on the Gitea runner |
|---|---|---|
| `matrix` (dm1701, md380) | `ODR_CI_PRUNE=1 tools/ci.sh --matrix --steps firmware --board <b>`: each switch off alone, fm-minimal, maximal | DM-1701 4.4 min (47 builds), MD-380 3.8 min (42) |
| `csky-matrix` (dm32, dm1801pro, db25d) | the same for each C-SKY board in the csky-elf image | DM-32UV 2.9 min (50 builds; measured on the CI VM) |
| `reuse` | `reuse lint`, a warning until it passes | under a minute |

`--matrix` leaves out the default builds the per-push run already made; `ODR_CI_PRUNE=1` removes each build tree
once its size is recorded (the logs and images stay), which keeps the runner's disk from filling with about 65 MB
per configuration.

**Only when something changed.** A scheduled run first looks up a cache entry named `nightly-ok-<commit>`; when it
exists the run stops there with a notice. The last job of a successful run saves that entry. This works the same
on Gitea and GitHub without an API token or comparing commit times against the schedule, catches up after nights
the runner was off, and keeps a failing commit failing every night until it is fixed. If the cache evicts an entry
the cost is one repeated run. A manual run (Actions, Nightly, Run workflow, or `gh workflow run nightly.yml`)
always builds.

## Running the pieces locally

```
tools/ci.sh                                   # everything (the release gate), about 10 minutes on an 8-core Mac
tools/ci.sh --quick                           # what CI runs, without coverage and sanitizers
SANITIZE=1 tools/coverage.sh                  # the unit tests job's test run
tools/ci.sh --steps registry,python,licenses  # the checks job
tools/ci.sh --quick --steps firmware --board md380
make BOARD=db25d stack-check                  # the stack check alone, after a build (stack-check.md)
ODR_CI_PRUNE=1 tools/ci.sh --matrix --steps firmware --board dm1701    # a nightly matrix job
tools/coverage.sh                             # build/coverage/index.html
tools/docker/csky-toolchain/run.sh tools/ci.sh --quick --steps firmware --board dm1801pro   # on a host with the image
```

`--steps` takes any of these, comma separated (default all):

| Step | What it checks |
|---|---|
| `registry` | Every `board.toml` and `platform.toml` is well formed and agrees with `board.mk`, the linker scripts and `board_t`; `tools/odrflash/odrflash/registry.json` is current; the loader's self-test (`tools/boards.py`) |
| `host` | `make -C tests/host`, with each board's own tests (`boards/<b>/tests/host.mk`) |
| `python` | Every Python file parses, the HIL test collection, odrflash's tests, every tool's `--self-test` (found by searching `tools/` and `stub/`, so a new tool needs no ci.sh change), doc links, the glossary, the web site build and the flasher's tests |
| `firmware` | Every board whose compiler is installed: the default build, each build option value (`board.toml` `[[build_options]]`, such as a band), each `[ci] extra` configuration, the feature matrix, and the flash stubs of each built platform (`platform.toml` `stubs`) |
| `licenses` | `tools/license_grep.sh` and `reuse lint` |
| `similarity` | `tools/similarity_scan.py`, only with `ODR_SIMILARITY_REFS` set |

Every step but `host` needs Python 3.11 or later for the registry's TOML. `tools/odr_python.sh` finds it:
`$ODR_PYTHON`, else the newest `python3.N` on `PATH`. A board whose compiler is missing is skipped with the command
that installs it (its platform's `toolchain.build`). `--quick` and `--matrix` choose the firmware configurations. `tools/install-arm-toolchain.sh` installs the pinned Arm toolchain
on Linux x86_64 the way CI does, trimmed to the Cortex-M4 multilib (215 MB instead of 1 GB; `FULL=1` for all of it).

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

At the time of writing (Apple clang, 2026-10-11): **90.9 % of lines** (28217 of 31047), **93.1 % of functions** (2313
of 2484) and 78.6 % of branches, over 122 files (it was 88.3 % / 92.0 % / 74.5 % before the parser and storage tests). The most uncovered lines:

| File | Lines | Covered | Notes |
|---|---|---|---|
| `core/radio/radio_sm.c` | 2714 | 93 % | the largest file; the misses are rare feature corners (roaming probes, hotspot keying, GPS in calls) |
| `core/ui/ui_screens.c`, `ui_app.c`, `ui_menus.c` | 3800 | 81 % | screens and menu actions no flow test opens |
| `core/aprs/aprs.c` | 1008 | 91 % | |
| `drivers/modem/hrc7000/hrc7000.c` | 165 | 3 % | the DM-32 modem's chip layer (its DMR layer is tested) |

Not built into any host test (25 files, about 7150 lines): the debug link commands (`core/debuglink/debuglink*.c`
except the protocol and split parts), `hostlink.c`, `net_services.c`, the RTOS task wrappers
(`radio_task.c`, `input_task.c`, `storage_rtos.c`, `ui.c`, `app.c`), the USB device and NCM drivers,
`st7735.c` and the SFR glue (`hrcSfr.c`). The HIL tests cover most of these on the radio.

Coverage on the radio itself (gcov counters read out over the debug link after a HIL run) is feasible but not set
up: the instrumented image would fit only the DM-32UV (588 KB free; the DM-1701 and MD-380 have 3 to 5 KB left), and
it needs a small handler that dumps the counters on request.

Hosted coverage services (Codecov, Coveralls) would need the report, with source excerpts, sent to a third party; the private repository doesn't use them, and the
HTML artifact serves the same purpose.

### Sanitizers and fuzzing

`make -C tests/host SANITIZE=1` builds every host test with AddressSanitizer and UndefinedBehaviorSanitizer (Apple
clang and GCC; UBSan stops at the first report). CI's unit tests job builds the tests this way and with coverage at
once (`SANITIZE=1 tools/coverage.sh`; leak checks off: job containers may not allow ptrace), `ODR_CI_SANITIZE=1
tools/ci.sh` runs it in the host step; build from clean when
switching (`make -C tests/host clean`). It isn't optimized, like the plain build: with `__OPTIMIZE__` glibc's headers
switch on extern inline and fortified functions, which don't build with the host flags' `__attribute__` override.
The parsers of untrusted input (SNMP, TFTP, mDNS, the gateway's Ethernet
input, KISS / HTTP / rigctld, LRRP, AX.25 / APRS, DMR data) have seeded mutation fuzz loops (`tests/host/fuzz.h`)
that feed exact-size heap buffers, so a read past the end is an ASan report; `ODR_FUZZ_ITERS=<n>` replaces the small
default counts and `ODR_FUZZ_SEED=<n>` varies the seed, e.g. `ODR_FUZZ_ITERS=1000000 make -C tests/host SANITIZE=1`
for a long run by hand.

## Runners

### The Gitea runner

A VM (Ubuntu 24.04, 8 vCPUs, 7 GB RAM) running `gitea-runner` 2.1 (`act_runner.service`, configuration in
`/etc/gitea_runner.conf`), capacity 4, with the labels `ubuntu-latest`, `ubuntu-24.04` and `ubuntu-22.04` mapped to
`docker.gitea.com/runner-images`. Each job runs in a fresh container of that image, as root; the workspace is a
Docker volume. The runner mounts the host's Docker socket into job containers (`container.docker_host` empty), which
the C-SKY jobs use: `run.sh` starts the csky-elf image with the job container's volumes (`--volumes-from`), so the
image never has to be the job's own container (it has no Node.js for the JavaScript actions) and the runner's
`force_pull: true` never tries to fetch the locally built image from Docker Hub.

Things to know:

- The root disk has only about 5.7 GB free and holds Docker's storage (the job containers, workspaces and the
  runner's cache). The trimmed toolchain (215 MB per firmware job) and `ODR_CI_PRUNE` in the matrix keep the jobs
  small; keep an eye on `df -h /` before adding jobs. `/scratch` (40 GB) is where `build.sh` builds the toolchain.
- The VM's virtual CPU lacks x86-64-v2, which current NumPy wheels from PyPI require (they stop with "NumPy was built
  with baseline optimizations (X86_V2)"). The workflows install NumPy from Ubuntu's packages instead, which works;
  a Proxmox CPU type of `x86-64-v2-AES` or `host` would remove the limit.
- Gitea runs `schedule`, `workflow_dispatch` (with inputs), path filters, `vars`, `concurrency` (older Gitea
  versions ignore it, so superseded runs then finish instead of being canceled), the cache
  (`actions/cache@v4` on the runner's built-in cache server) and `actions/upload-artifact@v4`. The job summary
  (`$GITHUB_STEP_SUMMARY`) is written but not shown by Gitea; the size and coverage tables are also in the logs.

### The GitHub self-hosted runner

GitHub's hosted runners cost the account's Actions minutes on a private repository. The repository variable
`ODR_CI_RUNNER` moves the Linux jobs to a self-hosted runner instead: each such job has
`runs-on: ${{ vars.ODR_CI_RUNNER || 'ubuntu-24.04' }}`, so with the variable set to `odr-ci` they go to a runner
with that label, and with it unset (or on Gitea, where it isn't defined) nothing changes. `hil.yml` keeps its own
runner. `release.yml` builds odrflash on GitHub's macOS and Windows runners only for a `v*` tag; a pull request or a
push to `main` builds the Linux odrflash alone, on the self-hosted runner.

The runner is the image in `tools/ci-runner/` (its README has the setup): Ubuntu 24.04 with everything the
workflows install (the Arm and csky-elf toolchains, host GCC, Python with NumPy, pytest, pyserial, Pillow and
markdown-it, gcovr, tshark, Node.js 22, gh, reuse). The workflows notice it by `ODR_CI_IMAGE=1` in the
environment (and `runner.environment == 'self-hosted'` in a step's `if`) and skip their `apt-get` steps, the
toolchain cache, `setup-python`, `setup-node`, `pip` and Docker: the C-SKY jobs build directly with the image's
csky-elf GCC. Every job gets a new container, as a non-root user, with no Docker socket, no host network or other
host directories, and CPU, memory and process limits. Its network has no route out: an egress proxy passes HTTPS
to the hosts in `tools/ci-runner/proxy/allow` only (GitHub, the npm registry and PyPI), so a job can't download from
anywhere else (nodejs.org, developer.arm.com). A tool a job needs goes into the image. Ubuntu's system Python takes
no `pip install`; a job that needs packages makes a venv (`release.yml`'s odrflash job does).

Pull request code from a fork never runs on it. `pr-tests.yml` runs on `pull_request_target`, which runs every
pull request without approval, so its jobs (and `ui-gallery.yml`'s and the site's pull request build) choose the
runner with `github.event.pull_request.head.repo.full_name == github.repository && vars.ODR_CI_RUNNER ||
'ubuntu-24.04'`: a branch of this repository gets the self-hosted runner, a fork GitHub's. The repository's setting
for fork pull requests ("Run workflows from fork pull requests", off) keeps the `pull_request` workflows from running
fork code at all.

When the runner is off, jobs routed to it wait in the queue (and fail after a day). Unset `ODR_CI_RUNNER` to go back
to the hosted runners.

The UI gallery has a runner of its own, so a pull request's screenshot preview never waits behind the build and test
jobs. One runner is registered with the label `odr-ui` instead of `odr-ci` (`LABELS=odr-ui` in its env file,
[tools/ci-runner/README.md](../../tools/ci-runner/README.md)), and the repository variable `ODR_UI_RUNNER` is set to
`odr-ui`. `ui-gallery.yml` and `ui-gallery-comment.yml` run on `ODR_UI_RUNNER`, falling back to `ODR_CI_RUNNER` when
it's unset. So does `pr-tests.yml`'s `PR checks status` job: it runs a few seconds of `gh` after every other job, when
the render is long done, so it never holds a build runner. No other job uses that label. With six runners that leaves five for everything else. Unset
`ODR_UI_RUNNER` to share the pool again.

### GitHub

The same workflows run on GitHub's hosted runners when the repository is pushed there and `ODR_CI_RUNNER` is unset. Everything except the C-SKY
jobs works as is; those skip with a notice until the csky-elf image is in a registry the runner can pull (below).
On a private repository the hosted runners use the account's Actions minutes: a push costs about 6 runner minutes
(seven jobs), a nightly with changes about 12.

## The csky-elf image

The C-SKY jobs use the image named by the repository variable `ODR_CSKY_IMAGE`, default `odr/csky-gcc14:latest`
(`platform/hrc7000/platform.toml` `[toolchain] image`, which `run.sh` and `push.sh` read):
the tag `tools/docker/csky-toolchain/build.sh` gives its image on the Gitea runner (`odr/csky-gcc14:20261011b`,
549 MB). `run.sh` uses an image already on the Docker host, and pulls one only when the name starts with a
registry host (`ghcr.io/...`, `docker.io/...`), never a bare name from Docker Hub. When the image can't be had the
job ends with a notice instead of failing.

### Publishing it to GitHub Packages (ghcr.io)

The `gh` login on the Mac lacks the `write:packages` scope; add it, log the VM's Docker in with that token through
stdin (it never appears in a command line, a file in the repository or a log), push, and log out again:

```
gh auth refresh -h github.com -s write:packages
gh auth token | ssh <runner-vm> 'docker login ghcr.io -u <github-user> --password-stdin'
ssh <runner-vm> 'cd <a checkout> && tools/docker/csky-toolchain/push.sh ghcr.io/<owner>/odr-csky-gcc14'
ssh <runner-vm> 'docker logout ghcr.io'
gh auth refresh -h github.com -r write:packages          # optional: drop the scope again
```

`push.sh REPO [TAG...]` tags the local image (`SRC`, default the platform's `image`) as `REPO:<its dated tag>`
and `REPO:latest`, pushes both and prints the digest. It never handles a credential: log in before and out after.
The same script pushes to Docker Hub (`push.sh docker.io/<user>/odr-csky-gcc14` after `docker login`).

A new package on ghcr.io is **private**. Then:

1. Set `ODR_CSKY_IMAGE` to the dated tag, e.g. `ghcr.io/<owner>/odr-csky-gcc14:20261011b` (a fixed tag keeps CI
   reproducible; `latest` follows each push), and `ODR_CSKY_REGISTRY=ghcr.io`, in both Gitea's and GitHub's
   repository variables.
2. GitHub's hosted runners: in the package's settings, under "Manage Actions access", add the OpenDigitalRadio
   repository with read access. The jobs log in with their own token (`packages: read`); no secret is needed.
3. The Gitea runner: either pull the image once on the VM (`docker login ghcr.io`, `docker pull`, `docker logout`;
   `run.sh` then finds it locally), or give the jobs a token: create a classic personal access token with only
   `read:packages`, and set it as the Gitea repository secret `ODR_CSKY_REGISTRY_TOKEN` and your GitHub user name as the variable
   `ODR_CSKY_REGISTRY_USER`. The jobs log in through stdin and log out at the end.

Making the package public instead removes steps 2 and 3.

## HIL runner

`hil.yml` runs the hardware-in-the-loop suite (`tests/hil/README.md`) on a self-hosted runner labeled `odr-hil`,
which has to be the machine with the radio on USB and SSH access to the SDR bench. Nothing is registered yet; to set
one up on the bench Mac:

- **Gitea** (the primary): download `gitea-runner` (act_runner) for macOS arm64, then
  `gitea-runner register --instance https://<gitea-host> --token <registration token from the
  repository's Settings, Actions, Runners> --name bench-mac --labels odr-hil:host` and run
  `gitea-runner daemon` (a LaunchAgent keeps it up). The `:host` label runs jobs directly on the Mac, not in Docker,
  so the job sees the USB serial ports; the JavaScript actions need `node` on the PATH (`brew install node`).
- **GitHub**: Settings, Actions, Runners, New self-hosted runner, macOS ARM64; run `./config.sh` with the label
  `odr-hil`, then `./svc.sh install` and `./svc.sh start`. A self-hosted runner on a private repository is fine;
  never attach one to a public repository (anyone's pull request could run on it).

The runner's environment (the LaunchAgent's, or a `.env` file next to `gitea-runner`; never the repository) provides:

| Variable | What |
|---|---|
| `ODR_SDR_BENCH_HOST` | the SDR bench's SSH host or alias (default `sdr-bench`); passwordless SSH from the runner's user |
| `ODR_PORT` | the radio's serial port when several are present |
| `ODR_HIL_AMBE` | the speech recording for `test_dmr_voice` (outside the repository) |
| `ODR_RADIO_LOCK` | the lock file everyone at the bench takes before using the radio (default `~/.odr-radio.lock`); the job holds it with `lockf` for the whole run. `tools/bringup.py run` takes the same file, with `{port}` in it replaced by the radio's port; without the variable it locks `~/.odr-radio-<port>.lock`, so a bench with two radios runs a bring-up on each |

Run it from Actions, HIL, Run workflow, with the inputs that map to the pytest flags: `rf` (`--allow-rf
--allow-service-stop`), `usb_switch`, `codeplug_write`, `reboot`, a `-k` selection and extra options. Without any
it runs the radio-only tests, which never transmit. A full run with RF takes 30 to 40 minutes. The JUnit XML and
the screenshots are the `hil-results` artifact. A scheduled nightly run happens only when the repository variable
`ODR_HIL_SCHEDULE` is `true`, with the flags in `ODR_HIL_SCHEDULE_FLAGS`.

Gotchas:

- **The radio's firmware isn't flashed by the workflow.** Putting the radio in its bootloader takes a person
  (`tools/odrflash`); the job tests whatever is on the radio and prints its version first. Flash the commit to test
  before starting the run.
- **USB personality tests can strand the radio.** `usb_switch` tests switch the radio to a CDC-NCM network adapter
  and back. When macOS misses the NCM carrier the radio can stay a network adapter, and only a power cycle brings the
  serial port back; later tests then skip with "no radio". Leave `usb_switch` off for unattended runs.
- **Transmitting stays on the approved test frequencies.** The RF tests use the defaults in `tests/hil/conftest.py`
  (DMR and FM at 461.6875 MHz at the lowest power, APRS AFSK at 446.050 MHz with the operator's call). Never pass
  other frequencies in the `extra` input, and run `rf` only with the dummy load or antenna of the bench set-up.
- **One radio, one run.** The workflow's concurrency group keeps HIL runs one at a time and never cancels a running
  one (it might be transmitting or restoring a codeplug); the lock keeps people and agents off the radio meanwhile.
- If the runner is offline a manual run waits in the queue (Gitea) or fails after a day (GitHub).
