# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Sourced by the scripts in this directory: the container engine, the host's architecture and the image tag suffix.
# ENGINE is RFOUNDRY_ENGINE if set, else Apple's container on macOS when it's installed, else docker. Images are
# built for the host's own architecture: amd64 tags are plain, arm64 tags end in -arm64.
ENGINE=${RFOUNDRY_ENGINE:-}
if [ -z "$ENGINE" ]; then
	if [ "$(uname -s)" = Darwin ] && command -v container > /dev/null 2>&1; then ENGINE=container; else ENGINE=docker; fi
fi
command -v "$ENGINE" > /dev/null 2>&1 || { echo "toolchains: $ENGINE not found" >&2; exit 3; }
case $(uname -m) in
arm64|aarch64) ARCH=arm64 TC_ARCH=aarch64 ;;
x86_64|amd64) ARCH=amd64 TC_ARCH=x86_64 ;;
*) echo "toolchains: no images for a $(uname -m) host" >&2; exit 3 ;;
esac
SUFFIX=$([ "$ARCH" = amd64 ] || echo "-$ARCH")
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)

free_gb() { df -Pk "$1" | awk 'NR == 2 { print int($4 / 1048576) }'; }
have_image() { "$ENGINE" image inspect "$1" > /dev/null 2>&1; }

# Options for a container that works on a bind-mounted host directory. docker: run as the caller so the files it
# writes are theirs. Apple's container: its mounts always map to the caller, but each container is a VM with 1 GB of
# memory and 4 CPUs unless told otherwise.
run_opts() {
	if [ "$ENGINE" = container ]; then
		echo "-c $NCPU -m ${RFOUNDRY_MEM:-6G}"
	else
		echo "--user $(id -u):$(id -g)"
	fi
}
