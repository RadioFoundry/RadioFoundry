#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Wraps Arm's prebuilt Arm GNU Toolchain (arm-none-eabi) for this host's architecture (Linux x86_64 or aarch64) in an
# image (Dockerfile.arm-gcc); nothing is compiled. Runs on Linux with docker or on an Apple silicon Mac with Apple's
# container (common.sh). Needs about 3 GB free while it builds.
#   tools/docker/toolchains/build-arm-gcc.sh [VERSION]       (default 15.2.rel1)
# Image: rfoundry/arm-gcc<major>:<VERSION>-<yyyymmdd>, with -arm64 on an arm64 host (TAG overrides it). The archive is
# checked against the SHA-256 Arm publishes beside it.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/common.sh"
V=${1:-15.2.rel1}
TAG=${TAG:-rfoundry/arm-gcc${V%%.*}:$V-$(date +%Y%m%d)$SUFFIX}
URL=https://developer.arm.com/-/media/Files/downloads/gnu/$V/binrel/arm-gnu-toolchain-$V-$TC_ARCH-arm-none-eabi.tar.xz
SUM=$(curl -fsSL "$URL.sha256asc" | awk '{print $1}')
[ ${#SUM} -eq 64 ] || { echo "build-arm-gcc: no SHA-256 for $URL" >&2; exit 1; }

CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
cat > "$CTX/toolchain-info.txt" <<INFO
Arm GNU Toolchain $V (arm-none-eabi, Linux $TC_ARCH host), prebuilt by Arm and unmodified apart from leaving out gdb
and the documentation. GPL; the sources are on Arm's download page beside the archive.
Archive: $URL
sha256: $SUM
Libc: newlib, newlib-nano (--specs=nano.specs).
INFO
"$ENGINE" build -t "$TAG" --build-arg URL="$URL" --build-arg SHA256="$SUM" -f "$HERE/Dockerfile.arm-gcc" "$CTX"
"$ENGINE" run --rm "$TAG" arm-none-eabi-gcc --version | head -1
echo "build-arm-gcc: image $TAG"
