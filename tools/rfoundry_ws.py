#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""rfoundry_ws: a client of the radio's /ws (docs/spec/tcp-services.md 5.1), Python's standard library only. Command
line tool and Python module (tools/rfoundry_gps.py and tests/hil use the WebControl class).

    rfoundry_ws.py --radio 12.0.39.20                         the hello and every message after it, as JSON lines
    rfoundry_ws.py --radio 12.0.39.20 "status?changes" gps    send these first (the radio's requests, in words)

The radio takes one /ws client: while this runs, the radio's own page and takRF get 503."""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time

OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0x1, 0x2, 0x8, 0x9, 0xA


class WebControlError(Exception):
    pass


class WebControl:
    """One /ws connection. hello: the radio's first message (proto, caps, control); recv() returns the next text
    message as a dict, None at the timeout; binary frames (screen, audio) are skipped, pings answered"""

    def __init__(self, radio, port=80, timeout=5.0):
        self.s = socket.create_connection((radio, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(f"GET /ws HTTP/1.1\r\nHost: {radio}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
        head = b""
        while b"\r\n\r\n" not in head:
            d = self.s.recv(1024)
            if not d:
                raise WebControlError("the radio closed the connection during the handshake")
            head += d
        head, _, self.buf = head.partition(b"\r\n\r\n")
        status = head.split(b"\r\n", 1)[0].decode("ascii", "replace")
        if " 101 " not in status:
            raise WebControlError(f"/ws: {status}" + (" (another client has it)" if " 503 " in status else ""))
        self.hello = self.recv(timeout)
        if self.hello is None or "hello" not in self.hello:
            raise WebControlError(f"no hello from the radio: {self.hello}")

    def send(self, text):
        """A text frame, masked as a client's must be (RFC 6455 5.3)"""
        data = text.encode()
        mask = os.urandom(4)
        hdr = bytes([0x80 | OP_TEXT, 0x80 | len(data)]) if len(data) < 126 else \
            bytes([0x80 | OP_TEXT, 0x80 | 126]) + struct.pack(">H", len(data))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _need(self, n, end):
        while len(self.buf) < n:
            self.s.settimeout(max(0.01, end - time.monotonic()))
            d = self.s.recv(4096)
            if not d:
                raise WebControlError("the radio closed /ws")
            self.buf += d

    def recv(self, timeout=1.0):
        end = time.monotonic() + timeout
        while True:
            try:
                self._need(2, end)
                op, n = self.buf[0] & 0x0F, self.buf[1] & 0x7F
                at = 2
                if n == 126:
                    self._need(4, end)
                    n, at = struct.unpack(">H", self.buf[2:4])[0], 4
                elif n == 127:
                    self._need(10, end)
                    n, at = struct.unpack(">Q", self.buf[2:10])[0], 10
                self._need(at + n, end)
            except socket.timeout:
                return None
            data, self.buf = self.buf[at:at + n], self.buf[at + n:]
            if op == OP_TEXT:
                return json.loads(data)
            if op == OP_PING:
                mask = os.urandom(4)
                self.s.sendall(bytes([0x80 | OP_PONG, 0x80 | len(data)]) + mask +
                               bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
            elif op == OP_CLOSE:
                raise WebControlError("the radio closed /ws")

    def close(self):
        try:
            self.s.sendall(bytes([0x80 | OP_CLOSE, 0x82]) + b"\0\0\0\0" + struct.pack(">H", 1000))
        except OSError:
            pass
        self.s.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--radio", required=True, help="the radio's address on the link")
    ap.add_argument("requests", nargs="*", help="sent first, in order")
    ap.add_argument("--seconds", type=float, help="stop after this long")
    a = ap.parse_args()
    try:
        with WebControl(a.radio) as w:
            print(json.dumps(w.hello), flush=True)
            for r in a.requests:
                w.send(r)
            end = time.monotonic() + a.seconds if a.seconds else None
            while end is None or time.monotonic() < end:
                m = w.recv(1.0 if end is None else max(0.05, min(1.0, end - time.monotonic())))
                if m is not None:
                    print(json.dumps(m), flush=True)
    except (WebControlError, OSError) as e:
        sys.exit(f"rfoundry_ws: {e}")
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
