# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# CI image for the DM-32 build: Ubuntu 24.04, the csky-elf GCC 14 toolchain from tools/build-csky-toolchain.sh in
# /opt/csky-elf, and what tools/ci.sh needs for `ONLY=dm32`. build.sh builds the toolchain in a throwaway container
# (on a scratch directory, not in image layers) and then this image from the result; the build context is only
# csky-elf/ and this file.
FROM ubuntu:24.04
RUN apt-get update \
	&& DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		make git python3 ca-certificates libgmp10 libmpfr6 libmpc3 libisl23 zlib1g libzstd1 \
	&& rm -rf /var/lib/apt/lists/*
COPY csky-elf /opt/csky-elf
ENV PATH=/opt/csky-elf/bin:$PATH CSKY_GCC_DIR=/opt/csky-elf/bin/
RUN csky-elf-gcc --version | head -1
