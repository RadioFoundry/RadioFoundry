#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""A reference USRP voice-over-UDP peer for the radio (CONFIG_NET_USRP, docs/spec/usb-audio.md 4.2): what AllStarLink's
USRP channel driver or DVSwitch Analog_Bridge would do, for tests and scripts. The radio listens on UDP 34001 and
sends to UDP 32001 at the address that last sent it a frame (the host's address on the USB link until then).

  usrp_client.py 12.47.82.51 listen 10           print the radio's frames for 10 s: squelch (keyup), level
  usrp_client.py 12.47.82.51 listen 10 -o rx.raw  ... and save the audio (8 kHz, 16-bit little endian mono)
  usrp_client.py 12.47.82.51 tone 1000 3         transmit a 1 kHz tone for 3 s (the radio keys up)
  usrp_client.py 12.47.82.51 play tx.raw          transmit a raw file (8 kHz, 16-bit little endian mono)

The frame: a 32-byte header of big endian words ("USRP", sequence, memory, keyup, talkgroup, type, mpxid, reserved)
and 160 samples of 8 kHz 16-bit PCM, little endian (type 0), or 160 mu-law bytes (type 6). One frame every 20 ms.
"""
import argparse
import math
import socket
import struct
import sys
import time

RADIO_PORT = 34001      # the radio listens here
PEER_PORT = 32001       # and sends here
SAMPLES = 160           # 20 ms at 8 kHz
HEADER = struct.Struct(">4sIIIIIII")
TYPE_VOICE, TYPE_ULAW = 0, 6


def ulaw_decode(code):
    code = ~code & 0xFF
    seg = (code >> 4) & 7
    mag = ((((code & 0x0F) << 3) + 132) << seg) - 132
    return -mag if code & 0x80 else mag


def pack(seq, keyup, pcm=None):
    """One VOICE frame: pcm is SAMPLES ints (None: silence)"""
    body = struct.pack(f"<{SAMPLES}h", *(pcm if pcm is not None else [0] * SAMPLES))
    return HEADER.pack(b"USRP", seq & 0xFFFFFFFF, 0, 1 if keyup else 0, 0, TYPE_VOICE, 0, 0) + body


def parse(data):
    """(seq, keyup, type, samples or None); None if it isn't a USRP frame"""
    if len(data) < HEADER.size or data[:4] != b"USRP":
        return None
    _, seq, _, keyup, _, typ, _, _ = HEADER.unpack_from(data)
    body = data[HEADER.size:]
    if typ == TYPE_VOICE and len(body) == 2 * SAMPLES:
        return seq, bool(keyup), typ, list(struct.unpack(f"<{SAMPLES}h", body))
    if typ == TYPE_ULAW and len(body) == SAMPLES:
        return seq, bool(keyup), typ, [ulaw_decode(b) for b in body]
    return seq, bool(keyup), typ, None


def rms(pcm):
    return math.sqrt(sum(s * s for s in pcm) / len(pcm)) if pcm else 0.0


class Peer:
    """A UDP socket on PEER_PORT (or port) talking to the radio's RADIO_PORT"""

    def __init__(self, radio_ip, port=PEER_PORT):
        self.radio = (radio_ip, RADIO_PORT)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("", port))
        self.seq = 0

    def close(self):
        self.sock.close()

    def send(self, keyup, pcm=None):
        self.sock.sendto(pack(self.seq, keyup, pcm), self.radio)
        self.seq += 1

    def hello(self):
        """One keyup 0 frame: the radio answers this address from now on"""
        self.send(False)

    def transmit(self, frames, unkey=True):
        """Send each 160-sample frame of frames with keyup 1 every 20 ms, then (unkey) keyup 0. Frames received
        meanwhile are dropped"""
        t = time.monotonic()
        for pcm in frames:
            self.send(True, pcm)
            t += 0.020
            time.sleep(max(0.0, t - time.monotonic()))
        if unkey:
            self.send(False)

    def receive(self, seconds, on_frame=None):
        """Frames from the radio for seconds: a list of (time, seq, keyup, samples or None)"""
        out = []
        end = time.monotonic() + seconds
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return out
            self.sock.settimeout(left)
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                return out
            f = parse(data)
            if f is None:
                continue
            row = (time.monotonic(), f[0], f[1], f[3])
            out.append(row)
            if on_frame is not None:
                on_frame(row)


def tone_frames(hz, seconds, peak=13800):
    """A sine at peak (13800: about 60 % of the peak deviation on the DM-1701) in 160-sample frames"""
    n = int(seconds * 50)
    return [[int(peak * math.sin(2 * math.pi * hz * (k * SAMPLES + i) / 8000)) for i in range(SAMPLES)]
            for k in range(n)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("radio", help="the radio's address")
    ap.add_argument("--port", type=int, default=PEER_PORT, help="local port (default 32001)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("listen")
    p.add_argument("seconds", type=float)
    p.add_argument("-o", "--out", help="save the audio of keyup frames (raw, 8 kHz s16le)")
    p = sub.add_parser("tone")
    p.add_argument("hz", type=float)
    p.add_argument("seconds", type=float)
    p.add_argument("--peak", type=int, default=13800)
    p = sub.add_parser("play")
    p.add_argument("file")
    a = ap.parse_args()

    peer = Peer(a.radio, a.port)
    try:
        if a.cmd == "listen":
            peer.hello()
            out = open(a.out, "wb") if a.out else None
            t0 = time.monotonic()

            def show(row):
                t, seq, keyup, pcm = row
                print(f"{t - t0:7.3f} seq {seq:6d} keyup {int(keyup)} rms {rms(pcm or []):7.0f}")
                if out is not None and keyup and pcm:
                    out.write(struct.pack(f"<{SAMPLES}h", *pcm))
            rows = peer.receive(a.seconds, show)
            if out is not None:
                out.close()
            print(f"{len(rows)} frames, {sum(1 for r in rows if r[2])} with keyup")
        elif a.cmd == "tone":
            peer.transmit(tone_frames(a.hz, a.seconds, a.peak))
        else:
            raw = open(a.file, "rb").read()
            raw += b"\0" * (-len(raw) % (2 * SAMPLES))
            frames = [list(struct.unpack_from(f"<{SAMPLES}h", raw, i)) for i in range(0, len(raw), 2 * SAMPLES)]
            peer.transmit(frames)
    finally:
        peer.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
