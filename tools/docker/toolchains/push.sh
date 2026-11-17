#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Pushes a toolchain image built here to a registry, with docker or Apple's container (common.sh):
#   tools/docker/toolchains/push.sh rfoundry/arm-gcc15:15.2.rel1-20261115-arm64 ghcr.io/radiofoundry/rfoundry-arm-gcc15
# The image goes up as REPO:<its own tag> and REPO:latest (latest-arm64 for an -arm64 tag), or as the TAGs given
# after REPO. Log in first and out afterwards (docs/dev/ci-runners.md); the script never handles a password or token.
set -eu
[ $# -ge 2 ] || { sed -n '5,9p' "$0"; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/common.sh"
SRC=$1 REPO=$2; shift 2
have_image "$SRC" || { echo "push: no image $SRC here" >&2; exit 1; }
if [ $# -eq 0 ]; then
	t=${SRC##*:}
	case $t in *-arm64) set -- "$t" latest-arm64 ;; *) set -- "$t" latest ;; esac
fi
for t in "$@"; do
	"$ENGINE" image tag "$SRC" "$REPO:$t"
	"$ENGINE" image push "$REPO:$t"
done
echo "push: pushed $SRC as$(for t in "$@"; do printf ' %s:%s' "$REPO" "$t"; done)"
