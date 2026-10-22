#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Quick check that a radio answers as an MMDVM modem on its USB serial port (core/hotspot/mmdvm.c).

    mmdvm_probe.py [--port /dev/cu.usbmodemXXXX] [--config CC] [--freq MHZ] [--status N] [--no-dtr]
                   [--order mmdvmhost|sdrbench] [--quiet-commands]

Sends GET_VERSION and GET_STATUS and prints the answers. The first MMDVM frame puts the radio in hotspot mode (it
takes the DMR modem from the call stack); it goes back to normal 20 s after the last frame. With --config it also
sends SET_CONFIG (simplex, DMR, the color code), with --freq SET_FREQ (RX = TX), then SET_MODE DMR, and checks each
is ACKed. --status N polls GET_STATUS N times, 250 ms apart, as MMDVMHost does; after a configuration every status
must still show the DMR state (the configuration stuck). Exit status 0 when every request was answered as expected.

--no-dtr opens the port with DTR cleared, as MMDVMHost on Windows does (docs/spec/mmdvm-protocol.md 5.1, test
T-USB-2): the radio must answer and keep its configuration all the same.

--order picks the start-up sequence (docs/spec/mmdvm-protocol.md 3.1, 3.2, tests T-1 and T-2):
  mmdvmhost  (default) GET_VERSION, then SET_FREQ, SET_CONFIG (state 00), SET_MODE DMR, as given
  sdrbench   GET_VERSION, SET_MODE DMR at once (before any configuration), then the tune: SET_FREQ, SET_CONFIG with
             state 02, SET_MODE DMR, each waited on for 1 s. Needs --freq; --config defaults to 1

--quiet-commands sends what the radio must take without a NAK (docs/testing/mmdvm-audit.md A-5, A-13, I-2):
DMR_SHORTLC, DMR_START, SERIAL and TRANSPARENT (no answer at all) and FM_PARAMS1 (ACK).

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
DMR_SHORTLC, DMR_START, FM_PARAMS1, SERIAL, TRANSPARENT = 0x1C, 0x1D, 0x60, 0x80, 0x90
ACK, NAK = 0x70, 0x7F
MODE_DMR = 2
MODES = {0: "idle", 1: "D-Star", 2: "DMR", 3: "YSF", 4: "P25", 5: "NXDN", 6: "POCSAG", 10: "FM", 99: "lockout"}


def frame(cmd, payload=b""):
    return bytes([START, len(payload) + 3, cmd]) + bytes(payload)


class Link:
    def __init__(self, port, dtr=True):
        # A device or a URL such as rfc2217://host:port (a radio on a network serial hub)
        self.s = serial.serial_for_url(port, 115200, timeout=0.05, do_not_open=True)
        self.s.dtr = dtr               # applied at open: False opens with DTR cleared, as Windows MMDVMHost does
        self.s.open()
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


def quiet(link, cmd, payload, name):
    """True if cmd got no answer within 200 ms (frames heard on air are skipped)."""
    kind, f = link.request(cmd, payload, timeout=0.2)
    print(f"{name}: {'no answer (ok)' if kind is None else kind + ' ' + f.hex()}")
    return kind is None


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
    ap.add_argument("--no-dtr", action="store_true", help="open the port with DTR cleared (Windows MMDVMHost)")
    ap.add_argument("--order", choices=("mmdvmhost", "sdrbench"), default="mmdvmhost")
    ap.add_argument("--quiet-commands", action="store_true", help="check the commands that must not get a NAK")
    a = ap.parse_args()
    if a.order == "sdrbench":
        if a.freq is None:
            ap.error("--order sdrbench needs --freq")
        if a.config is None:
            a.config = 1

    link = Link(a.port or find_port(), dtr=not a.no_dtr)
    print(f"port {link.s.port}, DTR {'cleared' if a.no_dtr else 'set'}, {a.order} order")
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

    def check(kind, f, what):
        print(f"{what}: {kind}" + (f" (reason {f[4]})" if kind == "nak" else ""))
        return kind == "ack"

    if a.order == "sdrbench":
        # SDR bench: SET_MODE DMR right after the version, before any SET_FREQ / SET_CONFIG; then per tune
        ok &= check(*link.request(SET_MODE, bytes([MODE_DMR])), "SET_MODE DMR (before configuration)")
        hz = int(round(a.freq * 1e6)).to_bytes(4, "little")
        ok &= check(*link.request(SET_FREQ, b"\x00" + hz + hz + b"\xff" + hz), f"SET_FREQ {a.freq} MHz")
        p = bytearray(23)
        p[0], p[1], p[2], p[3], p[6], p[8] = 0x80, 0x02, 10, MODE_DMR, a.config & 0x0F, 128
        ok &= check(*link.request(SET_CONFIG, bytes(p)), f"SET_CONFIG CC {a.config} state DMR")
        ok &= check(*link.request(SET_MODE, bytes([MODE_DMR])), "SET_MODE DMR")
    elif a.freq is not None:
        hz = int(round(a.freq * 1e6)).to_bytes(4, "little")
        kind, f = link.request(SET_FREQ, b"\x00" + hz + hz + b"\xff" + hz)
        print(f"SET_FREQ {a.freq} MHz: {kind}" + (f" (reason {f[4]})" if kind == "nak" else ""))
        ok &= kind == "ack"
    if a.config is not None and a.order == "mmdvmhost":
        p = bytearray(23)
        p[0], p[1], p[2], p[3], p[6], p[8] = 0x80, 0x02, 10, 0, a.config & 0x0F, 128
        kind, f = link.request(SET_CONFIG, bytes(p))
        print(f"SET_CONFIG CC {a.config}: {kind}" + (f" (reason {f[4]})" if kind == "nak" else ""))
        ok &= kind == "ack"
        kind, f = link.request(SET_MODE, bytes([MODE_DMR]))
        print(f"SET_MODE DMR: {kind}" + (f" (reason {f[4]})" if kind == "nak" else ""))
        ok &= kind == "ack"

    if a.quiet_commands:
        ok &= quiet(link, DMR_SHORTLC, bytes(9), "DMR_SHORTLC")
        ok &= quiet(link, DMR_START, b"\x01", "DMR_START")
        ok &= quiet(link, SERIAL, b"ABC", "SERIAL")
        ok &= quiet(link, TRANSPARENT, b"\x00\x00", "TRANSPARENT")
        ok &= check(*link.request(FM_PARAMS1, bytes(7)), "FM_PARAMS1")

    configured = a.config is not None
    for i in range(a.status):
        kind, f = link.request(GET_STATUS)
        if kind != "reply" or len(f) < 13:
            print("GET_STATUS: no answer" if kind is None else f"GET_STATUS: {kind} {f.hex()}")
            ok = False
        else:
            print("GET_STATUS:")
            show_status(f)
            if configured and f[4] != MODE_DMR:
                print("  the configuration didn't stick (state not DMR): the radio left hotspot mode?")
                ok = False
        if i + 1 < a.status:
            time.sleep(0.25)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
