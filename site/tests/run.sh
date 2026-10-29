#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The web flasher's tests: fixtures from rfoundry (Python), every radio's picture (radio_images_test.py), the rfoundry
# tool zip of the manual install kit (toolzip_test.py), the firmware download links (firmware_links_test.py), then the
# JavaScript modules against them (Node 20+).
#   site/tests/run.sh
set -eu
cd "$(dirname "$0")/../.."
OUT=${RFOUNDRY_SITE_FIXTURES:-build/site-tests}
python3 site/tests/fixtures.py "$OUT"
python3 site/tests/radio_images_test.py
python3 site/tests/toolzip_test.py
python3 site/tests/firmware_links_test.py
RFOUNDRY_SITE_FIXTURES="$OUT" node --test site/tests/*.test.mjs
