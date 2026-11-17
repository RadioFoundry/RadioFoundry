#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Puts the pinned Arm toolchain, exactly as tools/install-arm-toolchain.sh installs it, alone in an image
# (Dockerfile.arm-ci). CI copies the compiler out of that image instead of downloading Arm's archive, so an outage of
# Arm's download site doesn't stop the firmware jobs. The CI runners are Linux x86_64: build on one, with docker.
#   tools/docker/toolchains/build-arm-ci.sh [TREE]
# TREE is an installed copy (~/opt/arm-gnu-toolchain-<version>-x86_64-arm-none-eabi). Without it the script runs
# install-arm-toolchain.sh from Arm's archive first. Image: rfoundry/arm-ci:<version>-<yyyymmdd> (TAG overrides it).
# Publish it with push.sh, then pin it by digest in platform/stm32f4/platform.toml [toolchain] image.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
. "$HERE/common.sh"
[ "$ARCH" = amd64 ] || { echo "build-arm-ci: the CI runners are x86_64: build on an x86_64 host" >&2; exit 3; }
V=$("$ROOT/tools/platform_toolchain.sh" stm32f4 version)
NAME=arm-gnu-toolchain-$V-x86_64-arm-none-eabi
TAG=${TAG:-rfoundry/arm-ci:$V-$(date +%Y%m%d)}

CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
if [ $# -ge 1 ]; then
	[ "$(basename "$1")" = "$NAME" ] || { echo "build-arm-ci: $1 isn't $NAME (the pinned version)" >&2; exit 1; }
	cp -a "$1" "$CTX/$NAME"
else
	HOME=$CTX/home ARM_GCC_IMAGE='' "$ROOT/tools/install-arm-toolchain.sh"
	mv "$CTX/home/opt/$NAME" "$CTX/$NAME"
	rm -rf "${CTX:?}/home"
fi
GCC=$CTX/$NAME/bin/arm-none-eabi-gcc
"$GCC" --version | head -1 | grep -qi "Toolchain $V" || { echo "build-arm-ci: $GCC isn't version $V" >&2; exit 1; }
[ "$("$GCC" -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -print-multi-directory)" = "thumb/v7e-m+fp/hard" ]
cat > "$CTX/toolchain-info.txt" <<INFO
Arm GNU Toolchain $V (arm-none-eabi, Linux x86_64 host), prebuilt by Arm and unmodified apart from the files
tools/install-arm-toolchain.sh leaves out (other multilibs, C++ and Fortran, gdb, the documentation). GPL; the sources
are on Arm's download page beside the archive:
https://developer.arm.com/-/media/Files/downloads/gnu/$V/binrel/$NAME.tar.xz
INFO
"$ENGINE" build -t "$TAG" --build-arg NAME="$NAME" -f "$HERE/Dockerfile.arm-ci" "$CTX"
echo "build-arm-ci: image $TAG"
