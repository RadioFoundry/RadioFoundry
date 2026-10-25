#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Update RadioFoundry on the DB25-D with no keys: enter the stub from the running RadioFoundry, write the changed sectors of the install
# image (build/db25d*/rfoundry_db25d_install.bin: first stage + RadioFoundry), restart into RadioFoundry, check it answers.
#   stub/db25d_update.sh rfoundry_db25d_install.bin [PORT]
# The universal stub loads into whatever RAM span the running firmware reports and runs from 0x30000, so this one
# command works from a build with or without CONFIG_DMR and installs either kind (stub/README.md). Needs stubtool.py
# (this directory) and the debug link tool (tools/rfoundry_debug.py, or RFOUNDRY_DEBUG=path).
# Radio stuck (no RadioFoundry answering)? Power-on recovery: `stubtool.py PORT --wait 120 --board db25d --write-app IMAGE`, then
# turn the radio off and on (the first stage takes the universal stub at 0x10000; hardware-db25d.md 7). With the legacy
# stub instead: `--legacy-stub --load-base 0x30000`.
set -e
IMG=${1:?install image}
PORT=${2:-/dev/ttyUSB1}
cd "$(dirname "$0")"
DBG=${RFOUNDRY_DEBUG:-../tools/rfoundry_debug.py}
[ -f "$DBG" ] || DBG=rfoundry_debug.py
python3 stubtool.py "$PORT" --rfoundry-enter --write-app "$IMG"
python3 stubtool.py "$PORT" --attach --boot reset
sleep 4
python3 "$DBG" --port "$PORT" --timeout 2 version | grep -E "board|git|uptime"
