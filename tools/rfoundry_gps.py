#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""rfoundry_gps: the radio's GPS fix on the host, over USB network or PPP (docs/spec/tcp-services.md 8.4). Asks the
radio's UDP control (port 4191) for fixes, renews the request every 60 s, and prints each fix as it comes, once a
second: as the radio's JSON, as NMEA (GGA and RMC), or as NMEA to a UDP port for gpsd or a mapping program.

    rfoundry_gps.py                                  JSON lines, the radio found as rfoundry_debug.py finds it
    rfoundry_gps.py --radio 12.0.39.20 --nmea        NMEA sentences on stdout
    rfoundry_gps.py --nmea-udp 127.0.0.1:4349        NMEA datagrams there, e.g. for gpsd -N udp://127.0.0.1:4349
    rfoundry_gps.py --seconds 60                     stop after 60 s (exit 1 if no fix came)

The radio has one subscriber: running this takes the fixes and zone notices over from another client on the link,
takRF included, until that client asks again.

Before the first request it asks the debug link (UDP port 3334 on the same address) whether the firmware has the UDP
control: on firmware without it, a datagram to port 4191 would go out over the air as DMR data. --no-check skips that
(the radio must then have NET_RADIO_UDP). Exit status 0 on success, 1 if the radio has no GPS or no fix came, 2 on a
usage or connection problem.
"""
import argparse
import datetime
import json
import os
import socket
import sys
import time

PORT = 4191     # NET_RADIO_UDP_PORT
RENEW_S = 60    # the lease is 120 s


def nmea(body):
    """$body*CS with its CR LF"""
    cs = 0
    for ch in body.encode("ascii"):
        cs ^= ch
    return f"${body}*{cs:02X}\r\n"


def _coord(deg, width, pos, neg):
    """ddmm.mmmmm (width 2) or dddmm.mmmmm (width 3) and its hemisphere"""
    a = abs(deg)
    d = int(a)
    m = (a - d) * 60
    if round(m, 5) >= 60:       # 59.999999' prints as 60.00000
        d, m = d + 1, 0.0
    return f"{d:0{width}d}{m:08.5f}", (neg if deg < 0 else pos)


def fix_to_nmea(fix, now=None):
    """GGA and RMC for one fix (the radio's JSON as a dict). now: the host's UTC time (default: the clock), for RMC's
    date (a fix carries the time only); a fix from just before midnight while the host is already past it gets
    yesterday's"""
    utc = int(fix["utc"])
    hh, mm, ss = utc // 10000, utc // 100 % 100, utc % 100
    now = now or datetime.datetime.now(datetime.timezone.utc)
    date = now.date()
    if hh * 3600 + mm * 60 + ss - (now.hour * 3600 + now.minute * 60 + now.second) > 12 * 3600:
        date -= datetime.timedelta(days=1)
    t = f"{hh:02d}{mm:02d}{ss:02d}.00"
    lat, ns = _coord(float(fix["lat"]), 2, "N", "S")
    lon, ew = _coord(float(fix["lon"]), 3, "E", "W")
    hdop = f"{fix['hdop']:.1f}" if "hdop" in fix else ""
    alt = f"{fix['alt_m']:.1f},M" if "alt_m" in fix else ","
    gga = nmea(f"GPGGA,{t},{lat},{ns},{lon},{ew},1,{int(fix.get('sats', 0)):02d},{hdop},{alt},,M,,")
    kn = f"{fix['kmh'] / 1.852:.1f}" if "kmh" in fix else ""
    course = f"{fix['course']:.1f}" if "course" in fix else ""
    rmc = nmea(f"GPRMC,{t},A,{lat},{ns},{lon},{ew},{kn},{course},{date:%d%m%y},,,A")
    return gga + rmc


def check_firmware(radio):
    """The radio's features over the debug link: the UDP control must be there before anything goes to 4191"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import rfoundry_debug
    try:
        with rfoundry_debug.DebugLink(f"udp:{radio}") as link:
            features = set(link.version().get("features", "").split(","))
    except rfoundry_debug.DebugLinkError as e:
        sys.exit(f"rfoundry_gps: no debug link answer from {radio}: {e} (--no-check skips this, see --help)")
    if "NET_RADIO_UDP" not in features:
        sys.exit(f"rfoundry_gps: {radio}'s firmware has no NET_RADIO_UDP: nothing sent (it would go over the air)")


def find_radio():
    """The radio's address, as rfoundry_debug.py finds a radio in network mode"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import rfoundry_debug
    try:
        port = rfoundry_debug.pick_port(None)
    except rfoundry_debug.DebugLinkError as e:
        sys.exit(f"rfoundry_gps: {e}; give --radio")
    if not port.startswith("udp:"):
        sys.exit(f"rfoundry_gps: the radio is on {port}, not in USB network or PPP mode; give --radio")
    return port[4:].partition(":")[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--radio", help="the radio's address on the link (default: found as rfoundry_debug.py does)")
    out = ap.add_mutually_exclusive_group()
    out.add_argument("--nmea", action="store_true", help="NMEA GGA and RMC on stdout instead of JSON")
    out.add_argument("--nmea-udp", metavar="HOST:PORT", help="NMEA datagrams to HOST:PORT (gpsd udp://HOST:PORT)")
    ap.add_argument("--seconds", type=float, help="stop after this long")
    ap.add_argument("--no-check", action="store_true", help="don't ask the debug link for NET_RADIO_UDP first")
    ap.add_argument("-v", "--verbose", action="store_true", help="also the replies and zone notices, to stderr")
    a = ap.parse_args()

    radio = a.radio or find_radio()
    if not a.no_check:
        check_firmware(radio)
    to = None
    if a.nmea_udp:
        host, _, port = a.nmea_udp.rpartition(":")
        if not host or not port.isdigit():
            ap.error("--nmea-udp HOST:PORT")
        to = (host, int(port))

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((radio, PORT))    # the replies and fixes come from there; anything else is dropped
    out_s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) if to else None
    end = time.monotonic() + a.seconds if a.seconds else None
    fixes = 0
    renew = 0.0
    try:
        while end is None or time.monotonic() < end:
            now = time.monotonic()
            if now >= renew:
                s.send(b"gps")
                renew = now + RENEW_S
            wait = min(renew, end) if end else renew
            s.settimeout(max(0.05, wait - time.monotonic()))
            try:
                data = s.recv(2048)
            except socket.timeout:
                continue
            except ConnectionRefusedError:
                print(f"rfoundry_gps: {radio} refused port {PORT}", file=sys.stderr)
                renew = time.monotonic() + 5
                continue
            try:
                msg = json.loads(data)
            except ValueError:
                continue
            if "gps" not in msg:
                if a.verbose:
                    print(data.decode("ascii", "replace").rstrip(), file=sys.stderr)
                continue
            if "zone" in msg:   # the reply to "gps": does the radio have one
                if a.verbose:
                    print(data.decode("ascii", "replace").rstrip(), file=sys.stderr)
                if msg["gps"] == 0:
                    sys.exit("rfoundry_gps: the radio has no GPS")
                continue
            fixes += 1
            if to:
                out_s.sendto(fix_to_nmea(msg).encode("ascii"), to)
            elif a.nmea:
                sys.stdout.write(fix_to_nmea(msg))
            else:
                sys.stdout.write(data.decode("ascii", "replace"))
            sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            s.send(b"gps?off")
        except OSError:
            pass
    return 0 if fixes or end is None else 1


if __name__ == "__main__":
    sys.exit(main())
