#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Runs a command in a toolchain image on this source tree, with docker or Apple's container (common.sh), e.g. on an
# Apple silicon Mac:
#   tools/docker/toolchains/run.sh ghcr.io/radiofoundry/rfoundry-arm-gcc15:latest-arm64 make BOARD=<radio>
# The tree is mounted at its own path, so build/ is the same as a native build's. An image that isn't here is pulled
# only when its name starts with a registry host (ghcr.io/...), never a bare name from Docker Hub. Exit status 3 when
# the engine or the image isn't there; otherwise the command's.
set -eu
[ $# -ge 2 ] || { sed -n '5,10p' "$0"; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
. "$HERE/common.sh"
IMG=$1; shift
if ! have_image "$IMG"; then
	case "${IMG%%/*}" in
	*.*|*:*|localhost) "$ENGINE" image pull "$IMG" > /dev/null || { echo "run: can't pull $IMG" >&2; exit 3; } ;;
	*) echo "run: image $IMG not here" >&2; exit 3 ;;
	esac
fi
# safe.directory: the tree may belong to another user than the container's (git only reads it for version strings)
# shellcheck disable=SC2046
exec "$ENGINE" run --rm $(run_opts) -v "$ROOT:$ROOT" -w "$ROOT" -e HOME=/tmp -e JOBS="${JOBS:-$NCPU}" \
	-e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' "$IMG" "$@"
