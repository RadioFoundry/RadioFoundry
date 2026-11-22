#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# An independent check of the frames a host test wrote to a pcap: tshark finds no malformed packets or bad checksums.
#
#   pcap_check.sh TSHARK udp PCAP [IGNORE]   IP and UDP checksums and the expert notes; lines matching the extended
#                                            regular expression IGNORE are expected (an ICMP error the test asked for)
#   pcap_check.sh TSHARK tcp PCAP            IP and TCP checksums and malformed segments
#
# The findings go to PCAP.expert. Exit status 1 when tshark found a problem.
set -u
tshark=$1 mode=$2 pcap=$3 ignore=${4:-}
expert=$pcap.expert

case $mode in
udp)
	"$tshark" -r "$pcap" -o ip.check_checksum:TRUE -o udp.check_checksum:TRUE -z expert,note -q > "$expert" || exit 1
	if [ -n "$ignore" ]; then
		found=$(grep -i -E "malformed|bad|incorrect|error" "$expert" | grep -v -E "$ignore")
	else
		found=$(grep -i -E "malformed|bad|incorrect|error" "$expert")
	fi
	;;
tcp)
	"$tshark" -r "$pcap" -o ip.check_checksum:TRUE -o tcp.check_checksum:TRUE -T fields -e _ws.malformed \
		-e _ws.expert.message > "$expert" 2>&1
	found=$(grep -i -E "malformed|bad checksum|checksum.*incorrect" "$expert")
	;;
*)
	echo "pcap_check.sh: unknown mode $mode" >&2
	exit 2
	;;
esac
if [ -n "$found" ]; then
	echo "# tshark found problems in $pcap"
	cat "$expert"
	exit 1
fi
echo "# tshark: $pcap has no malformed packets or bad checksums"
