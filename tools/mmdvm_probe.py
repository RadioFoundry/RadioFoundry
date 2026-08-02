#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Quick check that a radio answers as an MMDVM modem on its USB serial port (core/hotspot/mmdvm.c).

    mmdvm_probe.py [--port /dev/cu.usbmodemXXXX] [--config CC] [--freq MHZ] [--status N]

Sends GET_VERSION and GET_STATUS and prints the answers. The first MMDVM frame puts the radio in hotspot mode (it
takes the DMR modem from the call stack); it goes back to normal 20 s after the last frame. With --config it also
sends SET_CONFIG (simplex, DMR, the color code), with --freq SET_FREQ (RX = TX), then SET_MODE DMR, and checks each
is ACKed. --status N polls GET_STATUS N times, 250 ms apart, as MMDVMHost does. Exit status 0 when every request was
answered as expected.

Needs pyserial (pip install pyserial). Without --port it takes the only /dev/cu.usbmodem* (macOS) or
/dev/ttyACM* (Linux) there is.
"""
import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("needs pyserial: pip install pyserial")

START = 0xE0
GET_VERSION, GET_STATUS, SET_CONFIG, SET_MODE, SET_FREQ = 0x00, 0x01, 0x02, 0x03, 0x04
ACK, NAK = 0x70, 0x7F
MODE_DMR = 2
MODES = {0: "idle", 1: "D-Star", 2: "DMR", 3: "YSF", 4: "P25", 5: "NXDN", 6: "POCSAG", 10: "FM", 99: "lockout"}


def frame(cmd, payload=b""):
    return bytes([START, len(payload) + 3, cmd]) + bytes(payload)


class Link:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.05)
        self.s.dtr = True               # the radio treats the port as open only with DTR set
        self.buf = bytearray()

    def read_frame(self, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.buf += self.s.read(256)
            while self.buf and self.buf[0] != START:
                del self.buf[0]
            if len(self.buf) >= 2 and len(self.buf) >= self.buf[1] >= 3:
                n = self.buf[1]
                f = bytes(self.buf[:n])
                del self.buf[:n]
                return f
            if len(self.buf) >= 2 and self.buf[1] < 3:
                del self.buf[0]
        return None

    def request(self, cmd, payload=b"", timeout=1.0):
        """The reply to cmd: (kind, frame), kind 'reply', 'ack', 'nak' or None on timeout. Other frames
        (DMR data the radio hears) are printed and skipped."""
        self.s.write(frame(cmd, payload))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            f = self.read_frame(end - time.monotonic())
            if f is None:
                break
            if f[2] == cmd:
                return "reply", f
            if f[2] in (ACK, NAK) and len(f) > 3 and f[3] == cmd:
                return ("ack" if f[2] == ACK else "nak"), f
            print(f"  (frame 0x{f[2]:02X}: {f[3:].hex()})")
        return None, None


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if len(ports) != 1:
        sys.exit(f"give --port (found {ports or 'no USB serial ports'})")
    return ports[0]


def show_status(f):
    flags = f[5]
    names = [n for bit, n in ((0x01, "TX"), (0x04, "RX overflow"), (0x08, "TX overflow"), (0x40, "carrier"),
                              (0x80, "inbound busy")) if flags & bit]
    extra = f" + {len(f) - 13} extension bytes" if len(f) > 13 else ""
    print(f"  modes 0x{f[3]:02X}, state {MODES.get(f[4], f[4])}, flags 0x{flags:02X} {names}, "
          f"DMR space TS1 {f[7]} TS2 {f[8]}{extra}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--config", type=int, metavar="CC", help="send SET_CONFIG with this color code, then SET_MODE")
    ap.add_argument("--freq", type=float, metavar="MHZ", help="send SET_FREQ (RX = TX)")
    ap.add_argument("--status", type=int, default=1, metavar="N")
    a = ap.parse_args()

    link = Link(a.port or find_port())
    ok = True

    kind, f = None, None
    for _ in range(6):                  # as MMDVMHost: the radio may still be starting its USB side
        kind, f = link.request(GET_VERSION)
        if kind == "reply":
            break
    if kind != "reply" or len(f) < 4:
        sys.exit("no answer to GET_VERSION")
    text = f[4:].decode("ascii", "replace") if f[3] < 2 else f[23:].decode("ascii", "replace")
    print(f"GET_VERSION: protocol {f[3]}, \"{text}\"")

    if a.freq is not None:
        hz = int(round(a.freq * 1e6)).to_bytes(4, "little")
        kind, f = link.request(SET_FREQ, b"\x00" + hz + hz + b"\xff" + hz)
        print(f"SET_FREQ {a.freq} MHz: {kind}" + (f" (reason {f[4]})" if kind == "nak" else ""))
        ok &= kind == "ack"
    if a.config is not None:
        p = bytearray(23)
        p[0], p[1], p[2], p[3], p[6], p[8] = 0x80, 0x02, 10, 0, a.config & 0x0F, 128
        kind, f = link.request(SET_CONFIG, bytes(p))
        print(f"SET_CONFIG CC {a.config}: {kind}" + (f" (reason {f[4]})" if kind == "nak" else ""))
        ok &= kind == "ack"
        kind, f = link.request(SET_MODE, bytes([MODE_DMR]))
        print(f"SET_MODE DMR: {kind}" + (f" (reason {f[4]})" if kind == "nak" else ""))
        ok &= kind == "ack"

    for i in range(a.status):
        kind, f = link.request(GET_STATUS)
        if kind != "reply" or len(f) < 13:
            print("GET_STATUS: no answer" if kind is None else f"GET_STATUS: {kind} {f.hex()}")
            ok = False
        else:
            print("GET_STATUS:")
            show_status(f)
        if i + 1 < a.status:
            time.sleep(0.25)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
