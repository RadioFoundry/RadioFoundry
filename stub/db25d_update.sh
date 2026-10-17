#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# Update ODR on the DB25-D with no keys: enter the stub from the running ODR, write the changed sectors of the install
# image (build/db25d*/odr_db25d_install.bin: first stage + ODR), restart into ODR, check it answers.
#   stub/db25d_update.sh odr_db25d_install.bin [PORT]
# The stub is chosen by the RAM span the running firmware reports, so this one command works from a build with or
# without CONFIG_DMR and installs either kind (stub/README.md). Needs stubtool.py (this directory) and the debug link
# tool (tools/odr_debug.py, or ODR_DEBUG=path).
# Radio stuck (no ODR answering)? Power-on recovery: `stubtool.py PORT --wait 120 --load-base 0x30000 --write-app IMAGE`,
# then turn the radio off and on (the first stage takes the stub at 0x30000; hardware-db25d.md 7). Plain --wait loads
# the DM-32UV's boot ROM stub, which the DB25-D doesn't run.
set -e
IMG=${1:?install image}
PORT=${2:-/dev/ttyUSB1}
cd "$(dirname "$0")"
DBG=${ODR_DEBUG:-../tools/odr_debug.py}
[ -f "$DBG" ] || DBG=odr_debug.py
python3 stubtool.py "$PORT" --odr-enter --write-app "$IMG"
python3 stubtool.py "$PORT" --attach --boot reset
sleep 4
python3 "$DBG" --port "$PORT" --timeout 2 version | grep -E "board|git|uptime"
