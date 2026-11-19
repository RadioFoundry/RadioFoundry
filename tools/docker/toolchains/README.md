<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Toolchain images

This folder builds container images that hold a firmware toolchain plus `make`, `python3`, and `git`, enough for
`make BOARD=<radio>`. Each image is built for the host's own architecture, so an Apple silicon Mac runs it natively.
For the images, their tags, and which ones CI uses, see
[Toolchain images](../../../docs/dev/ci-runners.md#toolchain-images).

| Script | Does |
|---|---|
| `run.sh IMAGE COMMAND...` | Runs a command in an image, with the tree mounted at its own path |
| `build-arm-gcc.sh [VERSION]` | Builds `rfoundry-arm-gcc15` from Arm's prebuilt release (about 3 GB free while it builds) |
| `build-csky-gcc.sh [GCC_VERSION] [WORK]` | Builds `rfoundry-csky-gcc15` with `tools/build-csky-toolchain.sh` (about 6 GB free; 15 minutes on 8 cores) |
| `build-arm-ci.sh` | Builds `rfoundry-arm-ci`, the Arm toolchain CI pins |
| `push.sh IMAGE REPO` | Pushes the image's own tag and `latest` (or `latest-arm64`) |

## Engine

The scripts use Apple's `container` on macOS when it's installed, and `docker` otherwise. To pick one, set
`RFOUNDRY_ENGINE=docker` or `RFOUNDRY_ENGINE=container`. Apple's `container` needs macOS 26 and Apple silicon:

```
brew install container
container system start
```

## Build firmware in a container

```
tools/docker/toolchains/run.sh ghcr.io/radiofoundry/rfoundry-arm-gcc15:latest-arm64 make BOARD=<radio>
tools/docker/toolchains/run.sh ghcr.io/radiofoundry/rfoundry-csky-gcc15:latest-arm64 make BOARD=<radio>
```

`run.sh` pulls the image on first use. The packages are private to the organization, so log in first:
`gh auth token | container registry login ghcr.io -u <github-user> --password-stdin`. Apple's `container` gives each
container a VM, and `run.sh` gives it every CPU and 6 GB of memory (`RFOUNDRY_MEM`).

## Make and publish an image

```
tools/docker/toolchains/build-arm-gcc.sh [VERSION]
tools/docker/toolchains/build-csky-gcc.sh [GCC_VERSION] [WORK]
tools/docker/toolchains/push.sh rfoundry/arm-gcc15:15.2.rel1-<date>-arm64 ghcr.io/radiofoundry/rfoundry-arm-gcc15
```

Log in to the registry before you push and log out afterwards; the scripts never handle a token.

On a Mac, `container build` leaves a BuildKit VM running with its cache. Its disk only grows; to free it, run
`container builder delete --force`.
