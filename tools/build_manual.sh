#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Builds the PDF user manual from the Markdown chapters in docs/user/ (docs/dev/release.md).
#
#   tools/build_manual.sh                    # build/manual/RadioFoundry-manual-<version>.pdf
#   tools/build_manual.sh --version v0.1.0   # the version printed on the title page and in the file name
#   tools/build_manual.sh --typ-only         # stop after the typst source (no typst needed)
#   tools/build_manual.sh --out FILE.pdf
#
# Needs pandoc (3.1.12 or later) and typst (0.12 or later) on PATH, or $PANDOC and $TYPST. Nothing else: no LaTeX,
# no fonts beyond the ones typst carries. Chapters are the files docs/user/NN-*.md in name order; docs/user/README.md
# is the GitHub index and is not part of the PDF. Set RFOUNDRY_REPO_URL (for example
# https://github.com/OWNER/REPO/blob/v0.1.0) to turn links into the rest of the repository into web links; without
# it they print as plain text.
set -eu

cd "$(dirname "$0")/.."
ROOT=$(pwd)
PANDOC=${PANDOC:-pandoc}
TYPST=${TYPST:-typst}
VERSION=""
OUT=""
TYP_ONLY=0

while [ $# -gt 0 ]; do
	case "$1" in
	--version) VERSION=$2; shift 2 ;;
	--out) OUT=$2; shift 2 ;;
	--typ-only) TYP_ONLY=1; shift ;;
	-h|--help) sed -n '5,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "build_manual.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done

if [ -z "$VERSION" ]; then
	VERSION=$(git describe --tags --always --dirty 2> /dev/null || echo unknown)
fi

need() {
	if ! command -v "$1" > /dev/null 2>&1; then
		echo "build_manual.sh: $1 not found (docs/dev/release.md says how to install it)" >&2
		exit 2
	fi
}
need "$PANDOC"
[ "$TYP_ONLY" = 1 ] || need "$TYPST"

BUILD="$ROOT/build/manual"
mkdir -p "$BUILD"
[ -n "$OUT" ] || OUT="$BUILD/RadioFoundry-manual-$VERSION.pdf"
TYP="$BUILD/manual.typ"

# Chapters in order, relative to docs/user (pandoc --file-scope keys the cross-file links by these names)
CHAPTERS=$(cd docs/user && ls [0-9][0-9]-*.md)
if [ -z "$CHAPTERS" ]; then
	echo "build_manual.sh: no chapters in docs/user" >&2
	exit 2
fi

DATE=$(date -u +%Y-%m-%d)
# shellcheck disable=SC2086 # the chapter list is split on purpose
(cd docs/user && RFOUNDRY_MANUAL_DIR=docs/user RFOUNDRY_REPO_ROOT="$ROOT" "$PANDOC" \
	--from=gfm --file-scope \
	--to=typst --standalone \
	--template="$ROOT/tools/manual/manual.typ" \
	--lua-filter="$ROOT/tools/manual/manual.lua" \
	--toc --toc-depth=2 \
	--metadata title="RadioFoundry User Manual" \
	--metadata subtitle="Version $VERSION" \
	--metadata date="$DATE" \
	--output="$TYP" \
	$CHAPTERS)
echo "manual: $TYP"

if [ "$TYP_ONLY" = 1 ]; then
	exit 0
fi
"$TYPST" compile --root "$ROOT" "$TYP" "$OUT"
echo "manual: $OUT"
