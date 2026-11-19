<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# CI runners and images

This page covers where the [CI](ci.md) workflows run and the container images that hold the toolchains: the Gitea
runner, GitHub's hosted runners, the self-hosted runner image, the toolchain images, and the hardware-in-the-loop
runner. For what each workflow runs, see [Continuous integration](ci.md).

## The Gitea runner

The Gitea runner is an Ubuntu 24.04 VM running `gitea-runner` (`act_runner.service`, configured in
`/etc/gitea_runner.conf`). It maps the labels `ubuntu-latest`, `ubuntu-24.04`, and `ubuntu-22.04` to
`docker.gitea.com/runner-images`. Each job runs as root in a fresh container of that image, and the workspace is a
Docker volume.

The runner mounts the host's Docker socket into job containers (`container.docker_host` is empty), and the C-SKY jobs
use it: `run.sh` starts the csky-elf image with the job container's volumes (`--volumes-from`). The image therefore
never has to be the job's own container (it has no Node.js for the JavaScript actions), and the runner's
`force_pull: true` never tries to fetch the locally built image from Docker Hub.

Keep the following in mind:

- The root disk is small and holds Docker's storage: the job containers, workspaces, and the runner's cache. The
  trimmed toolchain and the pruning of build trees (the default in `tools/ci.sh`) keep the jobs small. Check `df -h /`
  before you add jobs. `build.sh` builds the toolchain in `/scratch`.
- The VM's virtual CPU lacks x86-64-v2, which NumPy wheels from PyPI require (they stop with "NumPy was built with
  baseline optimizations (X86_V2)"). The workflows install NumPy from Ubuntu's packages instead.
- Gitea runs `schedule`, `workflow_dispatch` (with inputs), path filters, `vars`, `concurrency`, the cache
  (`actions/cache@v4` on the runner's built-in cache server), and `actions/upload-artifact@v4`. Older Gitea versions
  ignore `concurrency`, so superseded runs finish instead of being canceled. Gitea writes the job summary
  (`$GITHUB_STEP_SUMMARY`) but doesn't show it; the size and coverage tables are also in the logs.

## GitHub hosted runners

On GitHub, every workflow runs on GitHub's hosted runners (`ubuntu-24.04`, and for `release.yml`'s rfoundry builds
`ubuntu-22.04`, macOS, and Windows), except `hil.yml` (see [HIL runner](#hil-runner)). The repository is public, so
they cost nothing. Each job starts on a fresh VM, installs what it needs (`apt-get`, the pinned Arm toolchain), and
pulls the public csky-elf image for the C-SKY builds. On a private copy they use the account's Actions minutes, so
`ci.yml` and `nightly.yml` run there only when the repository variable `RFOUNDRY_CI_GITHUB` is `true`.

A fork's `pull_request` runs wait for a maintainer's approval (Settings, Actions, "Approval for running fork pull
request workflows": all external contributors). Approve a run only after you read its changes to `.github/` and to the
scripts the jobs run.

## The self-hosted runner image

`tools/ci-runner/` holds a containerized runner for the Linux CI jobs, for a Docker host you own. No workflow routes a
job to it, and the repository variable `RFOUNDRY_CI_RUNNER` is unused. To set it up, see
[tools/ci-runner/README.md](../../tools/ci-runner/README.md).

### How the runner is isolated

- **A fresh container per job.** The runner is registered once. Its configuration and credentials (`.runner`,
  `.credentials*`) stay on the host in `STATE_DIR` (mode 700), mounted read-only. The loop starts
  `docker run --rm ... run`, which copies them into the empty `/work`, runs `run.sh --once` (one job, then exit), and
  removes the container. The next job gets a new container from the image. The setup doesn't use `--ephemeral`: an
  ephemeral runner needs a new registration for every job, which needs a long-lived admin token on the host.
- **Unprivileged.** The container runs as the user `runner`, with `--cap-drop ALL`, `no-new-privileges`, no Docker
  socket, no host network, and no host directories except the work directory and the read-only registration.
  `--cpus`, `--memory` (no swap beyond it), and `--pids-limit` come from the runner's env file.
- **Egress through one proxy.** The job container is only on a Docker network created with `--internal`: no route to
  the LAN, the host, or the internet, and no DNS for outside names. The proxy container (`rfoundry-ci-proxy`:
  tinyproxy, user `nobody`, read-only, 0.5 CPU, 64 MB) is on that network and on a normal bridge. It allows CONNECT
  to port 443 of the hosts in `proxy/allow` (GitHub, its Actions services and artifact storage, the npm registry, and
  PyPI), refuses everything else, and resolves the names. The runner reaches it through `https_proxy` and
  `http_proxy`, with `no_proxy` empty. Anything a job needs from elsewhere, such as nodejs.org, must be in the image.
- **What a job can still do.** A job runs as the same user as the runner, so it can read the runner's credentials, as
  on any runner. With them, someone could take this runner's place and receive later jobs of this repository until the
  runner is removed. For this reason, only this repository's own branches run code on it, never forks.

### Which jobs it takes

A self-hosted runner on a public repository runs any `pull_request` job that names its label, from a fork's own copy of
a workflow. `tools/ci-runner/job-gate.sh` is the runner's job-started hook (`ACTIONS_RUNNER_HOOK_JOB_STARTED`, set in
the image). The runner runs it before the first step of every job, and a job it refuses fails before anything from the
workflow runs.

The gate takes a job only when its workflow file is this repository's, from `main` or a `v*` tag: a push to `main`, a
schedule, a manual run on `main`, `pull_request_target`, `workflow_run`, or a release tag. A `pull_request` run uses
the pull request's own workflow file, which can name any label, so the gate always refuses it. `RFOUNDRY_CI_REPO` (the
env file's `REPO`) is the repository it accepts. `job-gate-test.sh` checks the cases, and `tools/ci.sh` runs it. Never
send the runner a job that runs a pull request's code.

A refused job shows `rfoundry job gate: refused ...` in its "Set up runner" step. The gate then stops the runner,
because a failed hook alone still lets the job's `if: always()` steps run. The container ends, the loop starts a new
one (which can wait up to ten minutes for GitHub to drop the stopped runner's session), and GitHub fails the job. Then
check that the workflow sends that job to GitHub's runners.

## The csky-elf image

The C-SKY jobs use the image that `platform/hrc7000/platform.toml` `[toolchain] image` names,
`ghcr.io/radiofoundry/rfoundry-csky-gcc14:<date>`. It's a public package: anyone can pull it, with no login. The
repository variable `RFOUNDRY_CSKY_IMAGE` overrides it in `ci.yml`, `nightly.yml`, and `release.yml`; `pr-tests.yml`
reads no variable.

- `tools/docker/csky-toolchain/build.sh` builds the image, tagged locally as `rfoundry/csky-gcc14:<date>` and
  `:latest`.
- `tools/docker/csky-toolchain/push.sh` publishes it.
- `tools/docker/csky-toolchain/run.sh` uses an image already on the Docker host. It pulls one only when the name starts
  with a registry host (`ghcr.io/...`, `docker.io/...`), never a bare name from Docker Hub.

When the image isn't available, a CI or nightly job ends with a notice instead of failing, and the release job fails.

### Publish the image to GitHub Packages

To publish, log the runner VM's Docker in to ghcr.io with a token that has the `write:packages` scope, push, and log
out. The token goes through stdin, so it never appears in a command line, a file in the repository, or a log.

```sh
gh auth refresh -h github.com -s write:packages
gh auth token | ssh <runner-vm> 'docker login ghcr.io -u <github-user> --password-stdin'
ssh <runner-vm> 'cd <a checkout> && tools/docker/csky-toolchain/push.sh ghcr.io/<owner>/rfoundry-csky-gcc14'
ssh <runner-vm> 'docker logout ghcr.io'
gh auth refresh -h github.com -r write:packages          # optional: drop the scope again
```

`push.sh REPO [TAG...]` tags the local image (`SRC`, default `rfoundry/csky-gcc14:latest`) as `REPO:<its dated tag>`
and `REPO:latest`, pushes both, and prints the digest. It never handles a credential: log in before and out after. The
same script pushes to Docker Hub (`push.sh docker.io/<user>/rfoundry-csky-gcc14` after `docker login`).

A new package on ghcr.io is private. Make it public (the package's settings, "Change visibility"), then set
`[toolchain] image` in `platform/hrc7000/platform.toml` to the new dated tag. A fixed tag keeps CI reproducible;
`latest` follows each push.

A private copy of the repository can keep a private image instead:

1. Set `RFOUNDRY_CSKY_IMAGE` to the image and `RFOUNDRY_CSKY_REGISTRY=ghcr.io`.
2. In the package's "Manage Actions access", give the repository read access. `ci.yml`, `nightly.yml`, and
   `release.yml` then log in with the job's own token (`packages: read`).
3. On the Gitea runner, pull the image once on the VM, or set the secret `RFOUNDRY_CSKY_REGISTRY_TOKEN` (a classic
   token with only `read:packages`) and the variable `RFOUNDRY_CSKY_REGISTRY_USER`.

## Toolchain images

The images in `tools/docker/toolchains/` hold a firmware toolchain plus `make`, `python3`, and `git`, enough for
`make BOARD=<b>`. Each image is built for the host's own architecture, so an Apple silicon Mac runs it natively. For
how to build, run, and publish them, see
[tools/docker/toolchains/README.md](../../tools/docker/toolchains/README.md).

| Image (`ghcr.io/radiofoundry/`) | Toolchain | Script |
|---|---|---|
| `rfoundry-arm-gcc15` | Arm GNU Toolchain 15.2.rel1, Arm's prebuilt release | `build-arm-gcc.sh` |
| `rfoundry-csky-gcc15` | csky-elf GCC 15.2.0, built by `tools/build-csky-toolchain.sh` | `build-csky-gcc.sh` |
| `rfoundry-arm-ci` | The Arm toolchain CI pins, as `tools/install-arm-toolchain.sh` installs it (x86_64 only, no shell). CI copies it out instead of downloading Arm's archive | `build-arm-ci.sh` |

Tags are `<version>-<yyyymmdd>` and `latest` for amd64, and the same with `-arm64` for arm64 (`latest-arm64`). The
images CI uses are the ones `platform/<p>/platform.toml` `[toolchain]` names.

## HIL runner

`hil.yml` runs the hardware-in-the-loop suite ([tests/hil/README.md](../../tests/hil/README.md)) on a self-hosted
runner labeled `rfoundry-hil`. That runner must be the machine with the radio on USB and network access to the bench
board. To set one up on a Mac, use one of these runners:

- **Gitea:** Download `gitea-runner` (act_runner) for macOS arm64, then run
  `gitea-runner register --instance https://<gitea-host> --token <registration token from the repository's Settings,
  Actions, Runners> --name <runner name> --labels rfoundry-hil:host` and `gitea-runner daemon`. A LaunchAgent keeps it
  running. The `:host` label runs jobs directly on the Mac, not in Docker, so the job sees the USB serial ports. The
  JavaScript actions need `node` on the `PATH` (`brew install node`).
- **GitHub, only on a private copy of the repository:** In Settings, Actions, Runners, choose New self-hosted runner,
  macOS ARM64. Run `./config.sh` with the label `rfoundry-hil`, then `./svc.sh install` and `./svc.sh start`. Never do
  this on the public repository: a pull request's own workflow could name the label and run on the machine with the
  radio and the bench (see [Which jobs it takes](#which-jobs-it-takes)).

The runner's environment (the LaunchAgent's, or a `.env` file next to `gitea-runner`; never the repository) provides
the following variables.

| Variable | What |
|---|---|
| `RFOUNDRY_BENCH_API` | The bench board, `HOST[:PORT]` |
| `RFOUNDRY_PORT` | The radio's serial port when several are present |
| `RFOUNDRY_HIL_AMBE` | The speech recording for `test_dmr_voice` (outside the repository) |
| `RFOUNDRY_RADIO_LOCK` | The lock file everyone at the bench takes before using the radio (default `~/.rfoundry-radio.lock`). The job holds it with `lockf` for the whole run. `tools/bringup.py run` takes the same file, with `{port}` in it replaced by the radio's port; without the variable it locks `~/.rfoundry-radio-<port>.lock`, so a bench with two radios runs a bring-up on each |

To run the suite, go to Actions, HIL, Run workflow, and set the inputs that map to the pytest flags: `rf`
(`--allow-rf --allow-service-stop`), `usb_switch`, `codeplug_write`, `reboot`, a `-k` selection, and extra options.
Without any of them, it runs the radio-only tests, which never transmit. A full run with RF takes 30 to 40 minutes. The
JUnit XML and the screenshots are the `hil-results` artifact. A scheduled nightly run happens only when the repository
variable `RFOUNDRY_HIL_SCHEDULE` is `true`, with the flags in `RFOUNDRY_HIL_SCHEDULE_FLAGS`.

Keep the following in mind:

- **The workflow doesn't flash the radio.** Putting the radio in its bootloader takes a person (`tools/rfoundry`). The
  job tests whatever is on the radio and prints its version first. Flash the commit to test before you start the run.
- **USB personality tests can strand the radio.** `usb_switch` tests switch the radio to a CDC-NCM network adapter and
  back. When macOS misses the NCM carrier, the radio can stay a network adapter, and only a power cycle brings the
  serial port back; later tests then skip with "no radio". Leave `usb_switch` off for unattended runs.
- **Transmitting stays on the approved test frequencies.** The RF tests use the defaults in `tests/hil/conftest.py`
  (DMR, FM, and APRS AFSK at the lowest power; APRS sends the operator's call). Never pass other frequencies in the
  `extra` input, and run `rf` only with the dummy load or antenna of the bench setup.
- **One radio, one run.** The workflow's concurrency group keeps HIL runs one at a time and never cancels a running one,
  which might be transmitting or restoring a codeplug. The lock keeps people and agents off the radio meanwhile.
- **An offline runner delays the run.** A manual run waits in the queue (Gitea) or fails after a day (GitHub).
