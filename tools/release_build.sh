#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Builds and packages every releasable board of one toolchain (docs/dev/release.md): each board whose board.toml has
# [status] release = "stable", its default image and each build option value's image (tools/boards.py builds), each
# packaged by tools/release.py into OUT. release.yml runs it once per toolchain, through the csky-elf image's run.sh
# for C-SKY, so it needs only sh, make, git and a Python 3.11 or later.
#
#   tools/release_build.sh --toolchain arm --version v1.2.0 --out dist
#
# Prints "no releasable board" and exits 0 when the toolchain has none.
set -eu
cd "$(dirname "$0")/.."
TOOLCHAIN="" VERSION="" OUT=""
while [ $# -gt 0 ]; do
	case $1 in
	--toolchain) TOOLCHAIN=$2; shift 2 ;;
	--version) VERSION=$2; shift 2 ;;
	--out) OUT=$2; shift 2 ;;
	*) echo "usage: $0 --toolchain T --version V --out DIR" >&2; exit 2 ;;
	esac
done
[ -n "$TOOLCHAIN" ] && [ -n "$VERSION" ] && [ -n "$OUT" ] || { echo "usage: $0 --toolchain T --version V --out DIR" >&2; exit 2; }
PY=$(tools/rfoundry_python.sh)	# the registry reads TOML
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

boards=$("$PY" tools/boards.py list --toolchain "$TOOLCHAIN" --release stable --comma)
[ -n "$boards" ] || { echo "release_build: no releasable board on toolchain $TOOLCHAIN"; exit 0; }
echo "release_build: $TOOLCHAIN: $boards"
builds=$(mktemp)
trap 'rm -f "$builds"' EXIT
for b in $(echo "$boards" | tr , ' '); do
	# the default image, then each build option value's ("name VAR=value...", tools/boards.py builds)
	{ echo default; "$PY" tools/boards.py builds "$b"; } > "$builds"
	while read -r name args <&3; do
		opts=""
		for a in $args; do opts="$opts --build-option $a"; done
		echo "== $b $name"
		# shellcheck disable=SC2086
		make BOARD="$b" $args -j"$JOBS"
		# shellcheck disable=SC2086
		"$PY" tools/release.py --board "$b" $opts --version "$VERSION" --out "$OUT"
	done 3< "$builds"
done
