#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Builds the csky-elf toolchain (tools/build-csky-toolchain.sh) for this host's architecture in a throwaway builder
# container (Dockerfile.csky-build), then the toolchain image (Dockerfile.csky-gcc) from the installed prefix. Runs on
# Linux with docker or on an Apple silicon Mac with Apple's container (common.sh); about 15 minutes on 8 cores.
#   tools/docker/toolchains/build-csky-gcc.sh [GCC_VERSION] [WORK]  (defaults: 15.2.0, $TMPDIR/rfoundry-csky-gcc)
# Image: rfoundry/csky-gcc<major>:<VERSION>-<yyyymmdd>, with -arm64 on an arm64 host (TAG overrides it).
# The build tree (peak about 4 GB) stays in WORK, outside the image store; the build stops when WORK's disk gets under
# MIN_FREE_GB (default 1) free. KEEP=1 leaves WORK and the builder image.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
. "$HERE/common.sh"
GV=${1:-15.2.0}
W=${2:-${TMPDIR:-/tmp}/rfoundry-csky-gcc}
TAG=${TAG:-rfoundry/csky-gcc${GV%%.*}:$GV-$(date +%Y%m%d)$SUFFIX}
MIN_FREE_GB=${MIN_FREE_GB:-1}
BUILDER=rfoundry/csky-builder

mkdir -p "$W/ctx"
f=$(free_gb "$W")
[ "$f" -ge 5 ] || { echo "build-csky-gcc: only $f GB free on $W (need about 5)" >&2; exit 1; }
# A copy, not a file mount: Apple's container shares directories only
cp "$ROOT/tools/build-csky-toolchain.sh" "$W/build.sh"

NAME=rfoundry-csky-build-$$
watch_space() {
	while sleep 20; do
		if [ "$(free_gb "$W")" -lt "$MIN_FREE_GB" ]; then
			echo "build-csky-gcc: under $MIN_FREE_GB GB free on $W, stopping the build" >&2
			"$ENGINE" kill "$NAME" > /dev/null 2>&1 || true
			return
		fi
	done
}
watch_space & WATCH=$!
trap 'kill $WATCH 2>/dev/null || true' EXIT

"$ENGINE" build -t "$BUILDER" -f "$HERE/Dockerfile.csky-build" "$HERE"
# shellcheck disable=SC2046
"$ENGINE" run --rm --name "$NAME" $(run_opts) -v "$W:/work" -e PREFIX=/work/ctx/prefix -e SRC=/work/src -e CLEAN=1 \
	-e GV="$GV" "$BUILDER" bash /work/build.sh
rm -rf "${W:?}/src" "$W/build.sh"

cat > "$W/ctx/toolchain-info.txt" <<INFO
csky-elf cross toolchain for the C-SKY CK803S (soft float, no multilib), Linux $TC_ARCH host, built by
tools/build-csky-toolchain.sh in a Debian trixie container: GCC $GV (C only), binutils and newlib as that script
pins them (newlib for -mcpu=ck803s only, nano malloc, small reent). GPL (GCC, binutils), BSD-style (newlib); the
sources are the GNU and sourceware release archives the script names, checked by SHA-256.
INFO
cp "$HERE/Dockerfile.csky-gcc" "$W/ctx/Dockerfile"
"$ENGINE" build -t "$TAG" -f "$W/ctx/Dockerfile" "$W/ctx"
"$ENGINE" run --rm "$TAG" csky-elf-gcc --version | head -1
echo "build-csky-gcc: image $TAG"
if [ "${KEEP:-0}" != 1 ]; then
	rm -rf "${W:?}"
	"$ENGINE" image rm "$BUILDER" > /dev/null 2>&1 || true	# rebuilt in a minute from Dockerfile.csky-build
fi
