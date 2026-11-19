<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Self-hosted GitHub Actions runner

This folder holds a containerized runner for this repository's Linux CI jobs, for a Docker host you own. Each job gets a
fresh container with no way out to the network except HTTPS to GitHub. No workflow routes a job to it, and none reads
`RFOUNDRY_CI_RUNNER`; to use it, add the routing to the workflows. For how the runner is isolated and which jobs its
gate takes, see [The self-hosted runner image](../../docs/dev/ci-runners.md#the-self-hosted-runner-image).

The folder has the following files:

| File | What |
|---|---|
| `Dockerfile` | the runner image: the C-SKY image (Ubuntu 24.04 + csky-elf GCC) plus the Arm toolchain, host GCC, Python packages, gcovr, tshark, Node.js, gh, reuse |
| `entrypoint.sh` | in the container: empties `/work`, gets the pinned runner release (checksummed, cached in `/work/.cache`), then runs one job or registers |
| `runner-loop.sh` | on the host: `setup` (networks and proxy), `register` (once), and the loop (one container per job) |
| `runner.env.example` | the loop's settings |
| `proxy/` | the egress proxy image: tinyproxy, HTTPS to the hosts in `proxy/allow` only (GitHub, npm, PyPI) |
| `job-gate.sh`, `job-gate-test.sh` | The job-started hook that refuses every job whose workflow isn't this repository's from `main` or a `v*` tag, and its test (`tools/ci.sh` runs it) |
| `rfoundry-ci-runner@.service` | The systemd unit that runs the loop, one instance per runner |

## Set up

On the Docker host, as a user in the `docker` group (no root needed), build the images and set up the networks:

```sh
mkdir -p ~/rfoundry-ci-runner/context
# the Arm toolchain's pin, from platform/stm32f4/platform.toml (the Dockerfile has no copy of it)
ARM_V=$(tools/platform_toolchain.sh stm32f4 version); ARM_S=$(tools/platform_toolchain.sh stm32f4 sha256)
cp -r tools/install-arm-toolchain.sh tools/ci-runner ~/rfoundry-ci-runner/context/
cp tools/ci-runner/runner-loop.sh ~/rfoundry-ci-runner/
cp tools/ci-runner/runner.env.example ~/rfoundry-ci-runner/runner-1.env   # then edit: REPO, directories, subnet, limits
cd ~/rfoundry-ci-runner/context
docker build -t rfoundry/ci-runner:latest --build-arg BASE=rfoundry/csky-gcc14:latest --build-arg RUNNER_UID="$(id -u)" \
	--build-arg ARM_GCC_VERSION="$ARM_V" --build-arg ARM_GCC_SHA256="$ARM_S" -f ci-runner/Dockerfile .
docker build -t rfoundry/ci-proxy:latest ci-runner/proxy
~/rfoundry-ci-runner/runner-loop.sh setup
```

Register once, from any machine where `gh` has admin rights on the repository. The registration token is valid for
an hour and goes over stdin, never onto a command line:

```sh
gh api -X POST repos/OWNER/RadioFoundry/actions/runners/registration-token --jq .token |
	ssh DOCKER_HOST 'RFOUNDRY_CI_ENV=$HOME/rfoundry-ci-runner/runner-1.env ~/rfoundry-ci-runner/runner-loop.sh register'
```

Run the loop as a systemd service, one instance per runner (`rfoundry-ci-runner@1` reads `runner-1.env`). It starts
after Docker, restarts a loop that died, and on `systemctl stop` stops the job's container with SIGINT so the runner
closes its GitHub session:

```sh
sed "s/CI_USER/$USER/g" tools/ci-runner/rfoundry-ci-runner@.service | sudo tee /etc/systemd/system/rfoundry-ci-runner@.service
sudo systemctl daemon-reload && sudo systemctl enable --now rfoundry-ci-runner@1
```

Then set the repository variable: `gh variable set RFOUNDRY_CI_RUNNER --body rfoundry-ci`.

### More runners

One runner takes one job at a time. For more in parallel, run another service instance with its own env file: the same
`LABELS` (so jobs go to whichever is free), the same network and proxy, and its own `NAME`, `WORK_DIR` and `STATE_DIR`.
Size `MEMORY` so the runners together fit the host's RAM. A job peaked at about 1.3 GB counting its page cache, which
the limit reclaims first, so six runners at 1200m fit an 8 GB host with no swap. `CPUS` is a cap, so runners can share
the cores.

Labels are set when the runner registers; to change a registered runner's, re-register it or set them in the
repository's Settings, Actions, Runners.

```sh
cd ~/rfoundry-ci-runner
sed -e 's/^NAME=.*/NAME=rfoundry-ci-2/' -e 's|/work$|/work2|' -e 's|/state$|/state2|' runner-1.env > runner-2.env
gh api -X POST repos/OWNER/RadioFoundry/actions/runners/registration-token --jq .token |
	ssh DOCKER_HOST 'RFOUNDRY_CI_ENV=$HOME/rfoundry-ci-runner/runner-2.env ~/rfoundry-ci-runner/runner-loop.sh register'
sudo systemctl enable --now rfoundry-ci-runner@2
```

The same again with 3, 4, ... for more runners.

## Operate

| Task | How |
|---|---|
| See what runs | `systemctl status 'rfoundry-ci-runner@*'`; `docker ps --filter name=rfoundry-ci`; `~/rfoundry-ci-runner/log/NAME.log`; `docker logs rfoundry-ci-proxy`; the repository's Settings, Actions, Runners |
| Pause | `touch ~/rfoundry-ci-runner/disabled` (a running job finishes; no new container starts) |
| Resume | `rm ~/rfoundry-ci-runner/disabled` |
| Stop now | `sudo systemctl stop 'rfoundry-ci-runner@*'` (the runner gets SIGINT and closes its session; a running job fails) |
| Update the runner or tools | change the pinned versions in the Dockerfile and rebuild; the next job uses the new image. Updates are off (`--disableupdate`), so keep `RUNNER_VERSION` current: GitHub stops sending jobs to runner versions that are too old. Rebuild after a `third_party` submodule bump too: the image's clones of the submodules then cover the new commits ([docs/dev/ci.md](../../docs/dev/ci.md#submodules)) |
| Allow another host | add it to `proxy/allow`, rebuild `rfoundry/ci-proxy`, `docker rm -f rfoundry-ci-proxy`, `runner-loop.sh setup` |
| Re-register | delete the runner in Settings, Actions, Runners (or keep it: `--replace` takes over the name), then `register` as above |
| Go back to hosted runners | `gh variable delete RFOUNDRY_CI_RUNNER` |
| Remove | `gh variable delete RFOUNDRY_CI_RUNNER`; `sudo systemctl disable --now 'rfoundry-ci-runner@*'` and remove the unit file; delete the runner in Settings, Actions, Runners; `docker rm -f rfoundry-ci-proxy`; `docker network rm rfoundry-ci-internal rfoundry-ci-egress`; `docker rmi rfoundry/ci-runner:latest rfoundry/ci-proxy:latest`; remove `~/rfoundry-ci-runner` (with the credentials in `STATE_DIR`) and the work directory |
