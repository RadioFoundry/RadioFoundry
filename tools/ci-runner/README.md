<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# Self-hosted GitHub Actions runner

A containerized runner for this repository's Linux CI jobs, for a Docker host you own: a fresh container for every
job, and no way out to the network except HTTPS to GitHub. The workflows use it when the repository variable
`ODR_CI_RUNNER` is set to its label (`odr-ci`); [docs/dev/ci.md](../../docs/dev/ci.md#the-github-self-hosted-runner)
explains the routing and why pull requests from forks never reach it.

| File | What |
|---|---|
| `Dockerfile` | the runner image: the DM-32 image (Ubuntu 24.04 + csky-elf GCC) plus the Arm toolchain, host GCC, Python packages, gcovr, tshark, Node.js, gh, reuse |
| `entrypoint.sh` | in the container: empties `/work`, gets the pinned runner release (checksummed, cached in `/work/.cache`), then runs one job or registers |
| `runner-loop.sh` | on the host: `setup` (networks and proxy), `register` (once), and the loop (one container per job) |
| `runner.env.example` | the loop's settings |
| `proxy/` | the egress proxy image: tinyproxy, HTTPS to the hosts in `proxy/allow` only (GitHub, npm, PyPI) |

## How it is isolated

- **A fresh container per job.** The runner is registered once; its configuration and credentials (`.runner`,
  `.credentials*`) stay on the host in `STATE_DIR` (mode 700), mounted read-only. The loop starts
  `docker run --rm ... run`, which copies them into the empty `/work`, runs `run.sh --once` (one job, then exit),
  and the container is removed. The next job gets a new container from the image.
  GitHub's runner warns that `--once` will be deprecated in favor of `--ephemeral`; an ephemeral runner needs a new
  registration for every job, which needs a long-lived admin token on the host, so this setup doesn't use it.
- **Unprivileged.** The user `runner`, `--cap-drop ALL`, `no-new-privileges`, no Docker socket, no host network, no
  host directories except the work directory and the read-only registration. `--cpus`, `--memory` (no swap beyond
  it) and `--pids-limit` from `runner.env`.
- **Egress through one proxy.** The job container is only on a Docker network created with `--internal`: no route to
  the LAN, the host or the internet, and no DNS for outside names. The proxy container (`odr-ci-proxy`: tinyproxy,
  user `nobody`, read-only, 0.5 CPU, 64 MB) is on that network and on a normal bridge. It allows CONNECT to port 443
  of the hosts in `proxy/allow` (GitHub, its Actions services and the artifact storage, the npm registry and PyPI) and
  refuses everything else,
  and it resolves the names. The runner reaches it through `https_proxy`/`http_proxy`, with `no_proxy` empty.
  Anything a job needs from elsewhere (nodejs.org, for one) must be in the image.
- **What a job could still do.** A job runs as the same user as the runner, so it can read the runner's
  credentials, as on any runner. With them someone could take this runner's place and receive later jobs of this
  repository until the runner is removed. That is why only this repository's own branches run here, never forks.

## Set up

On the Docker host, as a user in the `docker` group (no root needed):

```sh
mkdir -p ~/odr-ci-runner/context
cp -r tools/install-arm-toolchain.sh tools/ci-runner ~/odr-ci-runner/context/
cp tools/ci-runner/runner-loop.sh ~/odr-ci-runner/
cp tools/ci-runner/runner.env.example ~/odr-ci-runner/runner.env   # then edit: REPO, directories, subnet, limits
cd ~/odr-ci-runner/context
docker build -t odr/ci-runner:latest --build-arg BASE=odr/csky-gcc14:latest --build-arg RUNNER_UID="$(id -u)" \
	-f ci-runner/Dockerfile .
docker build -t odr/ci-proxy:latest ci-runner/proxy
~/odr-ci-runner/runner-loop.sh setup
```

Register once, from any machine where `gh` has admin rights on the repository. The registration token is valid for
an hour and goes over stdin, never onto a command line:

```sh
gh api -X POST repos/OWNER/OpenDigitalRadio/actions/runners/registration-token --jq .token |
	ssh DOCKER_HOST '~/odr-ci-runner/runner-loop.sh register'
```

Start the loop with cron (survives reboots and restarts a loop that died; `flock` keeps one):

```
@reboot      flock -n $HOME/odr-ci-runner/loop.lock $HOME/odr-ci-runner/runner-loop.sh
*/5 * * * *  flock -n $HOME/odr-ci-runner/loop.lock $HOME/odr-ci-runner/runner-loop.sh
```

Then set the repository variable: `gh variable set ODR_CI_RUNNER --body odr-ci`.

## Operate

| Task | How |
|---|---|
| See what runs | `docker ps --filter name=odr-ci`; `~/odr-ci-runner/log/runner.log`; `docker logs odr-ci-proxy`; the repository's Settings, Actions, Runners |
| Pause | `touch ~/odr-ci-runner/disabled` (a running job finishes; no new container starts) |
| Resume | `rm ~/odr-ci-runner/disabled` |
| Stop now | the pause above, then `docker stop odr-ci-1` (the runner gets SIGINT and closes its session; a running job fails) |
| Update the runner or tools | change the pinned versions in the Dockerfile and rebuild; the next job uses the new image. Updates are off (`--disableupdate`), so keep `RUNNER_VERSION` current: GitHub stops sending jobs to runner versions that are too old |
| Allow another host | add it to `proxy/allow`, rebuild `odr/ci-proxy`, `docker rm -f odr-ci-proxy`, `runner-loop.sh setup` |
| Re-register | delete the runner in Settings, Actions, Runners (or keep it: `--replace` takes over the name), then `register` as above |
| Go back to hosted runners | `gh variable delete ODR_CI_RUNNER` |
| Remove | `gh variable delete ODR_CI_RUNNER`; remove the cron lines; touch `disabled`; `docker stop odr-ci-1`; `pkill -f runner-loop.sh`; delete the runner in Settings, Actions, Runners; `docker rm -f odr-ci-proxy`; `docker network rm odr-ci-internal odr-ci-egress`; `docker rmi odr/ci-runner:latest odr/ci-proxy:latest`; remove `~/odr-ci-runner` (with the credentials in `STATE_DIR`) and the work directory |
