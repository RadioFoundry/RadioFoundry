#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""
APRS host helper for an OpenDigitalRadio radio in KISS mode (docs/spec/aprs.md 9): the APRS-IS connection for the
radio's iGate, a KISS TCP port for APRS programs (APRSdroid, Xastir, YAAC ...), and NMEA forwarding from a GPS on the
computer.

    aprs_gate.py --port /dev/cu.usbmodemXXXX | --port udp:192.168.10.1 [--call N0CALL] [--passcode N]
                 [--server rotate.aprs2.net:14580] [--filter "m/50"] [--no-is]
                 [--kiss-tcp 8001] [--nmea /dev/cu.gps | --nmea-file track.nmea] [-v]

The radio gates and decides (the iGate rules are in the firmware, aprs_igate.c); this tool only carries lines:

- radio -> APRS-IS: KISS SetHardware frames tagged 'G' carry finished APRS-IS lines (RF packets with qAR, our own
  beacons with TCPIP*). Sent as they are, with CR LF.
- APRS-IS -> radio: every server line that isn't a comment ('#') goes to the radio as a SetHardware 'G' frame; the
  radio passes on only messages to stations it heard directly (iGate TX on), and drops the rest.
- KISS TCP clients <-> radio: DATA frames both ways (what the radio hears, RX builds; what the client sends goes out
  on RF if its source is the radio's callsign, rate limited by the radio).
- NMEA: $GPRMC / $GPGGA lines from a serial GPS or a file go to the radio as SetHardware 'N' frames (the radio's
  "Computer" position source).

Turn KISS on first: Tools > APRS > KISS on USB, or `odr_debug.py aprs set kiss 1`. The radio must not be in hotspot
mode. The passcode is computed from the callsign when not given (the public APRS-IS hash); without --call the tool
logs in receive-only ("-1"), which APRS-IS takes for reading but not for gating.
"""
import argparse
import logging
import os
import selectors
import socket
import sys
import time

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
CMD_DATA, CMD_SETHW = 0x00, 0x06
TAG_IS, TAG_NMEA = ord("G"), ord("N")
VERSION = "ODR-aprs_gate 1.0"


def kiss_encode(cmd: int, payload: bytes) -> bytes:
    out = bytearray([FEND, cmd & 0x0F])
    for b in payload:
        if b == FEND:
            out += bytes([FESC, TFEND])
        elif b == FESC:
            out += bytes([FESC, TFESC])
        else:
            out.append(b)
    out.append(FEND)
    return bytes(out)


class KissDecoder:
    """Bytes in, (type byte, payload) frames out."""

    def __init__(self):
        self.buf = bytearray()
        self.inside = False
        self.esc = False

    def feed(self, data: bytes):
        frames = []
        for b in data:
            if b == FEND:
                if self.inside and self.buf:
                    frames.append((self.buf[0], bytes(self.buf[1:])))
                self.buf = bytearray()
                self.inside = True
                self.esc = False
            elif not self.inside:
                continue
            elif self.esc:
                self.buf.append(FEND if b == TFEND else FESC if b == TFESC else b)
                self.esc = False
            elif b == FESC:
                self.esc = True
            else:
                self.buf.append(b)
        return frames


def passcode(call: str) -> int:
    """The APRS-IS passcode of a callsign (base call, upper case): the published 15-bit hash."""
    base = call.upper().split("-")[0]
    h = 0x73E2
    for i in range(0, len(base), 2):
        h ^= ord(base[i]) << 8
        if i + 1 < len(base):
            h ^= ord(base[i + 1])
    return h & 0x7FFF


def nmea_ok(line: str) -> bool:
    if not line.startswith("$") or "*" not in line:
        return False
    body, _, cs = line[1:].partition("*")
    c = 0
    for ch in body.encode("ascii", "replace"):
        c ^= ch
    try:
        return int(cs[:2], 16) == c
    except ValueError:
        return False


class Radio:
    """The radio's host port: a USB serial port, or udp:<ip>[:3334] for a radio in USB network mode."""

    def __init__(self, port: str):
        self.udp = port.startswith("udp:")
        if self.udp:
            host, _, p = port[4:].partition(":")
            self.addr = (host, int(p or 3334))
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.setblocking(False)
            self.fileobj = self.sock
        else:
            try:
                import serial
            except ImportError:
                sys.exit("needs pyserial for a serial port: pip install pyserial")
            self.ser = serial.Serial(port, 115200, timeout=0)
            self.fileobj = self.ser

    def read(self) -> bytes:
        if self.udp:
            try:
                data, _ = self.sock.recvfrom(2048)
                return data
            except BlockingIOError:
                return b""
        return self.ser.read(4096)

    def write(self, data: bytes):
        if self.udp:
            self.sock.sendto(data, self.addr)
        else:
            self.ser.write(data)

    def keepalive(self):
        # An empty KISS frame keeps the radio's host link in its TNC mode (it ends after 30 s without frames)
        self.write(bytes([FEND, FEND]))


class AprsIs:
    def __init__(self, server: str, call: str, code: int, filt: str):
        host, _, p = server.partition(":")
        self.target = (host, int(p or 14580))
        self.call, self.code, self.filt = call, code, filt
        self.sock = None
        self.buf = b""
        self.next_try = 0.0

    def connect(self):
        if time.time() < self.next_try:
            return False
        try:
            s = socket.create_connection(self.target, timeout=10)
        except OSError as e:
            logging.warning("APRS-IS %s:%d: %s, retrying in 30 s", *self.target, e)
            self.next_try = time.time() + 30
            return False
        login = f"user {self.call} pass {self.code} vers {VERSION}"
        if self.filt:
            login += f" filter {self.filt}"
        s.sendall((login + "\r\n").encode("ascii"))
        s.setblocking(False)
        self.sock = s
        logging.info("APRS-IS connected to %s:%d as %s", *self.target, self.call)
        return True

    def lines(self):
        try:
            data = self.sock.recv(4096)
        except BlockingIOError:
            return []
        except OSError:
            data = b""
        if not data:
            logging.warning("APRS-IS connection closed")
            self.close()
            return []
        self.buf += data
        *done, self.buf = self.buf.split(b"\n")
        return [d.rstrip(b"\r") for d in done]

    def send(self, line: bytes):
        if self.sock is None:
            return
        try:
            self.sock.sendall(line + b"\r\n")
        except OSError:
            self.close()

    def close(self):
        if self.sock is not None:
            self.sock.close()
        self.sock = None
        self.next_try = time.time() + 30


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("ODR_PORT"), help="radio serial port, or udp:<radio ip>[:port]")
    ap.add_argument("--call", help="callsign for APRS-IS (the radio's own, with the SSID it uses)")
    ap.add_argument("--passcode", type=int, help="APRS-IS passcode (default: computed from --call)")
    ap.add_argument("--server", default="rotate.aprs2.net:14580")
    ap.add_argument("--filter", default="", help='APRS-IS filter, e.g. "m/50" (messages need none: the server '
                    "sends messages for stations the connection gated)")
    ap.add_argument("--no-is", action="store_true", help="no APRS-IS connection (KISS TCP / NMEA only)")
    ap.add_argument("--kiss-tcp", type=int, default=0, help="listen for KISS TCP clients on this port (e.g. 8001)")
    ap.add_argument("--nmea", help="a serial GPS whose NMEA goes to the radio")
    ap.add_argument("--nmea-file", help="replay an NMEA file to the radio, one sentence per second")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(asctime)s %(message)s")
    if not a.port:
        ap.error("--port (or $ODR_PORT) is required")

    radio = Radio(a.port)
    sel = selectors.DefaultSelector()
    sel.register(radio.fileobj, selectors.EVENT_READ, "radio")
    dec = KissDecoder()

    ais = None
    if not a.no_is:
        call = (a.call or "N0CALL").upper()
        code = a.passcode if a.passcode is not None else (passcode(call) if a.call else -1)
        ais = AprsIs(a.server, call, code, a.filter)

    clients = {}
    if a.kiss_tcp:
        lst = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        lst.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        lst.bind(("127.0.0.1", a.kiss_tcp))
        lst.listen(4)
        lst.setblocking(False)
        sel.register(lst, selectors.EVENT_READ, "listen")
        logging.info("KISS TCP on 127.0.0.1:%d", a.kiss_tcp)

    gps = None
    gps_buf = b""
    if a.nmea:
        import serial
        gps = serial.Serial(a.nmea, 4800, timeout=0)
        sel.register(gps, selectors.EVENT_READ, "gps")
    replay = open(a.nmea_file, "r", errors="replace") if a.nmea_file else None
    next_replay = 0.0
    next_keepalive = 0.0

    def to_radio_nmea(line: str):
        if nmea_ok(line) and line[3:6] in ("RMC", "GGA"):
            radio.write(kiss_encode(CMD_SETHW, bytes([TAG_NMEA]) + line.encode("ascii", "replace")))

    while True:
        if ais is not None and ais.sock is None and ais.connect():
            sel.register(ais.sock, selectors.EVENT_READ, "is")
        now = time.time()
        if now >= next_keepalive:
            radio.keepalive()
            next_keepalive = now + 10
        if replay is not None and now >= next_replay:
            line = replay.readline()
            if not line:
                replay.seek(0)
            else:
                to_radio_nmea(line.strip())
            next_replay = now + 1.0
        for key, _ in sel.select(timeout=0.2):
            what = key.data
            if what == "radio":
                for typ, payload in dec.feed(radio.read()):
                    if typ & 0x0F == CMD_SETHW and payload[:1] == bytes([TAG_IS]):
                        logging.info("radio -> IS: %s", payload[1:].decode("latin-1"))
                        if ais is not None:
                            ais.send(payload[1:])
                    elif typ & 0x0F == CMD_DATA:
                        logging.debug("radio heard %d bytes", len(payload))
                        frame = kiss_encode(CMD_DATA, payload)
                        for c in list(clients):
                            try:
                                c.sendall(frame)
                            except OSError:
                                sel.unregister(c)
                                clients.pop(c, None)
                                c.close()
            elif what == "is":
                if ais.sock is None:
                    sel.unregister(key.fileobj)
                    continue
                lines = ais.lines()
                if ais.sock is None:
                    sel.unregister(key.fileobj)
                for line in lines:
                    if not line or line.startswith(b"#"):
                        if line:
                            logging.debug("IS: %s", line.decode("latin-1"))
                        continue
                    radio.write(kiss_encode(CMD_SETHW, bytes([TAG_IS]) + line[:300]))
            elif what == "listen":
                c, peer = key.fileobj.accept()
                c.setblocking(False)
                clients[c] = KissDecoder()
                sel.register(c, selectors.EVENT_READ, "client")
                logging.info("KISS client %s:%d", *peer)
            elif what == "client":
                c = key.fileobj
                try:
                    data = c.recv(4096)
                except OSError:
                    data = b""
                if not data:
                    sel.unregister(c)
                    clients.pop(c, None)
                    c.close()
                    continue
                for typ, payload in clients[c].feed(data):
                    if typ & 0x0F == CMD_DATA and typ >> 4 == 0:
                        radio.write(kiss_encode(CMD_DATA, payload))
            elif what == "gps":
                gps_buf += gps.read(1024)
                *done, gps_buf = gps_buf.split(b"\n")
                for line in done:
                    to_radio_nmea(line.decode("ascii", "replace").strip())


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
