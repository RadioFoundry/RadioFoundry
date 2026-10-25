#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# IPv4 to a radio over its serial host port with pppd (CONFIG_NET_PPP builds; docs/spec/ppp.md): for radios that
# reach a computer only through a USB-serial cable, or as a fallback on the USB serial port.
#
#   sudo tools/rfoundry_ppp.sh /dev/ttyUSB0                 Linux
#   sudo tools/rfoundry_ppp.sh /dev/cu.usbserial-XXXX       macOS (/usr/sbin/pppd)
#   sudo tools/rfoundry_ppp.sh --stop                       hang up (LCP Terminate: the radio goes back to serial)
#
# pppd's first LCP frame gives the radio's host port to PPP by itself. The radio assigns the addresses (the same
# pair as on the USB network adapter: the host 12.<DMR ID>, the radio that xor 1); this script adds the routes to the
# other radios (12.0.0.0/8) and the talkgroups (225.0.0.0/8) through the link, as the adapter's DHCP does.
set -e
UNIT=7
IF=ppp$UNIT
PIDFILE=/var/run/$IF.pid

if [ "$1" = "--stop" ]; then
	[ -f $PIDFILE ] && kill "$(head -n 1 $PIDFILE)" && echo "rfoundry_ppp: $IF hung up" && exit 0
	echo "rfoundry_ppp: no $IF running"
	exit 1
fi
PORT=$1
[ -n "$PORT" ] || { sed -n '5,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[ "$(id -u)" = 0 ] || { echo "rfoundry_ppp: pppd needs root (sudo)"; exit 1; }
PPPD=$(command -v pppd || echo /usr/sbin/pppd)

# local, nocrtscts: the programming cable has no modem lines. noipdefault: the radio gives us our address.
# novj, noccp: the radio refuses them anyway. Echo requests notice a radio that went away. updetach: back here once
# the link is up
"$PPPD" "$PORT" 115200 noauth local nocrtscts noipdefault nodefaultroute novj noccp \
	lcp-echo-interval 10 lcp-echo-failure 3 unit $UNIT updetach

RADIO=$(ifconfig $IF 2>/dev/null | sed -n 's/.*--> *\([0-9.]*\).*/\1/p; s/.*destination \([0-9.]*\).*/\1/p' | head -n 1)
[ -n "$RADIO" ] || RADIO=$(ip -4 addr show $IF 2>/dev/null | sed -n 's/.*peer \([0-9.]*\).*/\1/p')
case "$(uname)" in
Darwin)
	route -q add -net 12.0.0.0/8 -interface $IF || true
	route -q add -net 225.0.0.0/8 -interface $IF || true
	;;
*)
	ip route replace 12.0.0.0/8 dev $IF
	ip route replace 225.0.0.0/8 dev $IF
	;;
esac
echo "rfoundry_ppp: radio at $RADIO on $IF. Try:"
echo "  ping $RADIO"
echo "  tools/rfoundry_debug.py --port udp:$RADIO view"
echo "  curl http://$RADIO/"
echo "  python3 tools/rfoundry_snmp.py get $RADIO sysName.0"
echo "  sudo $0 --stop"
