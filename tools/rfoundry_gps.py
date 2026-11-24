#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""rfoundry_gps: the radio's GPS fix on the host, over USB network or PPP (docs/spec/tcp-services.md 5.1). Asks the
radio's /ws for fixes and prints each as it comes, once a second: as the radio's JSON, as NMEA (GGA and RMC), or as
NMEA to a UDP port for gpsd or a mapping program. It asks for the status only on a zone or channel change, so the
link carries little more than the fixes.

    rfoundry_gps.py                                  JSON lines, the radio found as rfoundry_debug.py finds it
    rfoundry_gps.py --radio 12.0.39.20 --nmea        NMEA sentences on stdout
    rfoundry_gps.py --nmea-udp 127.0.0.1:4349        NMEA datagrams there, e.g. for gpsd -N udp://127.0.0.1:4349
    rfoundry_gps.py --seconds 60                     stop after 60 s (exit 1 if no fix came)

/ws takes one client: while this runs, the radio's own page and takRF can't connect. Fixes come only to a client on
the host link (the computer the radio is plugged into).

Before connecting it asks the debug link (UDP port 3334 on the same address) whether the firmware has web control:
on firmware without the IP services, a packet to the radio's address could go out over the air as DMR data.
--no-check skips that (the radio must then have NET_WEBCTL). Exit status 0 on success, 1 if the radio has no GPS or
no fix came, 2 on a usage or connection problem.
"""
import argparse
import datetime
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rfoundry_ws  # noqa: E402


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
    """The radio's features over the debug link: web control must be there before anything goes to port 80"""
    import rfoundry_debug
    try:
        with rfoundry_debug.DebugLink(f"udp:{radio}") as link:
            features = set(link.version().get("features", "").split(","))
    except rfoundry_debug.DebugLinkError as e:
        sys.exit(f"rfoundry_gps: no debug link answer from {radio}: {e} (--no-check skips this, see --help)")
    if "NET_WEBCTL" not in features:
        sys.exit(f"rfoundry_gps: {radio}'s firmware has no NET_WEBCTL: nothing sent")


def find_radio():
    """The radio's address, as rfoundry_debug.py finds a radio in network mode"""
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
    ap.add_argument("--no-check", action="store_true", help="don't ask the debug link for NET_WEBCTL first")
    ap.add_argument("-v", "--verbose", action="store_true", help="also the hello and the status, to stderr")
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

    out_s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) if to else None
    end = time.monotonic() + a.seconds if a.seconds else None
    fixes = 0
    try:
        w = rfoundry_ws.WebControl(radio)
    except (rfoundry_ws.WebControlError, OSError) as e:
        print(f"rfoundry_gps: {e}", file=sys.stderr)
        return 2
    try:
        if a.verbose:
            print(json.dumps(w.hello), file=sys.stderr)
        if "gps" not in w.hello.get("caps", []):
            print("rfoundry_gps: the radio has no GPS (or firmware without fixes on /ws)", file=sys.stderr)
            return 1
        if not w.hello.get("control"):
            print("rfoundry_gps: fixes go only to the host the radio is plugged into", file=sys.stderr)
            return 1
        w.send("status?changes")
        w.send("gps")
        while end is None or time.monotonic() < end:
            msg = w.recv(1.0 if end is None else max(0.05, min(1.0, end - time.monotonic())))
            if msg is None:
                continue
            if "err" in msg:
                print(f"rfoundry_gps: the radio refused: {msg}", file=sys.stderr)
                return 1
            if "lat" not in msg:
                if a.verbose:
                    print(json.dumps(msg), file=sys.stderr)
                continue
            fixes += 1
            if to:
                out_s.sendto(fix_to_nmea(msg).encode("ascii"), to)
            elif a.nmea:
                sys.stdout.write(fix_to_nmea(msg))
            else:
                sys.stdout.write(json.dumps(msg, separators=(",", ":")) + "\n")
            sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    except (rfoundry_ws.WebControlError, OSError) as e:
        print(f"rfoundry_gps: {e}", file=sys.stderr)
        return 2
    finally:
        w.close()
    return 0 if fixes or end is None else 1


if __name__ == "__main__":
    sys.exit(main())
