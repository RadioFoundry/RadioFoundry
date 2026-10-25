<!-- SPDX-License-Identifier: MIT -->

# CI Runners and Images

Where the [CI](ci.md) workflows run: the Gitea runner, the GitHub self-hosted runner, GitHub's hosted runners, the
csky-elf container image and the hardware-in-the-loop runner.

## The Gitea runner

A VM (Ubuntu 24.04) running `gitea-runner` (`act_runner.service`, configuration in
`/etc/gitea_runner.conf`), with the labels `ubuntu-latest`, `ubuntu-24.04` and `ubuntu-22.04` mapped to
`docker.gitea.com/runner-images`. Each job runs in a fresh container of that image, as root; the workspace is a
Docker volume. The runner mounts the host's Docker socket into job containers (`container.docker_host` empty), which
the C-SKY jobs use: `run.sh` starts the csky-elf image with the job container's volumes (`--volumes-from`), so the
image never has to be the job's own container (it has no Node.js for the JavaScript actions) and the runner's
`force_pull: true` never tries to fetch the locally built image from Docker Hub.

Things to know:

- The root disk is small and holds Docker's storage (the job containers, workspaces and the runner's cache). The trimmed
  toolchain and `RFOUNDRY_CI_PRUNE` in the matrix keep the jobs small; check `df -h /` before adding jobs. `/scratch` is
  where `build.sh` builds the toolchain.
- The VM's virtual CPU lacks x86-64-v2, which current NumPy wheels from PyPI require (they stop with "NumPy was built
  with baseline optimizations (X86_V2)"). The workflows install NumPy from Ubuntu's packages instead, which works;
  a Proxmox CPU type of `x86-64-v2-AES` or `host` would remove the limit.
- Gitea runs `schedule`, `workflow_dispatch` (with inputs), path filters, `vars`, `concurrency` (older Gitea
  versions ignore it, so superseded runs then finish instead of being canceled), the cache
  (`actions/cache@v4` on the runner's built-in cache server) and `actions/upload-artifact@v4`. The job summary
  (`$GITHUB_STEP_SUMMARY`) is written but not shown by Gitea; the size and coverage tables are also in the logs.

## The GitHub self-hosted runner

**The public GitHub repository has no self-hosted runner, and `RFOUNDRY_CI_RUNNER` and `RFOUNDRY_UI_RUNNER` are unset
there.** On a public repository, anyone can open a pull request whose own workflow file names any runner label, and a
`pull_request` run uses the pull request's copy of the workflows. The `runs-on` conditions below don't stop that: the
attacker writes the workflow. Approving fork runs one by one is the only other gate, and a missed `.github/` change in
one approval is enough. A job on the runner can read the runner's credentials and take its place for later jobs,
`main`'s included. The hosted runners cost nothing on a public repository. What follows applies to a private copy
of the repository (a mirror, or a fork kept private). A self-hosted runner on a public repository needs, at least, an
organization runner group restricted to `main`'s workflow files and single-use (just-in-time) registrations.

GitHub's hosted runners cost the account's Actions minutes on a private repository. The repository variable
`RFOUNDRY_CI_RUNNER` moves the Linux jobs to a self-hosted runner instead: each such job has
`runs-on: ${{ vars.RFOUNDRY_CI_RUNNER || 'ubuntu-24.04' }}`, so with the variable set to `rfoundry-ci` they go to a runner
with that label, and with it unset (or on Gitea, where it isn't defined) nothing changes. `hil.yml` keeps its own
runner. `release.yml` builds rfoundry on GitHub's macOS and Windows runners only for a `v*` tag; a pull request or a
push to `main` builds the Linux rfoundry alone, on the self-hosted runner.

The runner is the image in `tools/ci-runner/` (its README has the setup): Ubuntu 24.04 with everything the
workflows install (the Arm and csky-elf toolchains, host GCC, Python with NumPy, pytest, pyserial, Pillow and
markdown-it, gcovr, tshark, Node.js 22, gh, reuse). The workflows notice it by `RFOUNDRY_CI_IMAGE=1` in the
environment (and `runner.environment == 'self-hosted'` in a step's `if`) and skip their `apt-get` steps,
`setup-python`, `setup-node`, `pip` and Docker: the C-SKY jobs build directly with the image's
csky-elf GCC. Every job gets a new container, as a non-root user, with no Docker socket, no host network or other
host directories, and CPU, memory and process limits. Its network has no route out: an egress proxy passes HTTPS
to the hosts in `tools/ci-runner/proxy/allow` only (GitHub, the npm registry and PyPI), so a job can't download from
anywhere else (nodejs.org, developer.arm.com). A tool a job needs goes into the image. Ubuntu's system Python takes
no `pip install`; a job that needs packages makes a venv (`release.yml`'s rfoundry job does).

`pr-tests.yml` runs on `pull_request_target`, which runs every pull request without approval, so its jobs (and
`ui-gallery.yml`'s and `release.yml`'s) choose the runner with `github.event.pull_request.head.repo.full_name ==
github.repository && vars.RFOUNDRY_CI_RUNNER || 'ubuntu-24.04'`: a branch of this repository gets the self-hosted
runner, a fork GitHub's. Only `pr-tests.yml`'s condition holds against a fork, because `pull_request_target` runs
`main`'s copy of the workflow. In `ui-gallery.yml` and `release.yml`, `pull_request` runs, it only routes. On a private
repository, the setting "Run workflows from fork pull requests" (off) keeps those from running a fork's code. A
public repository doesn't have that setting, which is why it has no self-hosted runner (above).

When the runner is off, jobs routed to it wait in the queue (and fail after a day). Unset `RFOUNDRY_CI_RUNNER` to go back
to the hosted runners.

The UI gallery has a runner of its own, so a pull request's screenshot preview never waits behind the build and test
jobs. One runner is registered with the label `rfoundry-ui` instead of `rfoundry-ci` (`LABELS=rfoundry-ui` in its env file,
[tools/ci-runner/README.md](../../tools/ci-runner/README.md)), and the repository variable `RFOUNDRY_UI_RUNNER` is set to
`rfoundry-ui`. `ui-gallery.yml` and `ui-gallery-comment.yml` run on `RFOUNDRY_UI_RUNNER`, falling back to `RFOUNDRY_CI_RUNNER` when
it's unset. So does `pr-tests.yml`'s `PR checks status` job: it runs a few seconds of `gh` after every other job, when
the render is long done, so it never holds a build runner. No other job uses that label. With six runners that leaves five for everything else. Unset
`RFOUNDRY_UI_RUNNER` to share the pool again.

## GitHub hosted runners

The same workflows run on GitHub's hosted runners when the repository is pushed there and `RFOUNDRY_CI_RUNNER` is unset. Everything except the C-SKY
jobs works as is; those skip with a notice (the release's fails) until the csky-elf image is in a registry the runner
can pull (below).
On a private repository the hosted runners use the account's Actions minutes.

## The csky-elf image

The C-SKY jobs use the image named by the repository variable `RFOUNDRY_CSKY_IMAGE`, default `rfoundry/csky-gcc14:latest`
(`platform/hrc7000/platform.toml` `[toolchain] image`, which `run.sh` and `push.sh` read):
the tag `tools/docker/csky-toolchain/build.sh` gives its image on the Gitea runner (`rfoundry/csky-gcc14:<date>`). `run.sh` uses an image already on the Docker host, and pulls one only when the name starts with a
registry host (`ghcr.io/...`, `docker.io/...`), never a bare name from Docker Hub. When the image can't be had, a CI
or nightly job ends with a notice instead of failing; the release job fails.

### Publishing it to GitHub Packages (ghcr.io)

The `gh` login on the Mac lacks the `write:packages` scope; add it, log the VM's Docker in with that token through
stdin (it never appears in a command line, a file in the repository or a log), push, and log out again:

```
gh auth refresh -h github.com -s write:packages
gh auth token | ssh <runner-vm> 'docker login ghcr.io -u <github-user> --password-stdin'
ssh <runner-vm> 'cd <a checkout> && tools/docker/csky-toolchain/push.sh ghcr.io/<owner>/rfoundry-csky-gcc14'
ssh <runner-vm> 'docker logout ghcr.io'
gh auth refresh -h github.com -r write:packages          # optional: drop the scope again
```

`push.sh REPO [TAG...]` tags the local image (`SRC`, default the platform's `image`) as `REPO:<its dated tag>`
and `REPO:latest`, pushes both and prints the digest. It never handles a credential: log in before and out after.
The same script pushes to Docker Hub (`push.sh docker.io/<user>/rfoundry-csky-gcc14` after `docker login`).

A new package on ghcr.io is **private**. Then:

1. Set `RFOUNDRY_CSKY_IMAGE` to the dated tag, e.g. `ghcr.io/<owner>/rfoundry-csky-gcc14:20261011b` (a fixed tag keeps CI
   reproducible; `latest` follows each push), and `RFOUNDRY_CSKY_REGISTRY=ghcr.io`, in both Gitea's and GitHub's
   repository variables.
2. GitHub's hosted runners: in the package's settings, under "Manage Actions access", add the RadioFoundry
   repository with read access. The jobs log in with their own token (`packages: read`); no secret is needed.
3. The Gitea runner: either pull the image once on the VM (`docker login ghcr.io`, `docker pull`, `docker logout`;
   `run.sh` then finds it locally), or give the jobs a token: create a classic personal access token with only
   `read:packages`, and set it as the Gitea repository secret `RFOUNDRY_CSKY_REGISTRY_TOKEN` and your GitHub user name as the variable
   `RFOUNDRY_CSKY_REGISTRY_USER`. The jobs log in through stdin and log out at the end.

Making the package public instead removes steps 2 and 3.

## HIL runner

`hil.yml` runs the hardware-in-the-loop suite (`tests/hil/README.md`) on a self-hosted runner labeled `rfoundry-hil`,
which has to be the machine with the radio on USB and SSH access to the SDR bench. To set
one up on the bench Mac:

- **Gitea** (the primary): download `gitea-runner` (act_runner) for macOS arm64, then
  `gitea-runner register --instance https://<gitea-host> --token <registration token from the
  repository's Settings, Actions, Runners> --name bench-mac --labels rfoundry-hil:host` and run
  `gitea-runner daemon` (a LaunchAgent keeps it up). The `:host` label runs jobs directly on the Mac, not in Docker,
  so the job sees the USB serial ports; the JavaScript actions need `node` on the PATH (`brew install node`).
- **GitHub**: only on a private copy of the repository: Settings, Actions, Runners, New self-hosted runner, macOS
  ARM64; run `./config.sh` with the label `rfoundry-hil`, then `./svc.sh install` and `./svc.sh start`. Never on the
  public repository: a pull request's own workflow could name the label and run on the machine with the radio and the
  bench ([above](#the-github-self-hosted-runner)).

The runner's environment (the LaunchAgent's, or a `.env` file next to `gitea-runner`; never the repository) provides:

| Variable | What |
|---|---|
| `RFOUNDRY_SDR_BENCH_HOST` | the SDR bench's SSH host or alias (default `sdr-bench`); passwordless SSH from the runner's user |
| `RFOUNDRY_PORT` | the radio's serial port when several are present |
| `RFOUNDRY_HIL_AMBE` | the speech recording for `test_dmr_voice` (outside the repository) |
| `RFOUNDRY_RADIO_LOCK` | the lock file everyone at the bench takes before using the radio (default `~/.rfoundry-radio.lock`); the job holds it with `lockf` for the whole run. `tools/bringup.py run` takes the same file, with `{port}` in it replaced by the radio's port; without the variable it locks `~/.rfoundry-radio-<port>.lock`, so a bench with two radios runs a bring-up on each |

Run it from Actions, HIL, Run workflow, with the inputs that map to the pytest flags: `rf` (`--allow-rf
--allow-service-stop`), `usb_switch`, `codeplug_write`, `reboot`, a `-k` selection and extra options. Without any
it runs the radio-only tests, which never transmit. A full run with RF takes 30 to 40 minutes. The JUnit XML and
the screenshots are the `hil-results` artifact. A scheduled nightly run happens only when the repository variable
`RFOUNDRY_HIL_SCHEDULE` is `true`, with the flags in `RFOUNDRY_HIL_SCHEDULE_FLAGS`.

Gotchas:

- **The radio's firmware isn't flashed by the workflow.** Putting the radio in its bootloader takes a person
  (`tools/rfoundry`); the job tests whatever is on the radio and prints its version first. Flash the commit to test
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
