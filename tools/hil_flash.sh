#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Flashes a build onto the bench radio through the site's own flash command, then checks the radio runs that build
# (tests/hil/README.md, "Flashing for a run"). The repository holds no site's flash path: the stub, a bootloader or a
# remote host are the site's choice, given as $RFOUNDRY_HIL_FLASH_CMD, a shell command run with
#   RFOUNDRY_FLASH_BOARD  the board id
#   RFOUNDRY_FLASH_IMAGE  the image to write: rfoundry_<board>_codec.bin when the build merged the codec, else
#                         rfoundry_<board>_install.bin for a board with a first stage, else rfoundry_<board>.bin
#   RFOUNDRY_FLASH_PORT   the radio's port (empty: the only radio)
# It must return non-zero when the write fails, and return once the radio has restarted.
#
#   tools/hil_flash.sh BOARD BUILD_DIR [PORT]
#
# Prints the image's path last (for --firmware-image). Fails, and writes nothing more, on the first error: a failed
# write, or a radio whose VERSION git= isn't the commit BUILD_DIR's tree is at. The caller holds the radio lock.
set -eu
[ $# -ge 2 ] || { echo "usage: $0 BOARD BUILD_DIR [PORT]" >&2; exit 2; }
BOARD=$1 DIR=$2 PORT=${3:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PY=${PYTHON:-python3}
img=""
for f in "$DIR/rfoundry_${BOARD}_codec.bin" "$DIR/rfoundry_${BOARD}_install.bin" "$DIR/rfoundry_${BOARD}.bin"; do
	[ -f "$f" ] && { img=$f; break; }
done
[ -n "$img" ] || { echo "hil_flash: no image for $BOARD in $DIR (make BOARD=$BOARD BUILD=$DIR)" >&2; exit 1; }
[ -n "${RFOUNDRY_HIL_FLASH_CMD:-}" ] || {
	echo "hil_flash: set RFOUNDRY_HIL_FLASH_CMD to the site's flash command (tests/hil/README.md)" >&2; exit 1; }
# the commit the build shows (the Makefile's GITVERSION: the tree the build ran in)
want=$(git -C "$(dirname "$DIR")" rev-parse --short=8 HEAD 2>/dev/null || git -C "$ROOT" rev-parse --short=8 HEAD)
echo "hil_flash: $BOARD <- $img (commit $want)" >&2
RFOUNDRY_FLASH_BOARD=$BOARD RFOUNDRY_FLASH_IMAGE=$img RFOUNDRY_FLASH_PORT=$PORT \
	sh -c "$RFOUNDRY_HIL_FLASH_CMD" >&2 || { echo "hil_flash: the flash command failed" >&2; exit 1; }
# the radio may take a few seconds to come back on USB
tries=0
until ver=$("$PY" "$ROOT/tools/rfoundry_debug.py" ${PORT:+--port "$PORT"} version 2>/dev/null); do
	tries=$((tries + 1))
	[ "$tries" -lt 15 ] || { echo "hil_flash: the radio does not answer after the flash" >&2; exit 1; }
	sleep 2
done
got=$(printf '%s\n' "$ver" | sed -n 's/^git=//p')
case $got in
"$want"*) ;;
*) echo "hil_flash: the radio runs git=$got, not $want" >&2; exit 1 ;;
esac
echo "hil_flash: $BOARD runs $got, $(printf '%s\n' "$ver" | grep '^codec=' || echo 'codec=?')" >&2
echo "$img"
