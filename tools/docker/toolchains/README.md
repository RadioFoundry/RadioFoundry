<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
# Toolchain images

Container images with a firmware toolchain plus `make`, `python3` and `git`, enough for `make BOARD=<radio>`. Each
image is built for the host's own architecture, so an Apple silicon Mac runs it natively.

| Image (ghcr.io/radiofoundry/) | Toolchain | Script |
|---|---|---|
| `rfoundry-arm-gcc15` | Arm GNU Toolchain 15.2.rel1, Arm's prebuilt release | `build-arm-gcc.sh` |
| `rfoundry-csky-gcc15` | csky-elf GCC 15.2.0, built by `tools/build-csky-toolchain.sh` | `build-csky-gcc.sh` |
| `rfoundry-arm-ci` | The Arm toolchain CI pins, as `tools/install-arm-toolchain.sh` installs it (x86_64 only, no shell): CI copies it out instead of downloading Arm's archive | `build-arm-ci.sh` |

Tags: `<version>-<yyyymmdd>` and `latest` for amd64; the same with `-arm64` for arm64 (`latest-arm64`). The images
CI uses are the ones `platform/<platform>/platform.toml` `[toolchain]` names.

## Engine

The scripts use Apple's `container` on macOS when it's installed, otherwise `docker`; `RFOUNDRY_ENGINE=docker` or
`=container` picks one. Apple's `container` needs macOS 26 and Apple silicon:

```
brew install container
container system start
```

## Building in a container on a Mac

```
tools/docker/toolchains/run.sh ghcr.io/radiofoundry/rfoundry-arm-gcc15:latest-arm64 make BOARD=<radio>
tools/docker/toolchains/run.sh ghcr.io/radiofoundry/rfoundry-csky-gcc15:latest-arm64 make BOARD=<radio>
```

`run.sh` mounts the tree at its own path and pulls the image on first use. The packages are private to the
organization for now: log in first (`gh auth token | container registry login ghcr.io -u <github-user>
--password-stdin`). Apple's `container` gives each container a VM; `run.sh` gives it every CPU and 6 GB of memory
(`RFOUNDRY_MEM`).

## Making and publishing an image

```
tools/docker/toolchains/build-arm-gcc.sh [VERSION]            # about 3 GB free while it builds
tools/docker/toolchains/build-csky-gcc.sh [GCC_VERSION] [WORK] # about 6 GB free; 15 minutes on 8 cores
tools/docker/toolchains/push.sh rfoundry/arm-gcc15:15.2.rel1-<date>-arm64 ghcr.io/radiofoundry/rfoundry-arm-gcc15
```

`push.sh` pushes the image's own tag and `latest` (or `latest-arm64`). Log in to the registry first and out
afterwards; the scripts never handle a token.

On a Mac, `container build` leaves a BuildKit VM running with its cache. Its disk only grows; `container builder
delete --force` frees it.
