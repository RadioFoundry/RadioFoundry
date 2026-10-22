#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# Runs a command in the DM-32 CI image (build.sh) on this source tree, e.g.
#   tools/docker/csky-toolchain/run.sh tools/ci.sh --quick --steps firmware --board dm32
#   ODR_CSKY_IMAGE=ghcr.io/sfowlr/odr-csky-gcc14:20261011b tools/docker/csky-toolchain/run.sh make BOARD=dm32
#
# The image is $ODR_CSKY_IMAGE (default platform/hrc7000/platform.toml [toolchain] image, the tag build.sh's image
# gets on the Gitea runner).
# When it isn't on this Docker host it is pulled, but only when the name starts with a registry host
# (ghcr.io/..., docker.io/..., registry.example:5000/...): a bare name is never looked up on Docker Hub, where
# anyone could own it. Exit status 3 (and a message) when Docker or the image isn't there, so CI can skip the job;
# otherwise the command's.
#
# Works on a plain host (the tree is bind-mounted at its own path) and from inside a CI job container that reaches
# the host's Docker socket (Gitea's act runner: the workspace is a Docker volume, so the new container takes the job
# container's volumes with --volumes-from). JOBS and ODR_CI_PRUNE are passed through.
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
IMG=${ODR_CSKY_IMAGE:-$("$ROOT/tools/platform_toolchain.sh" hrc7000 image)}
[ $# -gt 0 ] || { echo "usage: $0 command [args...]" >&2; exit 2; }

if ! command -v docker > /dev/null 2>&1 || ! docker info > /dev/null 2>&1; then
	echo "csky run: no usable Docker here" >&2
	exit 3
fi
if ! docker image inspect "$IMG" > /dev/null 2>&1; then
	case "${IMG%%/*}" in
	*.*|*:*|localhost) docker pull -q "$IMG" > /dev/null 2>&1 || { echo "csky run: can't pull $IMG" >&2; exit 3; } ;;
	*) echo "csky run: image $IMG not on this Docker host (set ODR_CSKY_IMAGE to a registry name to pull one)" >&2
	   exit 3 ;;
	esac
fi

# Inside a container? Find our own container id (the mounts of /etc/hostname name it) to share its volumes.
SELF=""
if [ -f /.dockerenv ]; then
	SELF=$(sed -n 's|.*/containers/\([0-9a-f]\{64\}\)/.*|\1|p' /proc/self/mountinfo | head -1)
	[ -n "$SELF" ] || SELF=$(hostname)
fi
if [ -n "$SELF" ]; then
	MOUNT="--volumes-from $SELF"
	USER_OPT=""		# the job container's user (root under act) owns the workspace
else
	MOUNT="-v $ROOT:$ROOT"
	USER_OPT="--user $(id -u):$(id -g)"
fi
echo "csky run: $IMG ($(docker image inspect -f '{{.Id}}' "$IMG" | cut -c1-19)) in $ROOT"
# safe.directory: the tree may belong to another user than the container's (git only reads it for version strings)
# shellcheck disable=SC2086
exec docker run --rm $MOUNT $USER_OPT -w "$ROOT" -e HOME=/tmp \
	-e JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}" -e ODR_CI_PRUNE="${ODR_CI_PRUNE:-0}" \
	-e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
	"$IMG" "$@"
