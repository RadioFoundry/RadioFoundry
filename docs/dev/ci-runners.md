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

## GitHub hosted runners

On GitHub every workflow runs on GitHub's hosted runners (`ubuntu-24.04`, and for `release.yml`'s rfoundry builds
`ubuntu-22.04`, macOS and Windows), except `hil.yml` ([below](#hil-runner)). The repository is public, so they cost
nothing. Each job starts on a fresh VM: it installs what it needs (`apt-get`, the pinned Arm toolchain) and pulls the
public csky-elf image for the C-SKY builds. On a private copy they use the account's Actions minutes, so `ci.yml` and
`nightly.yml` run there only when `RFOUNDRY_CI_GITHUB` is `true`.

A fork's `pull_request` runs wait for a maintainer's approval (Settings, Actions, "Approval for running fork pull
request workflows": all external contributors). Approve a run only after reading its changes to `.github/` and to the
scripts the jobs run.

## The GitHub self-hosted runner

Retired: no workflow routes a job to it any more, and the repository variable `RFOUNDRY_CI_RUNNER` is unused. The
image and its job gate stay in `tools/ci-runner/` (its README has the setup).

### Which jobs it takes

None. A self-hosted runner on a public repository runs any `pull_request` job that names its label, from a fork's
own copy of a workflow, so bringing one back takes `tools/ci-runner/job-gate.sh` (it refuses every workflow that isn't
`main`'s or a `v*` tag's) and never a job that runs a pull request's code.

## The csky-elf image

The C-SKY jobs use the image `platform/hrc7000/platform.toml` `[toolchain] image` names,
`ghcr.io/radiofoundry/rfoundry-csky-gcc14:<date>`, a public package: anyone can pull it, with no login. The repository
variable `RFOUNDRY_CSKY_IMAGE` overrides it in `ci.yml`, `nightly.yml` and `release.yml` (`pr-tests.yml` reads no
variable). `tools/docker/csky-toolchain/build.sh` builds the image (local tag `rfoundry/csky-gcc14:<date>` and
`:latest`) and `push.sh` publishes it. `run.sh` uses an image already on the Docker host, and pulls one only when the
name starts with a registry host (`ghcr.io/...`, `docker.io/...`), never a bare name from Docker Hub. When the image
can't be had, a CI or nightly job ends with a notice instead of failing; the release job fails.

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

`push.sh REPO [TAG...]` tags the local image (`SRC`, default `rfoundry/csky-gcc14:latest`) as `REPO:<its dated tag>`
and `REPO:latest`, pushes both and prints the digest. It never handles a credential: log in before and out after.
The same script pushes to Docker Hub (`push.sh docker.io/<user>/rfoundry-csky-gcc14` after `docker login`).

A new package on ghcr.io is **private**: make it public (the package's settings, "Change visibility"), then set
`[toolchain] image` in `platform/hrc7000/platform.toml` to the new dated tag (a fixed tag keeps CI reproducible;
`latest` follows each push). A private copy of the repository can keep a private image instead: set
`RFOUNDRY_CSKY_IMAGE` to it and `RFOUNDRY_CSKY_REGISTRY=ghcr.io`, and give the package's "Manage Actions access" the
repository with read access; `ci.yml`, `nightly.yml` and `release.yml` then log in with the job's own token
(`packages: read`). On the Gitea runner, pull a private image once on the VM, or set the secret
`RFOUNDRY_CSKY_REGISTRY_TOKEN` (a classic token with only `read:packages`) and the variable `RFOUNDRY_CSKY_REGISTRY_USER`.

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
