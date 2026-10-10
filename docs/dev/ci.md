<!-- SPDX-License-Identifier: MIT -->

# Continuous integration

What runs automatically, when, how long it takes, and how to run each piece by hand. Every CI job calls the same
scripts a developer runs locally (`tools/ci.sh`, `tools/coverage.sh`), so the YAML holds only setup and triggers.
The workflows are in `.github/workflows/`; Gitea Actions runs them from there too (there is no `.gitea/workflows`),
so one set serves the self-hosted Gitea runner and GitHub.

| Workflow | Runs on | When | What |
|---|---|---|---|
| `ci.yml` | `ubuntu-24.04` | every push to any branch and every pull request that changes source (below); by hand | host tests, Python and license checks, coverage, every board's default build |
| `nightly.yml` | `ubuntu-24.04` | 07:17 UTC daily when the default branch has an untested commit; by hand | the feature matrix of every board, `reuse lint` |
| `hil.yml` | `odr-hil` (the bench Mac) | by hand; nightly only when `ODR_HIL_SCHEDULE=true` | `tests/hil` against the radio and the SDR bench |
| `manual.yml` | `ubuntu-24.04` | docs changes, `v*` tags | the user manual PDF ([release.md](release.md)) |
| `release.yml` | `ubuntu-24.04` and the hosted macOS / Windows runners | `v*` tags, release tooling changes | release images and odrflash ([release.md](release.md)) |

On GitHub, `ci.yml` and `nightly.yml` run only when the repository variable `ODR_CI_GITHUB` is `true`: the
repository is private, so hosted runners use paid minutes (about 6 per push). On Gitea they always run.

## Per push: `ci.yml`

Triggered by changes under `boards/`, `config/`, `core/`, `drivers/`, `hal/`, `platform/`, `tests/`, `tools/`,
`third_party/` (submodule bumps), the `Makefile`, `AUTHORS` (the About page), `.gitmodules`, `LICENSES/`,
`REUSE.toml` or the workflow itself. A docs-only push runs only the manual workflow. A newer push to the same branch
cancels the run in progress (`concurrency`).

| Job | Command | Time on the Gitea runner |
|---|---|---|
| `host` | `tools/ci.sh --steps host,python,licenses`, then the UI screen gallery (`uiSim --gallery`) | about 1.5 min: apt 18 s, host tests 36 s (64 suites, tshark pcap checks), Python checks 15 s, gallery 2 s |
| `coverage` | `tools/coverage.sh` | about 1.2 min: apt 15 s, instrumented host tests and gcovr 52 s |
| `firmware` (dm1701, md380) | `tools/ci.sh --quick --steps firmware --board <b>` | DM-1701 7 s, MD-380 UHF and VHF 15 s; the toolchain 50 s on a cache miss |
| `dm32` | the same for `dm32`, in the csky-elf image (`tools/docker/csky-toolchain/run.sh`) | 4 s; skipped where the image isn't available |

The jobs run in parallel, so a push is checked in about two minutes plus the runner's image start. Times are from
the Gitea runner (8 vCPUs), measured with the jobs' own commands in the same container images; on an 8-core Mac
the host tests take 43 s and the coverage run 68 s.

Each firmware job builds with zero warnings, writes the size report (image bytes and free space in the linker's
application region, also put in the job summary on GitHub) and fails when the application region overflows. The
images and logs are workflow artifacts (`firmware-<board>`), as are the host logs with the screen gallery
(`host-logs`) and the coverage report (`coverage-report`).

`reuse lint` is part of the host job only when the repository variable `ODR_CI_REUSE` is `true`; it doesn't pass on
the current tree yet (run it to see the files), and the nightly reports it as a warning meanwhile.

## Nightly: `nightly.yml`

| Job | Command | Time on the Gitea runner |
|---|---|---|
| `matrix` (dm1701, md380) | `ODR_CI_PRUNE=1 tools/ci.sh --matrix --steps firmware --board <b>`: each switch off alone, fm-minimal, maximal | DM-1701 4.4 min (47 builds), MD-380 3.8 min (42) |
| `dm32-matrix` | the same for `dm32` in the csky-elf image | 2.9 min (50 builds; measured on the CI VM) |
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
tools/ci.sh --quick                           # what the per-push workflow runs, minus coverage
tools/ci.sh --steps host,python,licenses      # the host job
tools/ci.sh --quick --steps firmware --board md380
ODR_CI_PRUNE=1 tools/ci.sh --matrix --steps firmware --board dm1701    # a nightly matrix job
tools/coverage.sh                             # build/coverage/index.html
tools/docker/csky-toolchain/run.sh tools/ci.sh --quick --steps firmware --board dm32   # on a host with the image
```

`--steps` takes any of `host`, `python`, `firmware`, `licenses`, `similarity` (comma separated); `--quick` and
`--matrix` choose the firmware configurations. `tools/install-arm-toolchain.sh` installs the pinned Arm toolchain
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
clang and GCC; UBSan stops at the first report). CI's coverage job runs it after the report (leak checks off: job
containers may not allow ptrace), `ODR_CI_SANITIZE=1 tools/ci.sh` runs it in the host step; build from clean when
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
the DM-32 jobs use: `run.sh` starts the csky-elf image with the job container's volumes (`--volumes-from`), so the
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

### GitHub

The same workflows run on GitHub's hosted runners when the repository is pushed there. Everything except the DM-32
jobs works as is; those skip with a notice until the csky-elf image is in a registry the runner can pull (below).
On a private repository the hosted runners use the account's Actions minutes: a push costs about 6 runner minutes
(five jobs), a nightly with changes about 12.

## The csky-elf image

The DM-32 jobs use the image named by the repository variable `ODR_CSKY_IMAGE`, default `odr/csky-gcc14:latest`:
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

`push.sh REPO [TAG...]` tags the local image (`SRC`, default `odr/csky-gcc14:latest`) as `REPO:<its dated tag>`
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
| `ODR_RADIO_LOCK` | the lock file everyone at the bench takes before using the radio (default `~/.odr-radio.lock`); the job holds it with `lockf` for the whole run |

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
  (DMR and FM at 461.6875 MHz at the lowest power, APRS AFSK at 145.090 MHz with the operator's call). Never pass
  other frequencies in the `extra` input, and run `rf` only with the dummy load or antenna of the bench set-up.
- **One radio, one run.** The workflow's concurrency group keeps HIL runs one at a time and never cancels a running
  one (it might be transmitting or restoring a codeplug); the lock keeps people and agents off the radio meanwhile.
- If the runner is offline a manual run waits in the queue (Gitea) or fails after a day (GitHub).
