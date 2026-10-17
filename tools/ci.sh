#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# The release gate: everything CI runs, in one script (docs/testing/README.md).
#
#   tools/ci.sh                 host tests, Python checks, every board with its feature matrix, license checks
#   tools/ci.sh --quick         the same with only each board's default build (no matrix)
#   tools/ci.sh --board dm1701  one board only
#   tools/ci.sh --matrix        only the feature matrix (no default builds; what the nightly CI adds per push)
#   tools/ci.sh --steps host,python   only these steps (host, python, firmware, licenses, similarity; default all)
#   JOBS=8 tools/ci.sh          parallel make jobs (default: the CPU count)
#   ODR_CI_SANITIZE=1 tools/ci.sh   the host step also runs the host tests under ASan and UBSan (make SANITIZE=1)
#   ODR_CI_PRUNE=1 tools/ci.sh  remove each build tree once its size is in the report (the log and the .bin stay;
#                               the matrix otherwise leaves about 65 MB per configuration)
#
# The CI workflows (docs/dev/ci.md) run pieces of it: per push `--steps host,python,licenses` and
# `--quick --steps firmware --board <b>`; nightly `--matrix --steps firmware --board <b>`.
#
# Steps, each reported as ok / FAIL / skip:
#   1. host tests (make -C tests/host), and with ODR_CI_SANITIZE=1 again under ASan and UBSan
#   2. Python: every tool and HIL test parses; pytest collects tests/hil (when pytest is installed); the flashing tools'
#      tests (tools/odrflash/tests, codec_extract, stm32_dfu, codeplug_port, md380_migrate, dm32_flash, dm32_migrate
#      and dm32_codec self-tests) and the UI gallery comparison's (ui_gallery_compare); every relative link, image and anchor in the Markdown resolves
#      (tools/check_doc_links.py); the web site builds without a warning (site/build.py, needs markdown-it-py or uv)
#      and the web flasher's tests pass against odrflash (site/tests/run.sh, needs Node 20+)
#   3. firmware, for every boards/<b>/board.mk whose compiler is installed (a board without one is skipped with a
#      notice: the DM-32's csky toolchain comes from tools/build-csky-toolchain.sh):
#        default            the board's defaults
#        no-<FEATURE>       each switch that is on by default, turned off alone (sub-features follow), except those
#                           the board can't build without (BOARD_REQUIRED: FM on the DB25-D while DMR isn't built)
#        fm-minimal         FM only: no DMR, USB, VFO, diagnostics, sub-audio tones
#        maximal            every switch on, except those the board lists as unsupported (BOARD_UNSUPPORTED) or as
#                           not needed (the board's MAXIMAL_SKIP: NET_PPP on the radios with the USB network adapter)
#                           and the bench diagnostics (MAXIMAL_SKIP here: AUDIO_PROBE), with the board's MAXIMAL_VARS
#                           (the DM-1701: LTO=1)
#        audio-probe        (DM-1701 only, the board with an audio tap) the default config with CONFIG_AUDIO_PROBE=y
#                           and LTO=1 (without LTO it no longer fits), so the probe keeps building
#        vhf                (MD-380 only) the VHF band image, default config
#      and the STM32F4 flash stub (stub/stm32f4, with the DM-1701 or MD-380 compiler).
#      Each must build with zero compiler and linker warnings. Sizes go to build/ci/size-report.md.
#   4. tools/license_grep.sh, and `reuse lint` (skipped with a notice when reuse isn't installed: pipx install reuse)
#   5. tools/similarity_scan.py against the trees in $ODR_SIMILARITY_REFS (space separated), only when it is set:
#      the reference trees are never part of this repository or of public CI
#
# Build trees: build/ci/<board>-<config>/ (logs next to them). The exit status is 0 only when nothing failed.
set -u
# Switches the maximal configuration leaves off on every board: bench diagnostics, never in a release
MAXIMAL_SKIP="AUDIO_PROBE"
cd "$(dirname "$0")/.." || exit 2

QUICK=0
MATRIX=0
ONLY=""
STEPS="host,python,firmware,licenses,similarity"
while [ $# -gt 0 ]; do
	case "$1" in
	--quick) QUICK=1 ;;
	--matrix) MATRIX=1 ;;
	--board) ONLY="$2"; shift ;;
	--steps) STEPS="$2"; shift ;;
	-h|--help) sed -n '5,38p' "$0"; exit 0 ;;
	*) echo "ci: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
for s in $(echo "$STEPS" | tr ',' ' '); do
	case "$s" in host|python|firmware|licenses|similarity) ;; *) echo "ci: unknown step $s" >&2; exit 2 ;; esac
done
[ "$QUICK" = 1 ] && [ "$MATRIX" = 1 ] && { echo "ci: --quick and --matrix exclude each other" >&2; exit 2; }
step() { case ",$STEPS," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }
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
if step host; then
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
if [ "${ODR_CI_SANITIZE:-0}" = 1 ]; then
	# the binaries don't depend on the flags: clean before and after, so later steps get normal builds
	make -s -C tests/host clean > /dev/null
	if make -C tests/host -j"$JOBS" SANITIZE=1 > "$OUT/host-sanitize.log" 2>&1; then
		ok "host tests under ASan and UBSan"
	else
		grep -E -A12 'ERROR: AddressSanitizer|runtime error:' "$OUT/host-sanitize.log" | head -40
		tail -15 "$OUT/host-sanitize.log"
		bad "host tests under ASan and UBSan (see $OUT/host-sanitize.log)"
	fi
	make -s -C tests/host clean > /dev/null
fi

fi

# ── 2. Python ────────────────────────────────────────────────────────────────
if step python; then
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
	   python3 tools/stm32_dfu.py --self-test >> "$OUT/odrflash-tests.log" 2>&1 &&
	   python3 tools/codeplug_port.py --self-test >> "$OUT/odrflash-tests.log" 2>&1 &&
	   python3 tools/md380_migrate.py --self-test >> "$OUT/odrflash-tests.log" 2>&1 &&
	   python3 tools/dm32_flash.py --self-test >> "$OUT/odrflash-tests.log" 2>&1 &&
	   python3 tools/dm32_migrate.py --self-test >> "$OUT/odrflash-tests.log" 2>&1 &&
	   python3 tools/dm32_codec.py --self-test >> "$OUT/odrflash-tests.log" 2>&1 &&
	   python3 tools/ui_gallery_compare.py --self-test >> "$OUT/odrflash-tests.log" 2>&1 &&
	   python3 stub/f4stub.py --self-test >> "$OUT/odrflash-tests.log" 2>&1; then
		ok "flashing tools: $(grep -E '^Ran [0-9]+ tests' "$OUT/odrflash-tests.log") (odrflash), codec_extract, stm32_dfu, codeplug_port, md380_migrate, dm32_flash, dm32_migrate, dm32_codec, ui_gallery_compare and f4stub self-tests"
	else
		tail -30 "$OUT/odrflash-tests.log"
		bad "flashing tool tests (see $OUT/odrflash-tests.log)"
	fi
	# the APRS AFSK decoder the HIL tests run on the SDR bench: frames, synthesized I/Q and the firmware's own WAV
	if python3 -c 'import numpy' 2>/dev/null; then
		if make -s -C tests/host aprsTest > "$OUT/aprs-afsk.log" 2>&1 &&	# built by the host step, unless skipped
		   tests/host/aprsTest --wav "$OUT" > /dev/null 2>&1 &&
		   python3 tools/aprs_afsk_check.py --self-test --wav "$OUT/aprs_test_44k1.wav" >> "$OUT/aprs-afsk.log" 2>&1; then
			ok "APRS AFSK decoder self-test ($(grep -c '^ok' "$OUT/aprs-afsk.log") checks)"
		else
			tail -20 "$OUT/aprs-afsk.log"
			bad "APRS AFSK decoder self-test (see $OUT/aprs-afsk.log)"
		fi
	else
		skip "APRS AFSK decoder self-test: numpy not installed"
	fi
	if python3 tools/check_doc_links.py > "$OUT/doc-links.log" 2>&1; then
		ok "doc links: $(tail -1 "$OUT/doc-links.log" | sed 's/^check_doc_links: //')"
	else
		head -20 "$OUT/doc-links.log"
		bad "doc links (see $OUT/doc-links.log)"
	fi
	# the web site (docs/dev/website.md): its generator with markdown-it-py, from the environment or through uv
	if python3 -c "import markdown_it" 2> /dev/null; then SITE_PY="python3"
	elif command -v uv > /dev/null; then SITE_PY="uv run --no-project -q --with-requirements site/requirements.txt python3"
	else SITE_PY=""; fi
	if [ -n "$SITE_PY" ]; then
		if $SITE_PY site/build.py --strict --out "$OUT/site" > "$OUT/site.log" 2>&1; then
			ok "web site: $(tail -1 "$OUT/site.log" | sed 's/^site: //')"
		else
			tail -20 "$OUT/site.log"
			bad "web site build (see $OUT/site.log)"
		fi
	else
		skip "web site build: markdown-it-py not installed (pip install -r site/requirements.txt, or install uv)"
	fi
	if command -v node > /dev/null; then
		if ODR_SITE_FIXTURES="$OUT/site-tests" site/tests/run.sh > "$OUT/site-tests.log" 2>&1; then
			ok "web flasher tests: $(grep -E '^. pass' "$OUT/site-tests.log" | sed 's/^. //') against odrflash"
		else
			tail -30 "$OUT/site-tests.log"
			bad "web flasher tests (see $OUT/site-tests.log)"
		fi
	else
		skip "web flasher tests: node not installed"
	fi
else
	skip "python checks: no python3"
fi
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
		echo "| $b | $name | build failed | | | | |" >> "$REPORT"
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
		# the app region's fit: the linker fails on an overflow; this catches a log it couldn't be read from
		[ -n "$size" ] && [ -n "$used" ] && bad "$b $name: $used bytes used in a $size byte app region"
	fi
	# a board with a second flash region (the DM-1701's sector 9, FLASH2 in its linker script): the bytes in it
	set -- $(grep -E '^ *FLASH2:' "$log" | head -1)
	if [ -n "${2:-}" ]; then s9=$(tobytes "${2:-}" "${3:-}"); else s9="-"; fi
	delta=""
	if [ "$name" = default ]; then
		default_bytes=$bytes
	elif [ -n "${default_bytes:-}" ]; then
		delta=$((bytes - default_bytes))
	fi
	echo "| $b | $name | $bytes | $delta | $free | $pct % | $s9 |" >> "$REPORT"
	ok "$b $name: $bytes bytes, $free bytes free in the app region$( [ "$s9" != - ] && echo ", $s9 bytes in sector 9")"
	if [ "${ODR_CI_PRUNE:-0}" = 1 ]; then
		cp "$bin" "$dir.bin" && rm -rf "$dir"
	fi
}

if step firmware; then
{
	echo "# Image sizes ($(git rev-parse --short=8 HEAD 2>/dev/null), $(date '+%Y-%m-%d %H:%M'))"
	echo
	echo "Bytes of the .bin; delta against the board's default; free = the linker's app FLASH region minus used (the"
	echo "codec windows are outside that region); sector 9 = bytes in the linker's FLASH2 region (the DM-1701's constant"
	echo "data, a second image written with the application)."
	echo
	echo "| Board | Config | Image bytes | Delta | Free | Used | Sector 9 |"
	echo "|---|---|---|---|---|---|---|"
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
	if [ "$MATRIX" = 0 ]; then
		build "$b" default
		# Single-band radios built per band: the other band's image too (default config)
		[ "$b" = md380 ] && build "$b" vhf MD380_BAND=vhf
	fi
	[ "$QUICK" = 1 ] && continue
	features=$(make -s BOARD="$b" print-config 2>/dev/null)
	all_on=""
	unsupported=" $(make -s BOARD="$b" print-unsupported 2>/dev/null) $(make -s BOARD="$b" print-maximal-skip 2>/dev/null) $MAXIMAL_SKIP "
	required=" $(make -s BOARD="$b" print-required 2>/dev/null) "
	for kv in $features; do
		f=${kv%%=*}
		case "$unsupported" in *" $f "*) ;; *) all_on="$all_on CONFIG_$f=y" ;; esac
		case "$required" in *" $f "*) continue ;; esac
		[ "${kv#*=}" = y ] && build "$b" "no-$f" "CONFIG_$f=n"
	done
	[ "$b" = dm1701 ] && build "$b" audio-probe CONFIG_AUDIO_PROBE=y CONFIG_APRS_RX=n LTO=1
	build "$b" fm-minimal CONFIG_DMR=n CONFIG_USB=n CONFIG_VFO=n CONFIG_DIAG=n CONFIG_SUBTONE=n CONFIG_FONT_XL=n
	# shellcheck disable=SC2086
	build "$b" maximal $all_on $(make -s BOARD="$b" print-maximal-vars 2>/dev/null)
done
# The STM32F4 flash stub (stub/stm32f4): one image for the DM-1701 and the MD-380, loaded by CONFIG_STUB_ENTRY builds
if [ -z "$ONLY" ] || [ "$ONLY" = dm1701 ] || [ "$ONLY" = md380 ]; then
	cc=$(make -s BOARD=dm1701 print-cc 2>/dev/null)
	if [ -z "$cc" ] || ! command -v "$cc" > /dev/null 2>&1; then
		skip "STM32F4 flash stub: compiler ${cc:-?} not installed"
	elif make -C stub/stm32f4 BUILD="$(pwd)/$OUT/stub-stm32f4" > "$OUT/stub-stm32f4.log" 2>&1; then
		if grep -qi 'warning:' "$OUT/stub-stm32f4.log"; then
			grep -i 'warning:' "$OUT/stub-stm32f4.log" | sort -u | head -10
			bad "STM32F4 flash stub: warnings (see $OUT/stub-stm32f4.log)"
		elif ! python3 stub/f4stub.py --check-embedded "$OUT/stub-stm32f4/f4stub.bin" > /dev/null; then
			bad "STM32F4 flash stub: the copy embedded in stub/f4stub.py differs from this build (python3 stub/f4stub.py --embed $OUT/stub-stm32f4/f4stub.bin)"
		else
			ok "STM32F4 flash stub: $(wc -c < "$OUT/stub-stm32f4/f4stub.bin" | tr -d ' ') bytes (at most 8192), the embedded copy matches"
		fi
	else
		grep -E 'error|ASSERT' "$OUT/stub-stm32f4.log" | head -10
		bad "STM32F4 flash stub: build failed (see $OUT/stub-stm32f4.log)"
	fi
fi
echo
cat "$REPORT"
echo
fi

# ── 4. license checks ────────────────────────────────────────────────────────
if step licenses; then
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
fi

# ── 5. similarity scan (author's machine only) ──────────────────────────────
REFS=${ODR_SIMILARITY_REFS:-}
if ! step similarity; then
	:
elif [ -n "$REFS" ]; then
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
