#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Prints one value of a platform's [toolchain] table (platform/<platform>/platform.toml), for the shell scripts that
# install or run a toolchain. They can run before any Python 3.11 is there, so this reads the file with awk: a value
# is a quoted string on a "key = value" line of that table.
#
#   tools/platform_toolchain.sh stm32f4 version        13.3.rel1
set -eu
[ $# -eq 2 ] || { echo "usage: $0 PLATFORM KEY" >&2; exit 2; }
f="$(cd "$(dirname "$0")/.." && pwd)/platform/$1/platform.toml"
[ -f "$f" ] || { echo "platform_toolchain: no $f" >&2; exit 1; }
v=$(awk -v k="$2" '/^\[/ { t = ($0 == "[toolchain]") } t && $1 == k && $2 == "=" { v = $3; gsub(/"/, "", v); print v; exit }' "$f")
[ -n "$v" ] || { echo "platform_toolchain: $f has no [toolchain] $2" >&2; exit 1; }
echo "$v"
