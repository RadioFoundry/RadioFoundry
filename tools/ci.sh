#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# The release gate: everything CI runs, in one script (docs/testing/README.md).
#
#   tools/ci.sh                 host tests, Python checks, every board with its feature matrix, license checks
#   tools/ci.sh --quick         the same with only each board's default build (no matrix)
#   tools/ci.sh --board dm1701  one board only
#   JOBS=8 tools/ci.sh          parallel make jobs (default: the CPU count)
#
# Steps, each reported as ok / FAIL / skip:
#   1. host tests (make -C tests/host)
#   2. Python: every tool and HIL test parses; pytest collects tests/hil (when pytest is installed); the flashing
#      tools' tests (tools/odrflash/tests, codec_extract and stm32_dfu self-tests); every relative link, image and
#      anchor in the Markdown resolves (tools/check_doc_links.py)
#   3. firmware, for every boards/<b>/board.mk whose compiler is installed (a board without one is skipped with a
#      notice: the DM-32's csky toolchain comes from tools/build-csky-toolchain.sh):
#        default            the board's defaults
#        no-<FEATURE>       each switch that is on by default, turned off alone (sub-features follow)
#        fm-minimal         FM only: no DMR, USB, VFO, diagnostics, sub-audio tones
#        maximal            every switch on
#        vhf                (MD-380 only) the VHF band image, default config
#      Each must build with zero compiler and linker warnings. Sizes go to build/ci/size-report.md.
#   4. tools/license_grep.sh, and `reuse lint` (skipped with a notice when reuse isn't installed: pipx install reuse)
#   5. tools/similarity_scan.py against the trees in $ODR_SIMILARITY_REFS (space separated), only when it is set:
#      the reference trees are never part of this repository or of public CI
#
# Build trees: build/ci/<board>-<config>/ (logs next to them). The exit status is 0 only when nothing failed.
set -u
cd "$(dirname "$0")/.." || exit 2

QUICK=0
ONLY=""
while [ $# -gt 0 ]; do
	case "$1" in
	--quick) QUICK=1 ;;
	--board) ONLY="$2"; shift ;;
	-h|--help) sed -n '5,28p' "$0"; exit 0 ;;
	*) echo "ci: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}
OUT=build/ci
mkdir -p "$OUT"
REPORT="$OUT/size-report.md"
FAILED=""
SKIPPED=""

ok()   { echo "ci: ok    $*"; }
bad()  { echo "ci: FAIL  $*"; FAILED="$FAILED
  $*"; }
skip() { echo "ci: skip  $*"; SKIPPED="$SKIPPED
  $*"; }

# ── 1. host tests ────────────────────────────────────────────────────────────
echo "== host tests"
if make -C tests/host > "$OUT/host-tests.log" 2>&1; then
	ok "host tests ($(grep -cE 'PASS|passed' "$OUT/host-tests.log") suites reported passing)"
else
	tail -30 "$OUT/host-tests.log"
	bad "host tests (see $OUT/host-tests.log)"
fi
if grep -q 'warning:' "$OUT/host-tests.log"; then
	grep 'warning:' "$OUT/host-tests.log" | sort -u | head -20
	bad "host tests built with warnings"
fi

# ── 2. Python ────────────────────────────────────────────────────────────────
echo "== python"
PY_FILES=$(git ls-files 'tools/*.py' 'tools/**/*.py' 'tests/hil/*.py' 2>/dev/null)
if command -v python3 > /dev/null; then
	if python3 -c 'import ast, sys
for f in sys.argv[1:]:
    ast.parse(open(f, encoding="utf-8").read(), f)' $PY_FILES; then
		ok "python: $(echo "$PY_FILES" | wc -w | tr -d ' ') files parse"
	else
		bad "python: syntax errors"
	fi
	if python3 -c 'import pytest' 2>/dev/null; then
		if python3 -m pytest tests/hil --collect-only -q -p no:cacheprovider > "$OUT/hil-collect.log" 2>&1; then
			ok "HIL tests collect ($(grep -cE '::' "$OUT/hil-collect.log") tests; run them with the radio, tests/hil/README.md)"
		else
			tail -20 "$OUT/hil-collect.log"
			bad "HIL test collection (see $OUT/hil-collect.log)"
		fi
	else
		skip "HIL test collection: pytest not installed (pip install pytest pyserial)"
	fi
	# the flashing tools: odrflash's host tests (sector planning, manifests, codec merge, fake bootloader runs) and
	# the self-tests of the developer tools (no USB, synthetic keys and stock files)
	if python3 -W ignore -m unittest discover -s tools/odrflash/tests > "$OUT/odrflash-tests.log" 2>&1 &&
	   python3 tools/codec_extract.py --self-test >> "$OUT/odrflash-tests.log" 2>&1 &&
	   python3 tools/stm32_dfu.py --self-test >> "$OUT/odrflash-tests.log" 2>&1; then
		ok "flashing tools: $(grep -E '^Ran [0-9]+ tests' "$OUT/odrflash-tests.log") (odrflash), codec_extract and stm32_dfu self-tests"
	else
		tail -30 "$OUT/odrflash-tests.log"
		bad "flashing tool tests (see $OUT/odrflash-tests.log)"
	fi
	if python3 tools/check_doc_links.py > "$OUT/doc-links.log" 2>&1; then
		ok "doc links: $(tail -1 "$OUT/doc-links.log" | sed 's/^check_doc_links: //')"
	else
		head -20 "$OUT/doc-links.log"
		bad "doc links (see $OUT/doc-links.log)"
	fi
else
	skip "python checks: no python3"
fi

# ── 3. firmware: every board, the feature matrix, sizes ──────────────────────
# build BOARD NAME [make variables...]: one configuration; appends a row to the size report
# "205744" "B" / "880" "KB" / "1" "MB" (the linker's --print-memory-usage units) to bytes; empty when unknown
tobytes() {
	case "$2" in
	B) echo "$1" ;;
	KB) echo $(($1 * 1024)) ;;
	MB) echo $(($1 * 1048576)) ;;
	*) echo "" ;;
	esac
}

build() {
	b=$1; name=$2; shift 2
	dir="$OUT/$b-$name"
	log="$dir.log"
	rm -f "$dir/odr_$b.elf"		# always relink: the size report reads the linker's memory-usage lines
	if ! make BOARD="$b" BUILD="$dir" -j"$JOBS" "$@" > "$log" 2>&1; then
		grep -E 'error|undefined' "$log" | head -10
		bad "$b $name: build failed ($*; see $log)"
		echo "| $b | $name | build failed | | | |" >> "$REPORT"
		return
	fi
	if grep -qi 'warning:' "$log"; then
		grep -i 'warning:' "$log" | sort -u | head -10
		bad "$b $name: warnings ($*; see $log)"
	fi
	bin=$(ls "$dir"/odr_"$b".bin 2>/dev/null)
	bytes=$(wc -c < "$bin" | tr -d ' ')
	# --print-memory-usage: "FLASH:  123024 B  336 KB  35.76%" (the app region, codec sectors excluded)
	set -- $(grep -E '^ *FLASH:' "$log" | head -1)
	used=$(tobytes "${2:-}" "${3:-}"); size=$(tobytes "${4:-}" "${5:-}")
	if [ -n "$size" ] && [ "$used" -le "$size" ] 2>/dev/null; then
		free=$((size - used))
		pct=$(awk "BEGIN { printf \"%.1f\", 100 * $used / $size }")
	else
		free="?"; pct="?"
	fi
	delta=""
	if [ "$name" != default ] && [ -n "${default_bytes:-}" ]; then
		delta=$((bytes - default_bytes))
	else
		default_bytes=$bytes
	fi
	echo "| $b | $name | $bytes | $delta | $free | $pct % |" >> "$REPORT"
	ok "$b $name: $bytes bytes, $free bytes free in the app region"
}

{
	echo "# Image sizes ($(git rev-parse --short=8 HEAD 2>/dev/null), $(date '+%Y-%m-%d %H:%M'))"
	echo
	echo "Bytes of the .bin; delta against the board's default; free = the linker's app FLASH region minus used (the"
	echo "codec windows are outside that region)."
	echo
	echo "| Board | Config | Image bytes | Delta | Free | Used |"
	echo "|---|---|---|---|---|---|"
} > "$REPORT"

for bm in boards/*/board.mk boards/*/; do
	case "$bm" in
	*/board.mk) ;;
	*) [ -f "$bm/board.mk" ] || skip "$(basename "$bm"): no boards/$(basename "$bm")/board.mk yet (its platform hasn't landed)"
	   continue ;;
	esac
	b=$(basename "$(dirname "$bm")")
	[ -n "$ONLY" ] && [ "$b" != "$ONLY" ] && continue
	echo "== firmware: $b"
	cc=$(make -s BOARD="$b" print-cc 2>/dev/null)
	if [ -z "$cc" ] || ! command -v "$cc" > /dev/null 2>&1; then
		skip "$b: compiler ${cc:-?} not installed (DM-32: tools/build-csky-toolchain.sh)"
		continue
	fi
	default_bytes=""
	build "$b" default
	# Single-band radios built per band: the other band's image too (default config)
	[ "$b" = md380 ] && build "$b" vhf MD380_BAND=vhf
	[ "$QUICK" = 1 ] && continue
	features=$(make -s BOARD="$b" print-config 2>/dev/null)
	all_on=""
	for kv in $features; do
		f=${kv%%=*}
		all_on="$all_on CONFIG_$f=y"
		[ "${kv#*=}" = y ] && build "$b" "no-$f" "CONFIG_$f=n"
	done
	build "$b" fm-minimal CONFIG_DMR=n CONFIG_USB=n CONFIG_VFO=n CONFIG_DIAG=n CONFIG_SUBTONE=n CONFIG_FONT_XL=n
	# shellcheck disable=SC2086
	build "$b" maximal $all_on
done
echo
cat "$REPORT"
echo

# ── 4. license checks ────────────────────────────────────────────────────────
echo "== licenses"
if tools/license_grep.sh; then ok "license_grep"; else bad "license_grep: markers of foreign code found"; fi
# third_party must stay submodule links (mode 160000) in HEAD: a worktree that symlinks them must never commit the
# symlinks (a merge would then replace the submodules in every checkout)
if git ls-tree HEAD third_party/ | awk '$1 != "160000" {bad=1} END {exit bad}'; then ok "third_party: submodule links"
else bad "third_party: entries in HEAD are not submodule links (git ls-tree HEAD third_party/)"; fi
if command -v reuse > /dev/null; then
	if reuse lint > "$OUT/reuse.log" 2>&1; then ok "reuse lint"; else tail -30 "$OUT/reuse.log"; bad "reuse lint (see $OUT/reuse.log)"; fi
else
	skip "reuse lint: reuse not installed (pipx install reuse, or pip install --user reuse)"
fi

# ── 5. similarity scan (author's machine only) ──────────────────────────────
REFS=${ODR_SIMILARITY_REFS:-}
if [ -n "$REFS" ]; then
	echo "== similarity scan"
	# shellcheck disable=SC2086
	if python3 tools/similarity_scan.py --report "$OUT/similarity.txt" $REFS; then
		ok "similarity scan: no shared token runs"
	else
		bad "similarity scan: shared token runs, review $OUT/similarity.txt by hand"
	fi
else
	skip "similarity scan: set ODR_SIMILARITY_REFS to the reference trees to run it"
fi

echo
if [ -n "$SKIPPED" ]; then
	echo "ci: skipped:$SKIPPED"
fi
if [ -n "$FAILED" ]; then
	echo "ci: FAILED:$FAILED"
	exit 1
fi
echo "ci: all checks passed"
