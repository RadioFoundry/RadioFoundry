#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# The agent (snmpPeer) against independent managers: tools/odr_snmp.py always, and the system's SNMP command line
# tools (snmpget, snmpwalk, snmpbulkwalk, snmpset) when they are installed (used as external programs only).
set -e
cd "$(dirname "$0")"
PY="python3 -I ../../tools/odr_snmp.py"
PORT=$((20000 + $$ % 20000))
TPORT=$((PORT + 1))

./snmpPeer $PORT > snmpPeer.out &
PEER=$!
fail() { echo "snmp_peer_check: $*"; kill $PEER 2>/dev/null; exit 1; }
# Up when it answers (a loaded -j build can take seconds to start it), at most about 5 s
i=0
until $PY -p $PORT -t 0.2 get 127.0.0.1 sysName.0 > /dev/null 2>&1; do
	i=$((i + 1))
	[ $i -lt 25 ] || fail "snmpPeer didn't answer on port $PORT"
	kill -0 $PEER 2>/dev/null || fail "snmpPeer exited: $(cat snmpPeer.out)"
done

$PY -p $PORT get 127.0.0.1 sysName.0 odrDmrId.0 odrRssi.0 > snmpPeer.get || fail "odr_snmp get"
grep -q "sysName.0 = N0CALL" snmpPeer.get || fail "sysName: $(cat snmpPeer.get)"
grep -q "odrDmrId.0 = 3100001" snmpPeer.get || fail "odrDmrId: $(cat snmpPeer.get)"
grep -q "odrRssi.0 = -101" snmpPeer.get || fail "odrRssi: $(cat snmpPeer.get)"
$PY -p $PORT walk 127.0.0.1 > snmpPeer.walk || fail "odr_snmp walk"
N=$(wc -l < snmpPeer.walk | tr -d ' ')
[ "$N" -ge 60 ] || fail "walk: only $N instances"
$PY -p $PORT -c private set 127.0.0.1 sysName.0 s:W1AW > /dev/null || fail "odr_snmp set"
$PY -p $PORT get 127.0.0.1 sysName.0 | grep -q "= W1AW" || fail "set didn't take"
if $PY -p $PORT -t 0.5 set 127.0.0.1 sysName.0 s:X > /dev/null 2>snmpPeer.err; then fail "SET with public worked"; fi
grep -q noAccess snmpPeer.err || fail "SET with public: $(cat snmpPeer.err)"
echo "# odr_snmp.py: get, walk ($N instances), set, noAccess"

if command -v snmpwalk > /dev/null 2>&1; then
	snmpget -v2c -c public -t 1 -r 1 -On -m '' -Ln 127.0.0.1:$PORT 1.3.6.1.2.1.1.5.0 | grep -q 'STRING: "W1AW"' \
		|| fail "snmpget sysName"
	# (both walks end with a "No more variables" line for the endOfMibView; values that tick are left out)
	snmpwalk -v2c -c public -t 1 -r 1 -On -m '' -Ln 127.0.0.1:$PORT 1.3 | grep -v 'No more variables' \
		| cut -d' ' -f1 > snmpPeer.nwalk || fail "snmpwalk"
	M=$(wc -l < snmpPeer.nwalk | tr -d ' ')
	[ "$M" = "$N" ] || fail "snmpwalk saw $M instances, odr_snmp.py $N"
	snmpbulkwalk -v2c -c public -t 1 -r 1 -On -m '' -Ln -Cr7 127.0.0.1:$PORT 1.3 | grep -v 'No more variables' \
		| cut -d' ' -f1 > snmpPeer.bwalk || fail "snmpbulkwalk"
	cmp -s snmpPeer.nwalk snmpPeer.bwalk || fail "snmpbulkwalk differs from snmpwalk"
	snmpset -v2c -c private -t 1 -r 1 -On -m '' -Ln 127.0.0.1:$PORT 1.3.6.1.2.1.1.5.0 s N0CALL > /dev/null \
		|| fail "snmpset"
	snmpget -v2c -c public -t 1 -r 1 -On -m '' -Ln 127.0.0.1:$PORT 1.3.6.1.2.1.1.3.0 1.3.6.1.4.1.32473.2.1.3.7.0 \
		> snmpPeer.nget || fail "snmpget 2"
	grep -q "Timeticks" snmpPeer.nget && grep -q "Gauge32: 439412500" snmpPeer.nget || fail "$(cat snmpPeer.nget)"
	echo "# system tools: snmpget, snmpwalk ($M), snmpbulkwalk, snmpset agree"
	# docs/spec/ODR-MIB.txt names every enterprise instance the agent has
	snmpwalk -v2c -c public -t 1 -r 1 -OS -M "+$PWD/../../docs/spec" -m +ODR-MIB -Ln 127.0.0.1:$PORT \
		.1.3.6.1.4.1.32473 > snmpPeer.mwalk 2> snmpPeer.merr || fail "snmpwalk with ODR-MIB"
	U=$(grep -v -c '^ODR-MIB::odr[A-Za-z]*\.0 = ' snmpPeer.mwalk || true)
	E=$(wc -l < snmpPeer.mwalk | tr -d ' ')
	[ "$U" = "0" ] && [ "$E" -ge 40 ] || fail "ODR-MIB doesn't name ($E instances): $(grep -v '^ODR-MIB::' snmpPeer.mwalk | head -3)"
	grep -q -i -E "error|unlinked|cannot" snmpPeer.merr && fail "ODR-MIB.txt: $(head -3 snmpPeer.merr)"
	echo "# ODR-MIB.txt parses and names all $E enterprise instances"
else
	echo "# system SNMP tools not installed: skipped"
fi
wait $PEER || fail "the agent reported parse errors: $(cat snmpPeer.out)"

# Notifications: two traps to the listener
$PY -p $TPORT traps -n 2 > snmpPeer.traps &
LISTEN=$!
sleep 0.5
./snmpPeer --trap $TPORT
wait $LISTEN || fail "no traps"
grep -q "snmpTrapOID.0 = 1.3.6.1.4.1.32473.2.0.1" snmpPeer.traps || fail "traps: $(cat snmpPeer.traps)"
grep -q "odrEventSource.0 = 3100002" snmpPeer.traps || fail "trap bindings: $(cat snmpPeer.traps)"
grep -q "snmpTrapOID.0 = 1.3.6.1.6.3.1.1.5.1" snmpPeer.traps || fail "coldStart: $(cat snmpPeer.traps)"
echo "# notifications: odrCallStart and coldStart decoded"
rm -f snmpPeer.mwalk snmpPeer.merr snmpPeer.out snmpPeer.get snmpPeer.walk snmpPeer.err snmpPeer.nwalk snmpPeer.bwalk snmpPeer.nget snmpPeer.traps
