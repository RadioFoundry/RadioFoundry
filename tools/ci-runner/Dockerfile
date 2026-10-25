# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# The self-hosted GitHub Actions runner image (README.md): a fresh container per job with everything tools/ci.sh and
# the workflows need, so jobs install nothing. Built on the C-SKY image (tools/docker/csky-toolchain), which brings
# Ubuntu 24.04 and csky-elf GCC in /opt/csky-elf; that image itself is not changed.
#
#   docker build -t rfoundry/ci-runner:latest --build-arg BASE=rfoundry/csky-gcc14:latest \
#       --build-arg ARM_GCC_VERSION="$(tools/platform_toolchain.sh stm32f4 version)" \
#       --build-arg ARM_GCC_SHA256="$(tools/platform_toolchain.sh stm32f4 sha256)" \
#       -f tools/ci-runner/Dockerfile tools
#
# The build context is tools/ (install-arm-toolchain.sh and ci-runner/). Every download is pinned and checksummed.
# The Arm toolchain's pin is platform/stm32f4/platform.toml's, passed in as build arguments: this file has no copy
# (tools/boards.py check fails on one that differs), and the build stops without them.
ARG BASE=rfoundry/csky-gcc14:latest
FROM ${BASE}

ARG RUNNER_VERSION=2.338.0
ARG RUNNER_SHA256=af4b794c1bc41d73d40535e3fe092a39f9679cd8d965954c2aca25a05ca41d32
ARG NODE_VERSION=22.23.3
ARG NODE_SHA256=df450af89261115ef9f9e3830c3eeb2cc9213b63c720b1af623cb5dcbe2e02de
ARG GH_VERSION=2.102.0
ARG GH_SHA256=bb766f710eef8ede859c18578c72c327597cd4c8a85b06001b1f3843c6019386
ARG ARM_GCC_VERSION
ARG ARM_GCC_SHA256
# the runner's uid; match the owner of the host directory mounted at /work
ARG RUNNER_UID=1000

# What ci.sh, coverage.sh, the manual and the site steps use; the apt installs in the workflows are skipped here.
# libpython3.12t64: PyInstaller (release.yml's rfoundry) needs the shared libpython, which python3 alone lacks
RUN apt-get update \
	&& DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		gcc libc6-dev zlib1g-dev make curl xz-utils zstd unzip jq ca-certificates git \
		python3 libpython3.12t64 python3-numpy python3-pytest python3-serial python3-markdown-it python3-pil pipx \
		gcovr tshark libicu74 libkrb5-3 liblttng-ust1t64 libssl3t64 zlib1g \
	&& rm -rf /var/lib/apt/lists/*

# Node 20+ for the web flasher tests (Ubuntu's is 18) and gh for the status and comment jobs
RUN set -eu; cd /tmp; \
	curl -fsSL --retry 3 -o node.tar.xz "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz"; \
	echo "${NODE_SHA256}  node.tar.xz" | sha256sum -c -; \
	tar -xJf node.tar.xz -C /usr/local --strip-components=1 \
		--exclude='*/share' --exclude='*/include' --exclude='*/lib/node_modules' --exclude='*/bin/np[mx]' --exclude='*/bin/corepack'; \
	curl -fsSL --retry 3 -o gh.tar.gz "https://github.com/cli/cli/releases/download/v${GH_VERSION}/gh_${GH_VERSION}_linux_amd64.tar.gz"; \
	echo "${GH_SHA256}  gh.tar.gz" | sha256sum -c -; \
	tar -xzf gh.tar.gz -C /usr/local --strip-components=1 "gh_${GH_VERSION}_linux_amd64/bin/gh"; \
	rm -f node.tar.xz gh.tar.gz; node --version; gh --version | head -1

# The pinned Arm GNU Toolchain, trimmed, the same way CI installs it (in /opt; the entrypoint links it into ~/opt)
COPY install-arm-toolchain.sh /tmp/install-arm-toolchain.sh
RUN [ -n "${ARM_GCC_VERSION}" ] && [ -n "${ARM_GCC_SHA256}" ] || \
		{ echo "pass --build-arg ARM_GCC_VERSION and ARM_GCC_SHA256 from platform/stm32f4/platform.toml" >&2; exit 1; }; \
	HOME=/ RUNNER_TEMP=/tmp ARM_GCC_VERSION=${ARM_GCC_VERSION} ARM_GCC_SHA256=${ARM_GCC_SHA256} \
	sh /tmp/install-arm-toolchain.sh && rm /tmp/install-arm-toolchain.sh
ENV ARM_GCC_DIR=/opt/arm-gnu-toolchain-${ARM_GCC_VERSION}-x86_64-arm-none-eabi/bin/ \
	ARM_GCC_NAME=arm-gnu-toolchain-${ARM_GCC_VERSION}-x86_64-arm-none-eabi

# The Actions runner isn't in the image (it is 700 MB unpacked): the entrypoint downloads this pinned, checksummed
# release into the work volume, which keeps the tarball between jobs
ENV RUNNER_VERSION=${RUNNER_VERSION} RUNNER_SHA256=${RUNNER_SHA256}

# reuse for the license lint, with chardet for its encoding detection (the jobs can't reach PyPI through the
# runner's proxy). Not on PATH: tools/ci.sh lints whenever reuse is installed, and the tree doesn't pass yet; the
# workflows add /opt/reuse/bin where they want it
ARG REUSE_VERSION=6.1.2
RUN PIPX_HOME=/opt/pipx PIPX_BIN_DIR=/opt/reuse/bin pipx install "reuse==${REUSE_VERSION}" \
	&& PIPX_HOME=/opt/pipx pipx inject reuse chardet && /opt/reuse/bin/reuse --version

# Bare clones of third_party's submodules, keyed by URL: tools/checkout-submodules.sh borrows their objects, so a job
# fetches only commits newer than the image. Keep the list in step with .gitmodules; a submodule missing here still
# comes over the network. Rebuild the image after a submodule bump to bring the clones up to date.
ARG SUBMODULE_URLS="\
https://github.com/STMicroelectronics/stm32f4xx-hal-driver.git \
https://github.com/STMicroelectronics/cmsis-device-f4.git \
https://github.com/STMicroelectronics/cmsis-core.git \
https://github.com/FreeRTOS/FreeRTOS-Kernel.git \
https://github.com/hathach/tinyusb.git"
ENV RFOUNDRY_GIT_CACHE=/opt/rfoundry-git
# owned by the runner, so git's safe.directory check never refuses them
RUN set -eu; for url in $SUBMODULE_URLS; do \
		dir="$RFOUNDRY_GIT_CACHE/$(printf '%s\n' "$url" | sed -E 's#^[a-z]+://##; s#\.git$##').git"; \
		git clone -q --bare "$url" "$dir"; \
	done; \
	chown -R "${RUNNER_UID}" "$RFOUNDRY_GIT_CACHE"; du -sh "$RFOUNDRY_GIT_CACHE"

RUN useradd --create-home --uid "${RUNNER_UID}" --shell /bin/bash runner 2>/dev/null \
	|| { userdel -r "$(id -nu "${RUNNER_UID}")" && useradd --create-home --uid "${RUNNER_UID}" --shell /bin/bash runner; }
COPY ci-runner/entrypoint.sh /usr/local/bin/rfoundry-ci-entrypoint
# the job-started hook: only main's (or a v* tag's) workflows run here (README.md "Which jobs it takes"). Root-owned,
# so a job can't change it (each job gets a fresh container anyway); chmod in a RUN, since COPY --chmod needs BuildKit
COPY ci-runner/job-gate.sh /usr/local/lib/rfoundry/job-gate.sh
RUN chmod 755 /usr/local/lib/rfoundry/job-gate.sh && touch /usr/local/lib/rfoundry/job-gate.kill
# RFOUNDRY_CI_IMAGE tells the workflows that the tools are already installed (they skip their apt and download steps)
ENV RFOUNDRY_CI_IMAGE=1 RUNNER_ALLOW_RUNASROOT=0 DOTNET_CLI_TELEMETRY_OPTOUT=1 \
	ACTIONS_RUNNER_HOOK_JOB_STARTED=/usr/local/lib/rfoundry/job-gate.sh
USER runner
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/rfoundry-ci-entrypoint"]
CMD ["run"]
