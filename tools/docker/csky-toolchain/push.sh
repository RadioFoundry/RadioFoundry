#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Pushes the C-SKY CI image (build.sh) to a registry, run on the Docker host that has the image:
#   tools/docker/csky-toolchain/push.sh ghcr.io/sfowlr/rfoundry-csky-gcc14             (GitHub Packages)
#   tools/docker/csky-toolchain/push.sh docker.io/<user>/rfoundry-csky-gcc14            (Docker Hub)
#   SRC=rfoundry/csky-gcc14:20261011b tools/docker/csky-toolchain/push.sh REPO [TAG...]
#
# Tags SRC (default rfoundry/csky-gcc14:latest, build.sh's local tag) as REPO:<its dated tag> and REPO:latest (or the TAGs given), pushes
# them and prints the digest. Log in first and out afterwards (docs/dev/ci-runners.md "Publishing it to GitHub Packages"): the
# script never handles a password or token, so none ends up in its arguments, its output or a file it writes.
set -eu
[ $# -ge 1 ] || { sed -n '5,12p' "$0"; exit 2; }
REPO=$1; shift
SRC=${SRC:-rfoundry/csky-gcc14:latest}
docker image inspect "$SRC" > /dev/null 2>&1 || { echo "push: no image $SRC here" >&2; exit 1; }

if [ $# -eq 0 ]; then
	# the dated tag build.sh gave the same image (rfoundry/csky-gcc14:20261011b next to :latest), if there is one
	id=$(docker image inspect -f '{{.Id}}' "$SRC")
	dated=$(docker image ls --no-trunc --format '{{.Tag}} {{.ID}}' "${SRC%:*}" |
		awk -v id="$id" '$2 == id && $1 != "latest" && $1 != "<none>" {print $1; exit}')
	set -- ${dated:+"$dated"} latest
fi
for t in "$@"; do
	docker tag "$SRC" "$REPO:$t"
	docker push "$REPO:$t"
done
docker image inspect -f '{{join .RepoDigests "\n"}}' "$SRC" | grep "^$REPO@" || true
echo "push: pushed $SRC as$(for t in "$@"; do printf ' %s:%s' "$REPO" "$t"; done)"
