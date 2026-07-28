#!/bin/sh
# SPDX-License-Identifier: MIT
# Everything CI runs. The similarity scan needs the old trees, so it only runs
# where OLD_TREES is set (the author's machine), never in public CI.
set -e
cd "$(dirname "$0")/.."
if [ -f tests/host/Makefile ]; then make -C tests/host; fi
tools/license_grep.sh
if command -v reuse >/dev/null; then reuse lint; else echo "ci: reuse not installed, skipping reuse lint"; fi
if [ -n "$OLD_TREES" ]; then tools/similarity_scan.py $OLD_TREES; fi
