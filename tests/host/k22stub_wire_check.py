#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The host tool (stub/k22stub.py, with f4stub.py's radio class) against the K22F stub's real protocol engine
(k22stubSim, built from stub/common/stubeng.c and stub/k22f/k22stub_board.c): the test profile's board record, installs
of a plain and a merged build on a simulated flash of that layout, the refusals (the loader, the last sector, the SPI flash's first
megabyte, writes before the record), SPI flash work above the floor, all over the stub's wire protocol. Also checks
every K22F board's profile against the firmware's own sources (linker script, codec placement, flash window, pins, storage map), so
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
TB = k22stub.TEST_BOARD
k22stub.BOARDS[TB] = k22stub.test_board()


def run(sim, flash, job, board=TB):
    with tempfile.TemporaryDirectory(prefix="rfoundry-k22wire-") as d:
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

    out, _, res, log = run(sim, bytes(flash), lambda r, d: r.write_image(TB, [(APP, bytes(app))], backup_dir=d))
    e = erases(log)
    check("config 254 85 84 86 13 1" in log, "test record decoded by the stub: %r" % log.splitlines()[:1])
    check(res == (164, 0) and e[0] == 8 and sorted(e) == list(range(8, 172)), "install: sectors 8-171 %s" % (res,))
    check(out[:APP] == loader and out[0x7F800:] == bytes(flash[0x7F800:]), "the loader and the last sector untouched")
    check(out[0x55D00:] == bytes(flash[0x55D00:]), "the codec area untouched")
    check(out[APP:APP + len(app)] == bytes(app) and out[APP + len(app):0x55D00] == b"\xff" * (0x55D00 - APP - len(app)),
          "the image in place, the rest of the application area erased")

    merged = bytearray(b"\xff" * (0x7B780 - APP))
    merged[:len(app)] = app
    merged[0x55D00 - APP:0x7B780 - APP] = bytes((i * 11) & 0xFF for i in range(0x7B780 - 0x55D00))
    with tempfile.TemporaryDirectory() as d:
        mp = os.path.join(d, "rfoundry_%s_codec.bin" % TB)
        open(mp, "wb").write(merged)
        segs = f4stub.load_segments(mp, TB, K22)
    out, _, res, log = run(sim, bytes(flash), lambda r, d: r.write_image(TB, segs, backup_dir=d))
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

    # The SPI flash: the radio's first megabyte refused by the tool, RadioFoundry's part written by changed sector
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
    check(spi_out[:0x100000] == spi[:0x100000] and spi_out[0x100000:] == bytes(upper), "SPI flash holds RadioFoundry's new part")

    # Every K22F board's profile against the firmware's sources
    root = os.path.join(HERE, "..", "..")
    src = lambda *p: open(os.path.join(root, *p)).read()
    m = re.search(r"#define CODEC_DESC_ADDR\s+(0x[0-9A-Fa-f]+)u", src("platform", "k22f", "codec_desc.h"))
    desc_end = int(m.group(1), 16) + 0x80 if m else None
    m = re.search(r"#define K22F_FTFA_HI\s+(0x[0-9A-Fa-f]+)u", src("platform", "k22f", "ftfa_math.h"))
    ftfa_hi = int(m.group(1), 16) if m else None
    boards = [b for b in k22stub.BOARDS if b != TB]
    check(boards, "the registry has K22F stub boards")
    for bid in boards:
        prof = k22stub.BOARDS[bid]
        bdir = os.path.join(root, "boards", bid)
        headers = "".join(src("boards", bid, f) for f in sorted(os.listdir(bdir)) if f.endswith(".h"))
        ld = src("boards", bid, "memory.ld")
        m = re.search(r"FLASH\s*\(rx\)\s*:\s*ORIGIN\s*=\s*__app_start,\s*LENGTH\s*=\s*__codec_slot\s*-\s*__app_start", ld) \
            and re.search(r"__codec_slot\s*=\s*(0x[0-9A-Fa-f]+);", ld)
        base = re.search(r"^APP_BASE\s*:=\s*(0x[0-9A-Fa-f]+)", src("boards", bid, "board.mk"), re.M)
        check(m is not None and base is not None and int(base.group(1), 16) == APP == prof["owned"][0][0]
              and int(m.group(1), 16) == prof["owned"][0][1] == prof["codec"][0], "%s: the application area is memory.ld's" % bid)
        check(desc_end == prof["codec_image_end"], "%s: a merged image ends after the descriptor" % bid)
        check(prof["codec"][1] % k22stub.SECTOR == 0 and prof["codec"][1] >= prof["codec_image_end"],
              "%s: the codec area ends on a sector" % bid)
        check(ftfa_hi == prof["app_end"], "%s: the writable end is the platform's flash window" % bid)
        if prof["spi"]:
            m = re.search(r"#define \w+_NOR_FACTORY_END\s+(0x[0-9A-Fa-f]+)u", headers)
            check(m is not None and int(m.group(1), 16) == prof["spi_floor"], "%s: the SPI floor is the board's factory end" % bid)
            pins = src("boards", bid, "pins.h")
            for key, name in (("cs", "CS"), ("mosi", "MOSI"), ("sck", "SCK"), ("miso", "MISO")):
                m = re.search(r"#define PIN_NOR_%s\s+K22F_PIN\(K22F_PORT_([A-E]), (\d+)\)" % name, pins)
                check(m is not None and "PT%s%s" % m.groups() == prof["spi"][key], "%s: the NOR %s pin is pins.h's" % (bid, name))
            cs = pins[pins.index("PIN_NOR_CS"):pins.index("PIN_NOR_MOSI")]
            check(("released" in cs) == bool(prof["spi"]["cs_released"]), "%s: the chip select's idle state is pins.h's" % bid)
        m = re.search(r'\.name\s*=\s*"([^"]+)"', src("boards", bid, "board.c"))
        check(m is not None and k22stub.board_for(m.group(1)) == bid, "%s: VERSION's board name finds the profile" % bid)

    print("k22stub_wire_check %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                   "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
