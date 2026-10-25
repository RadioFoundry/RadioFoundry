#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The web flasher's tests: fixtures from rfoundry (Python), then the JavaScript modules against them (Node 20+).
#   site/tests/run.sh
set -eu
cd "$(dirname "$0")/../.."
OUT=${RFOUNDRY_SITE_FIXTURES:-build/site-tests}
python3 site/tests/fixtures.py "$OUT"
RFOUNDRY_SITE_FIXTURES="$OUT" node --test site/tests/*.test.mjs
