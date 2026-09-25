#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# Line, function and branch coverage of the host tests (docs/dev/ci.md "Coverage").
#
#   tools/coverage.sh                 rebuild tests/host with COVERAGE=1, run it, write the report
#   COVERAGE_FAIL_UNDER=85 tools/coverage.sh    also fail when line coverage is under 85 %
#
# Needs gcovr (pip install gcovr, or pipx install gcovr). With GCC it reads the data with gcov; with clang
# (Apple's included) with `llvm-cov gcov` (`xcrun llvm-cov gcov` on macOS). GCOV=... overrides the choice.
#
# Writes build/coverage/: index.html (per-file pages with the source annotated), coverage.xml (Cobertura, for
# tools that read it), summary.txt (per file) and summary.md (the totals and the least covered files, what CI puts in
# its job summary). Counted: core/, drivers/, hal/, boards/ and platform/ sources a host test compiles; tests/host
# itself, third_party/ and generated headers are not. Firmware files no host test builds are not in the report at all
# (so the percentage is of the code under host test, not of the whole firmware).
#
# The test binaries are rebuilt instrumented and the tree is cleaned before and after (make -C tests/host clean),
# so a later plain `make -C tests/host` builds normal binaries again. Exit status: the tests' (and the threshold's).
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=build/coverage
HOST=tests/host

if ! command -v gcovr > /dev/null 2>&1; then
	echo "coverage: gcovr not installed (pip install gcovr, or pipx install gcovr)" >&2
	exit 2
fi
CC=${CC:-cc}
if [ -z "${GCOV:-}" ]; then
	if "$CC" --version 2>/dev/null | grep -qi clang; then
		if command -v xcrun > /dev/null 2>&1; then GCOV="xcrun llvm-cov gcov"; else GCOV="llvm-cov gcov"; fi
	else
		GCOV=gcov
	fi
fi

rm -rf "$OUT"
mkdir -p "$OUT"
make -C "$HOST" clean > /dev/null
echo "== coverage: host tests built with --coverage ($CC, read with $GCOV)"
make -C "$HOST" COVERAGE=1 CC="$CC" > "$OUT/host-tests.log" 2>&1
status=$?
if [ $status -ne 0 ]; then
	tail -30 "$OUT/host-tests.log"
	echo "coverage: host tests FAILED (see $OUT/host-tests.log); the report covers what ran"
fi

# The same source built into several tests with different switches has different functions at the same lines:
# --merge-mode-functions=separate keeps them apart instead of stopping.
gcovr --root . --object-directory "$HOST" --gcov-executable "$GCOV" \
	--merge-mode-functions=separate \
	--filter 'core/' --filter 'drivers/' --filter 'hal/' --filter 'boards/' --filter 'platform/' \
	--exclude '.*/gen/' --exclude 'tests/' --exclude 'third_party/' \
	-j "${JOBS:-4}" \
	--txt "$OUT/summary.txt" --html-details "$OUT/index.html" --cobertura "$OUT/coverage.xml" \
	--json-summary "$OUT/summary.json" --print-summary > "$OUT/gcovr.log" 2>&1
gstatus=$?
grep -v '^(INFO)' "$OUT/gcovr.log"
make -C "$HOST" clean > /dev/null
if [ $gstatus -ne 0 ]; then
	echo "coverage: gcovr failed (see $OUT/gcovr.log)" >&2
	exit 1
fi

# summary.md: totals and the files with the most uncovered lines
python3 - "$OUT/summary.json" "$OUT/summary.md" "${COVERAGE_FAIL_UNDER:-}" <<'EOF' || status=1
import json, sys
s = json.load(open(sys.argv[1]))
rows = sorted(s["files"], key=lambda f: f["line_total"] - f["line_covered"], reverse=True)
out = ["## Host test coverage", "",
       "| Lines | Functions | Branches | Files |", "|---|---|---|---|",
       "| %.1f %% (%d / %d) | %.1f %% (%d / %d) | %.1f %% (%d / %d) | %d |" % (
           s["line_percent"], s["line_covered"], s["line_total"],
           s["function_percent"], s["function_covered"], s["function_total"],
           s["branch_percent"], s["branch_covered"], s["branch_total"], len(s["files"])),
       "", "Most uncovered lines:", "", "| File | Lines | Uncovered | Covered |", "|---|---|---|---|"]
for f in rows[:15]:
    out.append("| %s | %d | %d | %.1f %% |" % (f["filename"], f["line_total"], f["line_total"] - f["line_covered"],
                                             f["line_percent"]))
open(sys.argv[2], "w").write("\n".join(out) + "\n")
print("coverage: lines %.1f %%, functions %.1f %%, branches %.1f %% over %d files (build/coverage/index.html)" % (
    s["line_percent"], s["function_percent"], s["branch_percent"], len(s["files"])))
if sys.argv[3] and s["line_percent"] < float(sys.argv[3]):
    print("coverage: line coverage %.1f %% is under COVERAGE_FAIL_UNDER=%s" % (s["line_percent"], sys.argv[3]))
    sys.exit(1)
EOF
if [ -n "${GITHUB_STEP_SUMMARY:-}" ] && [ -f "$OUT/summary.md" ]; then
	cat "$OUT/summary.md" >> "$GITHUB_STEP_SUMMARY"
fi
exit $status
