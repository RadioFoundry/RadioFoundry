#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""odr_fault_trace: find a radio's fault trace on its host port (docs/spec/crash-log.md 2.3).

A radio that takes a CPU fault prints a few "#ODRFAULT1 ..." lines on its host port, one a second, then restarts
(core/crash/crash_trace.c). This reads the port raw for a while, picks those lines out of whatever else it carries,
checks their CRC and prints the registers.

    odr_fault_trace.py PORT [--seconds 10]     read the port (needs pyserial; the debug link must not hold it)
    odr_fault_trace.py --file LOG              decode lines from a saved log
    odr_fault_trace.py --selftest

PORT is a serial device or an rfc2217:// URL.
"""
import argparse
import re
import sys
import time

SYNC = b"#ODRFAULT1"
LINE_RE = re.compile(rb"(#ODRFAULT1 (?:[a-z0-9]+=[0-9a-f]+ )+)\*([0-9a-f]{4})")
# A line from tests/host/crashTest.c (the same bytes the firmware prints); the selftest decodes it
GOLDEN = (b"#ODRFAULT1 n=01 v=00000002 pc=0300c1a4 psr=80000140 lr=0300c180 sp=00012340 r0=00000000 r1=00000001 "
          b"r2=00000002 r3=00000003 *")


def crc16(data):
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def decode(buf):
    """Every framed line in buf: (fields dict, crc ok)"""
    out = []
    for m in LINE_RE.finditer(buf):
        body, crc = m.group(1), int(m.group(2), 16)
        fields = {}
        for kv in body[len(SYNC) + 1:].split():
            k, v = kv.split(b"=")
            fields[k.decode()] = int(v, 16)
        out.append((fields, crc16(body) == crc))
    return out


def show(fields, ok):
    regs = " ".join(f"{k}={v:08x}" for k, v in fields.items() if k not in ("n",))
    print(f"fault line {fields.get('n', 0)}{'' if ok else ' (BAD CRC)'}: {regs}")


def selftest():
    line = GOLDEN + b"%04x\r\n" % crc16(GOLDEN[:-1])
    got = decode(b"noise\x00\xff" + line + b"more")
    assert len(got) == 1 and got[0][1], got
    f = got[0][0]
    assert f["n"] == 1 and f["v"] == 2 and f["pc"] == 0x0300C1A4 and f["r3"] == 3, f
    bad = decode(line.replace(b"r3=00000003", b"r3=00000004"))
    assert len(bad) == 1 and not bad[0][1]
    print(f"selftest ok (golden line CRC {crc16(GOLDEN[:-1]):04x})")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--file")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        selftest()
        return 0
    if a.file:
        buf = open(a.file, "rb").read()
    elif a.port:
        import serial
        s = serial.serial_for_url(a.port, 115200, timeout=0.2)
        buf = b""
        end = time.time() + a.seconds
        while time.time() < end:
            buf += s.read(4096)
        s.close()
    else:
        ap.error("PORT, --file or --selftest")
    lines = decode(buf)
    for fields, ok in lines:
        show(fields, ok)
    if not lines:
        print(f"no fault trace in {len(buf)} bytes")
    return 0 if lines else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
