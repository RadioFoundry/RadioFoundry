#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""A small TFTP client (RFC 1350, octet mode, the blksize / tsize options) for the radio's TFTP server in USB network
mode (docs/spec/tftp.md). Any TFTP client works; this one is for tests and scripts that want the error text.

  tftp_client.py 12.47.82.51 get codeplug.bin backup.bin
  tftp_client.py 12.47.82.51 put edited.bin codeplug.bin
  tftp_client.py 12.47.82.51 get info.txt -
"""
import argparse
import socket
import struct
import sys


class TftpError(Exception):
    def __init__(self, code, message):
        super().__init__(f"TFTP error {code}: {message}")
        self.code = code
        self.message = message


def _request(op, name, blksize, tsize):
    b = struct.pack(">H", op) + name.encode() + b"\0octet\0"
    if blksize:
        b += b"blksize\0" + str(blksize).encode() + b"\0"
    if tsize is not None:
        b += b"tsize\0" + str(tsize).encode() + b"\0"
    return b


def _options(pkt):
    fields = pkt[2:].split(b"\0")
    return {fields[i].decode().lower(): fields[i + 1].decode() for i in range(0, len(fields) - 1, 2)}


def _error(pkt):
    code = struct.unpack(">H", pkt[2:4])[0]
    raise TftpError(code, pkt[4:].split(b"\0")[0].decode(errors="replace"))


def get(host, name, blksize=1428, timeout=1.0, retries=5, port=69):
    """The file's bytes"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        req = _request(1, name, blksize, 0)
        last, dest = req, (host, port)
        out = bytearray()
        size, expect, tid = 512, 1, None
        tries = 0
        s.sendto(last, dest)
        while True:
            try:
                pkt, addr = s.recvfrom(70000)
            except socket.timeout:
                tries += 1
                if tries > retries:
                    raise TftpError(-1, "timed out")
                s.sendto(last, dest)
                continue
            if tid is None:
                tid = addr
                dest = addr
            elif addr != tid:
                continue
            tries = 0
            op = struct.unpack(">H", pkt[:2])[0]
            if op == 5:
                _error(pkt)
            if op == 6:
                size = int(_options(pkt).get("blksize", 512))
                last = struct.pack(">HH", 4, 0)
                s.sendto(last, dest)
            elif op == 3:
                block = struct.unpack(">H", pkt[2:4])[0]
                if block == expect & 0xFFFF:
                    out += pkt[4:]
                    expect += 1
                last = struct.pack(">HH", 4, block)
                s.sendto(last, dest)
                if block == (expect - 1) & 0xFFFF and len(pkt) - 4 < size:
                    return bytes(out)
    finally:
        s.close()


def put(host, name, data, blksize=1428, timeout=1.0, retries=5, port=69, commit_timeout=30.0):
    """Send data; returns once the server ACKed the last block (for codeplug.bin: after it checked and stored it)"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        last, dest = _request(2, name, blksize, len(data)), (host, port)
        size, block, tid, tries = 512, 0, None, 0
        done = False
        s.sendto(last, dest)
        while True:
            try:
                pkt, addr = s.recvfrom(2048)
            except socket.timeout:
                tries += 1
                # the last ACK waits for the commit (seconds): keep resending, but give it longer
                if tries > (int(commit_timeout / timeout) if done else retries):
                    raise TftpError(-1, "timed out")
                s.sendto(last, dest)
                continue
            if tid is None:
                tid = addr
                dest = addr
            elif addr != tid:
                continue
            tries = 0
            op = struct.unpack(">H", pkt[:2])[0]
            if op == 5:
                _error(pkt)
            if op == 6 and block == 0:
                size = int(_options(pkt).get("blksize", 512))
            elif not (op == 4 and struct.unpack(">H", pkt[2:4])[0] == block & 0xFFFF):
                continue
            if done:
                return
            chunk = data[block * size:(block + 1) * size]
            block += 1
            last = struct.pack(">HH", 3, block & 0xFFFF) + chunk
            s.sendto(last, dest)
            done = len(chunk) < size
    finally:
        s.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("op", choices=["get", "put"])
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--blksize", type=int, default=1428)
    a = ap.parse_args(argv)
    try:
        if a.op == "get":
            d = get(a.host, a.src, a.blksize)
            if a.dst == "-":
                sys.stdout.buffer.write(d)
            else:
                with open(a.dst, "wb") as f:
                    f.write(d)
                print(f"{len(d)} bytes")
        else:
            with open(a.src, "rb") as f:
                d = f.read()
            put(a.host, a.dst, d, a.blksize)
            print(f"{len(d)} bytes written")
    except TftpError as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
