#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Builds the C-SKY CI image: the builder image (Dockerfile.build) runs tools/build-csky-toolchain.sh (CLEAN=1) with
# WORK on the host as its work directory, then the CI image (Dockerfile) is made from the installed tree.
# Docker's own storage only grows by the base image, the build tools and the final image (about 0.6 GB); the build
# tree (peak about 4 GB) stays in WORK. Linux x86_64 or arm64, or a Mac, with Docker; the image is for the host's
# architecture, so the one CI pulls (x86_64 runners) is built on an x86_64 host.
#   tools/docker/csky-toolchain/build.sh [WORK] [TAG]
# Defaults: /scratch/rfoundry-csky ($TMPDIR/rfoundry-csky without /scratch, as on a Mac), rfoundry/csky-gcc14:<date>.
# The image is also tagged :latest in TAG's repository.
# KEEP=1 leaves WORK (the downloaded sources and the installed tree) and the builder image in place.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
TMP=${TMPDIR:-/tmp}
WORK=${1:-$([ -d /scratch ] && echo /scratch || echo "${TMP%/}")/rfoundry-csky}
TAG=${2:-rfoundry/csky-gcc14:$(date +%Y%m%d)}
MIN_FREE_GB=${MIN_FREE_GB:-1}

mkdir -p "$WORK/src"
printf 'src\n' > "$WORK/.dockerignore"	# the image takes only csky-elf/
DOCKER_FS=$(dirname "$(docker info -f '{{.DockerRootDir}}')")	# its parent: the root dir itself is root-only
free_gb() { df -Pk "$1" | awk 'NR == 2 { print int($4 / 1048576) }'; }	# POSIX df: GNU's -BG isn't on macOS
for d in "$WORK" "$DOCKER_FS"; do
	[ -d "$d" ] || continue
	f=$(free_gb "$d")
	[ "$f" -ge 5 ] || { echo "build.sh: only ${f} GB free on $d (need about 5)" >&2; exit 1; }
done

# Watch both file systems while the toolchain builds; stop the build if either gets too full
NAME=rfoundry-csky-build-$$
watch_space() {
	while sleep 20; do
		for d in "$WORK" "$DOCKER_FS"; do
			[ -d "$d" ] || continue
			if [ "$(free_gb "$d")" -lt "$MIN_FREE_GB" ]; then
				echo "build.sh: under ${MIN_FREE_GB} GB free on $d, stopping the build" >&2
				docker kill "$NAME" >/dev/null 2>&1 || true
				return
			fi
		done
	done
}
watch_space & WATCH=$!
trap 'kill $WATCH 2>/dev/null || true' EXIT

docker build -t rfoundry/csky-builder -f "$HERE/Dockerfile.build" "$HERE"
docker run --rm --name "$NAME" --user "$(id -u):$(id -g)" -v "$WORK:/work" -e GV="$("$ROOT/tools/platform_toolchain.sh" hrc7000 version)" \
	-v "$ROOT/tools/build-csky-toolchain.sh:/build.sh:ro" rfoundry/csky-builder

docker build -t "$TAG" -f "$HERE/Dockerfile" "$WORK"
# :latest too, which push.sh (its default SRC), stub/build_db25d_stubs.sh and the CI runner image start from. The
# repository is TAG up to its last ':', unless that ':' is a registry port (registry:5000/name, no tag)
REPO=${TAG%:*}
case ${TAG##*:} in */*) REPO=$TAG ;; esac
docker tag "$TAG" "${REPO}:latest"
docker run --rm "$TAG" csky-elf-gcc --version | head -1
echo "build.sh: image $TAG and ${REPO}:latest"
if [ "${KEEP:-0}" != 1 ]; then
	rm -rf "${WORK:?}/csky-elf" "${WORK:?}/src" "${WORK:?}/.dockerignore"
	rmdir "$WORK" 2>/dev/null || true
	docker rmi rfoundry/csky-builder >/dev/null 2>&1 || true	# rebuilt in a minute from Dockerfile.build
fi
