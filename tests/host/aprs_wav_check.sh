#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Black-box check of our Bell 202 / AX.25 output with an independent decoder (docs/spec/aprs.md 13): aprsTest writes
# a WAV with three frames, Direwolf's "atest" decodes it. Optional: skipped (exit 0) when atest isn't installed.
# Direwolf is only run here, never read for code (CLEANROOM.md).
#
#   tests/host/aprs_wav_check.sh        (after make -C tests/host)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
if ! command -v atest >/dev/null 2>&1; then
	echo "aprs_wav_check: atest (Direwolf) not installed, skipped"
	exit 0
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/aprs_wav.XXXXXX")
"$here/aprsTest" --wav "$out" >/dev/null
log="$out/atest.log"
atest -B 1200 "$out/aprs_test_44k1.wav" >"$log" 2>&1 || true
n=$(grep -c "APZRDF" "$log" || true)
echo "aprs_wav_check: atest decoded $n of 3 frames"
grep "APZRDF" "$log" || true
if [ "$n" -lt 3 ]; then
	echo "aprs_wav_check: FAILED (log in $log)"
	exit 1
fi
