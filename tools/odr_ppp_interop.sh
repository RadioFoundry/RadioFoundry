#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# Interop test of the radio's PPP (core/net/ppp.c, docs/spec/ppp.md) with pppd, without a radio: tests/host/pppPeer
# runs the PPP personality, the IP gateway and a few services on a pseudo-terminal and pppd connects to it. Checks
# ping, HTTP on TCP 80, SNMP on UDP 161 (tools/odr_snmp.py), the debug link over UDP 3334 (tools/odr_debug.py), and
# that the link ends cleanly when pppd hangs up.
#
#   sudo tools/odr_ppp_interop.sh [--active]
#
# Needs root (pppd creates the interface), pppd, ping, curl and python3; on Linux /dev/ppp. In a container:
#   docker run --rm --cap-add NET_ADMIN --device /dev/ppp -v $PWD:/src -w /src ubuntu:24.04 sh -c \
#     'apt-get update -qq && apt-get install -y -qq ppp gcc make iputils-ping curl python3 > /dev/null && \
#      tools/odr_ppp_interop.sh'
# --active starts the radio side with its own Configure-Request (as the debug link's PPP op does).
set -e
cd "$(dirname "$0")/.."
ACTIVE=
[ "$1" = "--active" ] && ACTIVE=--active
RADIO=12.0.39.20			# pppPeer's DMR ID 10005: the host gets 12.0.39.21
OUT=${TMPDIR:-/tmp}/odr_ppp_interop.$$
mkdir -p "$OUT"
make -s -C tests/host pppPeer

tests/host/pppPeer --once $ACTIVE "$OUT/pty" > "$OUT/peer.log" 2>&1 &
PEER=$!
i=0
until [ -s "$OUT/pty" ]; do
	i=$((i + 1)); [ $i -lt 50 ] || { echo "pppPeer didn't start"; cat "$OUT/peer.log"; exit 1; }
	sleep 0.1
done
PTY=$(cat "$OUT/pty")
# pppd as the host would run it (tools/odr_ppp.sh), with IPv6CP and CCP left on: the radio must Protocol-Reject them
pppd "$PTY" 115200 noauth local nodetach nocrtscts noipdefault nodefaultroute novj +ipv6 \
	lcp-echo-interval 5 lcp-echo-failure 3 unit 7 debug logfile "$OUT/pppd.log" > /dev/null 2>&1 &
PPPD=$!
fail() { echo "FAIL: $*"; echo "--- pppPeer"; cat "$OUT/peer.log"; echo "--- pppd"; cat "$OUT/pppd.log"; \
	kill $PPPD $PEER 2>/dev/null; exit 1; }
i=0
until grep -q "IP up" "$OUT/peer.log" && ip addr show ppp7 2>/dev/null | grep -q "inet "; do
	i=$((i + 1)); [ $i -lt 100 ] || fail "no IP link after 10 s"
	sleep 0.1
done
ip addr show ppp7 | grep "inet "
ok=0
ping -c 3 -W 2 $RADIO || fail "ping"
ok=$((ok + 1))
curl -s -m 5 http://$RADIO/ | tee "$OUT/http" | grep -q "OpenDigitalRadio over PPP" || fail "HTTP"
ok=$((ok + 1))
python3 -I tools/odr_snmp.py -t 2 get $RADIO sysName.0 odrDmrId.0 | tee "$OUT/snmp" | grep -q "odrDmrId.0 = 10005" || fail "SNMP"
ok=$((ok + 1))
python3 -I tools/odr_debug.py --port udp:$RADIO ping || fail "debug link ping over UDP"
python3 -I tools/odr_debug.py --port udp:$RADIO version | grep -q "fw=pppPeer" || fail "debug link version over UDP"
ok=$((ok + 1))
# A 500 byte ping: frames near the MRU, every byte value escaped or not
ping -c 2 -s 500 -p 7e7d1113 -W 2 $RADIO > /dev/null || fail "large ping"
ok=$((ok + 1))
kill $PPPD		# pppd sends LCP Terminate-Request; the radio side acks and ends the link
wait $PEER || fail "pppPeer: the link didn't end cleanly"
grep -q "Protocol-Rej" "$OUT/pppd.log" && echo "pppd: Protocol-Reject seen (IPv6CP / CCP refused)"
grep -E "LCP|IPCP|link ended" "$OUT/peer.log" | tail -4
echo "odr_ppp_interop: $ok checks passed (logs in $OUT)"
