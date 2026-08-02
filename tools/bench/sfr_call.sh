#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# Adapted from our earlier bench tool run_sfr_call.sh.
#
# sfr_call.sh <name> <hat: voice|none> <seconds> [0xD5 payload, e.g. 00,00]
# One SFR test call: the radio under test as an SFR over its USB MMDVM link (mmdvm_hotspot.py --sfr), the bench-host HAT
# sending one voice call on the inbound slot, and rfgw's SDR receiver (sdr_watch.py) showing what went out on air.
# Planned firmware capability (USB + MMDVM + SFR in OpenDigitalRadio).
#
# Precondition: the HAT is NOT in rfgw's config (ROAM1 removed) while rfgw itself keeps running for the SDR decode.
# That is a config change on the bench-host: the lead arranges it with the owner. With the stock config use
# `dmr_bench.py run S1` instead (raw SDR capture, rfgw stopped and restored around it).
#
# Env: PORT (the radio's MMDVM serial port) or UDP (host:port), POWER (radio RF level), HATPOWER, LOGDIR
# (default /tmp/odr-bench), bench-host (default bench-host), BENCH_DIR (default ~/odr-bench, where hat_tx.py and
# sdr_watch.py are copied: `dmr_bench.py sync`), RECEIVER (default SDR15, the 461.6875 MHz SDR receiver).
set -u
S=$(cd "$(dirname "$0")" && pwd); L=${LOGDIR:-/tmp/odr-bench}; mkdir -p "$L"
C=${bench-host:-bench-host}; D=${BENCH_DIR:-~/odr-bench}; R=${RECEIVER:-SDR15}
name=$1; hat=$2; secs=$3; dbg=${4:-}
if [ -n "${UDP:-}" ]; then link="--udp $UDP"; else link="--port ${PORT:?set PORT to the radio serial port or UDP}"; fi
PY=${PY:-python3}
($PY -I "$S/mmdvm_hotspot.py" $link --sfr --seconds "$secs" ${POWER:+--power $POWER} ${dbg:+--debug $dbg} \
    > "$L/r_$name.log" 2>&1 &)
sleep 2
timeout 60 ssh "$C" "cd $D && PY=/opt/radiodesk-rfgw/.venv/bin/python; export \$(sudo cat /etc/radiodesk-rfgw/env | xargs); \
  (\$PY -W ignore sdr_watch.py --receiver $R --seconds $((secs-4)) > w_$name.log 2>&1 &); sleep 1; \
  if [ $hat = voice ]; then \$PY hat_tx.py ${HATPOWER:+--power $HATPOWER} voice --superframes 1 >/dev/null 2>&1; fi; \
  sleep $((secs-4)); cat w_$name.log" > "$L/w_$name.log"
sleep 2
echo "== $name: SDR $(grep -c slot "$L/w_$name.log") bursts, slot2 $(grep -c 'slot 2' "$L/w_$name.log"); families: $(grep -o 'slot [12] [a-z0-9/-]*' "$L/w_$name.log" | sort | uniq -c | tr '\n' ';')"
echo "   uplink: $(grep -c DMR_DATA1 "$L/r_$name.log") DATA1, $(grep -c LOST "$L/r_$name.log") LOST, terms $(grep -c 'control 42' "$L/r_$name.log")"
grep DEBUG "$L/r_$name.log" | tail -1
