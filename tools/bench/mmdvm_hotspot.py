#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""
Adapted from our earlier bench tool hotspot_test.py.

Talk to the radio under test as an MMDVM modem (the public MMDVM serial protocol, as MMDVMHost drives a modem), for
bench tests of its hotspot and single frequency repeater (SFR) modes (CONFIG_HOTSPOT, CONFIG_SFR).

    mmdvm_hotspot.py [--port /dev/tty.usbmodemXXXX | --udp 192.168.x.y:3334 [--local :3335]]
                     [--freq 461.6875] [--cc 2] [--power 12] [--seconds 20] [--simplex | --sfr]
                     [--send [--send-at 3]] [--debug flags,framing[,reg10[,reg12[,rssiMin[,reg11[,flags2]]]]]]
                     [--json]

What it does: SET_FREQ, SET_CONFIG (simplex flag set with --simplex, the default; duplex with RX = TX with --sfr,
which is how our firmware's SFR is turned on: core/dmr/sfr.c sfrMmdvmTrigger), SET_MODE DMR; then GET_STATUS every
250 ms, logging every frame the radio sends up (DMR_DATA1/2, DMR_LOST1/2, NAK) and status changes, for --seconds.
--send sends a short network voice call after --send-at seconds (simplex: MS sync on DMR_DATA2; SFR: BS sync on the
outbound slot 2), paced at 60 ms; it needs RadioDesk (RADIODESK env, default ~/RadioDesk) for
burst building.

Transports: --port is a serial port (USB CDC ACM, 115200 8N1, the standard MMDVM link). --udp is MMDVMHost's UDP
modem transport (MMDVMHost [Modem] Protocol=udp, ModemAddress / ModemPort; also what rfgw's UdpPort speaks), for a
radio that offers MMDVM over a USB network interface; optional.

Our MMDVM extensions (OpenDigitalRadio, not part of MMDVM; only sent / parsed when asked):
  * vendor command 0xD5 (--debug): SFR experiment switches, payload flags, framing, reg10, reg12, rssiMin, reg11,
    flags2 (drivers/modem/hrc6000/sfr_glue/hrcSfr.h hrcSfrDebugSet; HRC_SFR_DEBUG_* and HRC_SFR_DEBUG2_* bits);
  * extra GET_STATUS bytes from frame offset 13 on: the SFR counters (hrcSfrDebug_t, same header). Printed as
    DEBUG lines when present (the status reply is longer than 13 bytes).
"""
import argparse
import json
import os
import socket
import sys
import threading
import time

FRAME_START = 0xE0
GET_VERSION, GET_STATUS, SET_CONFIG, SET_MODE, SET_FREQ = 0x00, 0x01, 0x02, 0x03, 0x04
DMR_DATA1, DMR_LOST1, DMR_DATA2, DMR_LOST2 = 0x18, 0x19, 0x1A, 0x1B
DMR_SHORTLC, DMR_START, DMR_ABORT = 0x1C, 0x1D, 0x1E
ACK, NAK = 0x70, 0x7F
ODR_SFR_DEBUG = 0xD5          # our extension
NAMES = {0x00: "VERSION", 0x01: "STATUS", 0x02: "SET_CONFIG", 0x03: "SET_MODE", 0x04: "SET_FREQ",
         0x18: "DMR_DATA1", 0x19: "DMR_LOST1", 0x1A: "DMR_DATA2", 0x1B: "DMR_LOST2", 0x1C: "DMR_SHORTLC",
         0x1D: "DMR_START", 0x1E: "DMR_ABORT", 0x70: "ACK", 0x7F: "NAK", 0xD5: "ODR_SFR_DEBUG"}
SYNC_DATA, SYNC_AUDIO = 0x40, 0x20

# hrcSfrDebug_t (drivers/modem/hrc6000/sfr_glue/hrcSfr.h), from GET_STATUS frame offset 13. (name, length, hex?)
SFR_STATUS = [
    ("txInterrupts", 1, 0), ("burstsWritten", 1, 0), ("rxBursts", 1, 0), ("rxRepeated", 1, 0),
    ("sendRejected", 1, 0), ("switchSkipped", 1, 0), ("sysAfterTsMs", 1, 0), ("txAfterTsMs", 1, 0),
    ("burstOnMs", 1, 0), ("tsPeriodMs", 1, 0), ("reg42Rx", 1, 1), ("state", 1, 1), ("flags", 1, 1),
    ("uplinkOverflow", 1, 0), ("reg42Tx", 1, 1), ("rxEcho", 1, 0), ("rxIgnored", 1, 0), ("rxDuplicate", 1, 0),
    ("lastRx", 1, 1), ("lastRxCC", 1, 1), ("txSwitch10us", 1, 0), ("rxSwitch10us", 1, 0), ("reg10", 1, 1),
    ("reg12", 1, 1), ("rxIgnoredCC", 1, 0), ("rxIgnoredSync", 1, 0), ("rxIgnoredNoCrc", 1, 0),
    ("rejectedType", 1, 1), ("rejected", 12, 1), ("rxWeak", 1, 0), ("lastRssi", 1, 1), ("rssiMin", 1, 1),
    ("rereadDiffers", 1, 0), ("pendingWas", 1, 1), ("pendingIs", 1, 1), ("rawKinds", 1, 1), ("rawArea", 1, 1),
    ("rawData", 1, 0), ("rawVoiceA", 1, 0), ("rawEmb", 1, 0), ("rawNone", 1, 0), ("rxEchoContent", 1, 0),
    ("raw", 33, 1), ("rxLcRebuilt", 1, 0), ("rxNoCall", 1, 0), ("spiFail", 1, 0), ("reg56", 1, 1),
    ("reg57", 1, 1), ("outAtReject", 1, 0), ("outAtRebuild", 1, 0), ("reg11", 1, 1), ("flags2", 1, 1),
    ("headerAsVoiceA", 1, 0), ("voiceAWas", 1, 1), ("voiceAIs", 1, 1), ("rxSameSlot", 1, 0),
    ("headerFilled", 1, 0), ("staleVoice", 1, 0), ("resyncs", 1, 0), ("outOfStepMs", 1, 0), ("relocks", 1, 0),
    ("lateEntryHeld", 1, 0), ("dataHeld", 1, 0), ("hangCut", 1, 0), ("rxDataEcho", 1, 0),
]
SFR_STATUS_OFFSET = 13


def frame(cmd: int, payload: bytes = b"") -> bytes:
    return bytes([FRAME_START, len(payload) + 3, cmd]) + payload


def parse_sfr_status(d: bytes):
    """Our SFR counters from a GET_STATUS reply (whole frame), or None when the radio sent none."""
    if len(d) <= SFR_STATUS_OFFSET:
        return None
    out, i = {}, SFR_STATUS_OFFSET
    for name, n, hx in SFR_STATUS:
        if i + n > len(d):
            break
        v = d[i:i + n]
        out[name] = v.hex() if n > 1 else (f"{v[0]:02X}" if hx else v[0])
        i += n
    return out


class Transport:
    """One MMDVM link: a serial port or MMDVMHost's UDP modem transport. recv() returns whole frames or None."""

    def __init__(self, port: str = None, udp: str = None, local: str = ":3335"):
        self.buf = bytearray()
        if udp:
            host, p = udp.rsplit(":", 1)
            lhost, lp = local.rsplit(":", 1)
            self.peer = (host, int(p))
            self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.s.bind((lhost, int(lp)))
            self.s.settimeout(0.05)
            self.ser = None
        else:
            import serial
            self.ser = serial.Serial(port, 115200, timeout=0.05)
            self.s = None

    def send(self, b: bytes) -> None:
        if self.s is not None:
            self.s.sendto(b, self.peer)
        else:
            self.ser.write(b)

    def recv(self):
        if self.s is not None:
            try:
                return self.s.recv(600)
            except socket.timeout:
                return None
        while True:
            # Serial: resynchronize on FRAME_START, then wait for the length byte's worth
            while self.buf and self.buf[0] != FRAME_START:
                del self.buf[0]
            if len(self.buf) >= 2 and len(self.buf) >= self.buf[1]:
                n = self.buf[1]
                out = bytes(self.buf[:n])
                del self.buf[:n]
                return out
            chunk = self.ser.read(max(1, self.ser.in_waiting))
            if not chunk:
                return None
            self.buf += chunk

    def close(self) -> None:
        if self.s is not None:
            self.s.close()
        else:
            self.ser.close()


def config_payload(cc: int, simplex: bool) -> bytes:
    cfg = bytearray(23)
    cfg[0], cfg[1], cfg[2], cfg[3] = (0x80 if simplex else 0x00), 0x02, 10, 2   # simplex/duplex, DMR, 100 ms, DMR
    cfg[4], cfg[5], cfg[6], cfg[7], cfg[8], cfg[10], cfg[13], cfg[14] = 128, 128, cc, 0, 128, 128, 128, 128
    return bytes(cfg)


def freq_payload(hz: int, power: int) -> bytes:
    return b"\x00" + hz.to_bytes(4, "little") * 2 + bytes([power]) + hz.to_bytes(4, "little")


class MmdvmLink:
    """The radio as MMDVMHost's modem: configured, polled every 250 ms, everything it sends recorded."""

    def __init__(self, transport: Transport, freq_hz: int, cc: int, power: int, simplex: bool = True,
                 debug: bytes = None, log=None):
        self.t = transport
        self.frames = []            # (t, cmd, bytes) of everything but ACKs and status
        self.naks = []
        self.space = [0, 0]
        self.flags = 0
        self.sfr = None
        self.status_raw = None
        self.lock = threading.Lock()
        self.stop = False
        self.log = log or (lambda *a: None)
        cmds = [(SET_FREQ, freq_payload(freq_hz, power)), (SET_CONFIG, config_payload(cc, simplex)),
                (SET_MODE, b"\x02")]
        if debug:
            cmds.append((ODR_SFR_DEBUG, debug))
        for cmd, p in cmds:
            self.t.send(frame(cmd, p))
            time.sleep(0.15)
        threading.Thread(target=self._loop, daemon=True).start()
        time.sleep(1.0)

    def send(self, b: bytes) -> None:
        self.t.send(b)

    def _loop(self):
        next_status = 0.0
        while not self.stop:
            now = time.monotonic()
            if now >= next_status:
                self.t.send(frame(GET_STATUS))
                next_status = now + 0.25
            d = self.t.recv()
            if not d or len(d) < 3:
                continue
            cmd = d[2]
            with self.lock:
                if cmd == GET_STATUS and len(d) >= 9:
                    self.flags, self.space = d[5], [d[7], d[8]]
                    if d[3:13] != self.status_raw:
                        self.status_raw = d[3:13]
                        self.log("STATUS", d[3:13].hex(), f"flags {d[5]:02X} space1 {d[7]} space2 {d[8]}")
                    sfr = parse_sfr_status(d)
                    if sfr is not None and sfr != self.sfr:
                        self.sfr = sfr
                        self.log("DEBUG", " ".join(f"{k}={v}" for k, v in sfr.items()))
                elif cmd == NAK:
                    self.naks.append((time.monotonic(), d[3] if len(d) > 3 else -1, d[4] if len(d) > 4 else 0))
                    self.log("NAK", d[3:].hex())
                elif cmd != ACK:
                    self.frames.append((time.monotonic(), cmd, d))
                    if cmd in (DMR_DATA1, DMR_DATA2):
                        self.log(NAMES[cmd], f"control {d[3]:02X}", "rssi", d[37:].hex())
                    else:
                        self.log(NAMES.get(cmd, hex(cmd)), d[3:].hex())

    def mark(self):
        with self.lock:
            return len(self.frames), len(self.naks)

    def since(self, mark):
        with self.lock:
            return self.frames[mark[0]:], self.naks[mark[1]:]

    def take_space(self, slot: int = 2) -> bool:
        with self.lock:
            if self.space[slot - 1] > 1:
                self.space[slot - 1] -= 1     # as MMDVMHost's CModem: one less per frame until the next status
                return True
            return False

    def transmitting(self) -> bool:
        with self.lock:
            return bool(self.flags & 0x01)


def radiodesk_path():
    rd = os.environ.get("RADIODESK", os.path.expanduser("~/RadioDesk"))
    for sub in ("rfgw", "server"):
        if os.path.join(rd, sub) not in sys.path:
            sys.path.insert(0, os.path.join(rd, sub))


def network_call(cc: int, sfr: bool):
    radiodesk_path()
    from radiodesk_rfgw import sim
    from radiodesk.dmr import pdu
    call = sim.voice_call(3101999, 215, cc, 2, family="bs" if sfr else "ms", slot=2, voice=[sim.AMBE_SILENCE] * 36)
    out = []
    for tb in call:
        c = SYNC_AUDIO if tb.dt == pdu.DT_VOICE_SYNC else (tb.n if tb.dt == pdu.DT_VOICE else SYNC_DATA | tb.dt)
        out.append(frame(DMR_DATA2, bytes([c]) + tb.burst))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="serial port of the radio's USB CDC MMDVM link")
    ap.add_argument("--udp", default=None, help="host:port of an MMDVM UDP modem (optional transport)")
    ap.add_argument("--local", default=":3335", help="local host:port for --udp")
    ap.add_argument("--freq", type=float, default=461.6875)
    ap.add_argument("--cc", type=int, default=2)
    ap.add_argument("--power", type=int, default=12, help="SET_FREQ RF level 0-255")
    ap.add_argument("--seconds", type=float, default=20)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--simplex", action="store_true", help="normal simplex hotspot (default)")
    mode.add_argument("--sfr", action="store_true", help="duplex with RX = TX: our SFR mode")
    ap.add_argument("--send", action="store_true")
    ap.add_argument("--send-at", type=float, default=3)
    ap.add_argument("--debug", default=None, help="our 0xD5 extension: hex bytes, e.g. 04,00")
    ap.add_argument("--json", action="store_true", help="log lines as JSON")
    args = ap.parse_args()
    if not (args.port or args.udp):
        ap.error("give --port (USB CDC serial) or --udp host:port")

    t0 = time.monotonic()

    def log(*a):
        if args.json:
            print(json.dumps({"t": round(time.monotonic() - t0, 3), "what": a[0], "text": " ".join(map(str, a[1:]))}),
                  flush=True)
        else:
            print(f"{time.monotonic() - t0:7.3f}", *a, flush=True)

    debug = bytes(int(x, 16) for x in args.debug.split(",")) if args.debug else None
    link = MmdvmLink(Transport(args.port, args.udp, args.local), int(round(args.freq * 1e6)), args.cc, args.power,
                     simplex=not args.sfr, debug=debug, log=log)
    calls = network_call(args.cc, args.sfr) if args.send else []
    sent = 0
    while time.monotonic() - t0 < args.seconds:
        now = time.monotonic() - t0
        if calls and sent < len(calls) and now >= args.send_at + max(0, sent - 4) * 0.06:
            link.send(calls[sent])
            sent += 1
        time.sleep(0.005)
    link.stop = True
    log("done, sent", sent, "network bursts")


if __name__ == "__main__":
    main()
