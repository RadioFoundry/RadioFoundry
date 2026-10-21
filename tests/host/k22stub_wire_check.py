#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The host tool (stub/k22stub.py, with f4stub.py's radio class) against the K22F stub's real protocol engine
(k22stubSim, built from stub/common/stubeng.c and stub/k22f/k22stub_board.c): the GD-77's board record, installs of a
plain and a merged build on a simulated GD-77 flash, the refusals (the loader, the last sector, the SPI flash's first
megabyte, writes before the record), SPI flash work above the floor, all over the stub's wire protocol. Also checks the
GD-77 profile against the firmware's own sources (linker script, codec placement, flash window, pins, storage map), so
it can't drift from them. Run by `make -C tests/host`."""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "stub"))
sys.path.insert(0, HERE)
import f4stub  # noqa: E402
import k22stub  # noqa: E402
import stubtool  # noqa: E402
from f4stub_wire_check import PipePort  # noqa: E402

K22 = k22stub.K22
APP = k22stub.APP_BASE


def run(sim, flash, job, board="gd77"):
    with tempfile.TemporaryDirectory(prefix="odr-k22wire-") as d:
        fin, fout = os.path.join(d, "in.bin"), os.path.join(d, "out.bin")
        open(fin, "wb").write(flash)
        proc = subprocess.Popen([sim, fin, fout], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        radio = f4stub.F4Radio(PipePort(proc), log=lambda *a: None, chip=K22)
        radio.hello()
        if board:
            radio.configure(board)
        result = job(radio, d)
        proc.stdin.close()
        log = proc.stderr.read().decode()
        proc.wait(10)
        return open(fout, "rb").read(), open(fout + ".spi", "rb").read(), result, log


def erases(log):
    return [int(x.split()[1]) for x in log.splitlines() if x.startswith("erase")]


def main():
    sim = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "k22stubSim")
    fails = []

    def check(c, what):
        if not c:
            fails.append(what)
            print("FAIL: " + what)

    loader = bytes((i * 7 + 1) & 0xFF for i in range(APP))
    flash = bytearray((i * 5 + 3) & 0xFF for i in range(0x80000))
    flash[:APP] = loader
    flash[APP:APP + 4] = (0x2000FEF0).to_bytes(4, "little")
    app = bytearray((i * 3 + (i >> 9)) & 0xFF for i in range(200000))
    app[:4] = (0x2000FEF0).to_bytes(4, "little")

    out, _, res, log = run(sim, bytes(flash), lambda r, d: r.write_image("gd77", [(APP, bytes(app))], backup_dir=d))
    e = erases(log)
    check("config 1 85 84 86 13 1" in log, "gd77 record decoded by the stub: %r" % log.splitlines()[:1])
    check(res == (164, 0) and e[0] == 8 and sorted(e) == list(range(8, 172)), "gd77 install: sectors 8-171 %s" % (res,))
    check(out[:APP] == loader and out[0x7F800:] == bytes(flash[0x7F800:]), "the loader and the last sector untouched")
    check(out[0x55D00:] == bytes(flash[0x55D00:]), "the codec area untouched")
    check(out[APP:APP + len(app)] == bytes(app) and out[APP + len(app):0x55D00] == b"\xff" * (0x55D00 - APP - len(app)),
          "the image in place, the rest of the application area erased")

    merged = bytearray(b"\xff" * (0x7B780 - APP))
    merged[:len(app)] = app
    merged[0x55D00 - APP:0x7B780 - APP] = bytes((i * 11) & 0xFF for i in range(0x7B780 - 0x55D00))
    with tempfile.TemporaryDirectory() as d:
        mp = os.path.join(d, "odr_gd77_codec.bin")
        open(mp, "wb").write(merged)
        segs = f4stub.load_segments(mp, "gd77", K22)
    out, _, res, log = run(sim, bytes(flash), lambda r, d: r.write_image("gd77", segs, backup_dir=d))
    e = erases(log)
    check(sorted(e) == list(range(8, 247)) and out[APP:0x7B780] == bytes(merged), "merged image: sectors 8-246 %s" % (res,))
    check(out[0x7B780:0x7B800] == b"\xff" * 0x80 and out[0x7B800:] == bytes(flash[0x7B800:]),
          "merged image: sectors 247 up untouched")

    def raw(r, d, ops):
        got = []
        for op, payload in ops:
            try:
                r.request(op, payload, retries=0)
                got.append("ok")
            except stubtool.DumpError as ex:
                got.append(str(ex))
        return got

    m = k22stub.MAGIC.to_bytes(4, "big")
    refused = ((f4stub.OP_ERASE, bytes([7]) + m), (f4stub.OP_ERASE, bytes([255]) + m),
               (f4stub.OP_ERASE, bytes([20]) + m),                                  # the interlock
               (f4stub.OP_PROGRAM, (0x7F800).to_bytes(4, "big") + b"\x00" * 8),
               (f4stub.OP_PROGRAM, (0x3FF8).to_bytes(4, "big") + b"\x00" * 8),
               (f4stub.OP_XERASE, (0x0F000).to_bytes(4, "big") + m),
               (f4stub.OP_XPROGRAM, (0x0F000).to_bytes(4, "big") + b"\x00" * 16),
               (f4stub.OP_PEEK, (0x40072090).to_bytes(4, "big") + (1).to_bytes(2, "big")))
    out, spi_out, got, log = run(sim, bytes(flash), lambda r, d: raw(r, d, refused))
    check(all("status 0x0F" in g for g in got), "refusals: loader, last sector, interlock, SPI floor, USB peek %s" % got)
    check(out == bytes(flash) and "erase" not in log, "nothing changed by the refused requests")
    out, _, got, log = run(sim, bytes(flash), lambda r, d: raw(r, d, refused[:1]), board=None)
    check(got and "status 0x11" in got[0], "unconfigured: writes refused %s" % got)

    # The SPI flash: the radio's first megabyte refused by the tool, ODR's part written by changed sector
    spi = bytes((i * 13 + (i >> 12)) & 0xFF for i in range(0x200000))
    upper = bytearray(spi[0x100000:])
    upper[0x11000:0x11020] = b"\x00" * 32

    def spi_job(r, d):
        first = r.read_flash(0, 0x10000, f4stub.MEM_SPI)
        low = bytearray(spi[:0x10000])
        low[0] ^= 1
        try:
            r.spi_write(bytes(low), 0)
            refused_low = False
        except f4stub.F4Error:
            refused_low = True
        return first, refused_low, r.spi_write(bytes(upper), 0x100000)

    out, spi_out, (first, refused_low, res), log = run(sim, bytes(flash), spi_job)
    check(first == spi[:0x10000], "SPI read through the real engine")
    check(refused_low and res == (1, 255) and log.count("xerase") == 1, "SPI: first MB refused, one sector above %s" % (res,))
    check(spi_out[:0x100000] == spi[:0x100000] and spi_out[0x100000:] == bytes(upper), "SPI flash holds ODR's new part")

    # The profile against the firmware's sources
    root = os.path.join(HERE, "..", "..")
    prof = k22stub.BOARDS["gd77"]
    src = lambda *p: open(os.path.join(root, *p)).read()
    ld = src("boards", "gd77", "gd77.ld")
    m = re.search(r"FLASH\s*\(rx\)\s*:\s*ORIGIN\s*=\s*(0x[0-9A-Fa-f]+),\s*LENGTH\s*=\s*(0x[0-9A-Fa-f]+)\s*-\s*(0x[0-9A-Fa-f]+)", ld)
    check(m is not None and int(m.group(1), 16) == APP == int(m.group(3), 16) == prof["owned"][0][0]
          and int(m.group(2), 16) == prof["owned"][0][1] == prof["codec"][0], "the application area is gd77.ld's")
    m = re.search(r"#define CODEC_DESC_ADDR\s+(0x[0-9A-Fa-f]+)u", src("platform", "k22f", "codec_desc.h"))
    check(m is not None and int(m.group(1), 16) + 0x80 == prof["codec_image_end"], "a merged image ends after the descriptor")
    check(prof["codec"][1] % k22stub.SECTOR == 0 and prof["codec"][1] >= prof["codec_image_end"], "the codec area ends on a sector")
    m = re.search(r"#define K22F_FTFA_HI\s+(0x[0-9A-Fa-f]+)u", src("platform", "k22f", "ftfa_math.h"))
    check(m is not None and int(m.group(1), 16) == prof["app_end"], "the writable end is the platform's flash window")
    m = re.search(r"#define GD77_NOR_FACTORY_END\s+(0x[0-9A-Fa-f]+)u", src("boards", "gd77", "storage_gd77.h"))
    check(m is not None and int(m.group(1), 16) == prof["spi_floor"], "the SPI floor is the board's factory end")
    pins = src("boards", "gd77", "pins.h")
    for key, name in (("cs", "CS"), ("mosi", "MOSI"), ("sck", "SCK"), ("miso", "MISO")):
        m = re.search(r"#define PIN_NOR_%s\s+K22F_PIN\(K22F_PORT_([A-E]), (\d+)\)" % name, pins)
        check(m is not None and "PT%s%s" % m.groups() == prof["spi"][key], "the NOR %s pin is pins.h's" % name)
    check("released" in pins[pins.index("PIN_NOR_CS"):pins.index("PIN_NOR_MOSI")] and prof["spi"]["cs_released"],
          "the chip select idles released, as pins.h says")
    m = re.search(r'\.name\s*=\s*"([^"]+)"', src("boards", "gd77", "board.c"))
    check(m is not None and k22stub.board_for(m.group(1)) == "gd77", "VERSION's board name finds the profile")

    print("k22stub_wire_check %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                   "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
