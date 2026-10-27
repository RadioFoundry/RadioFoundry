#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The host tool (stub/f4stub.py) against the stub's real protocol engine (f4stubSim, built from stub/common/stubeng.c
and stub/stm32f4/f4stub_board.c): the board records, installs on a simulated DM-1701 and MD-380 flash and on f4stub's test
profile (other SPI wiring), a read-back and the refusals, the SPI flash's security registers (one locked), all over the
stub's wire protocol. The profiles come from the
board registry, whose check holds them to the boards' sources (tools/boards.py). Run by `make -C tests/host`."""
import os
import select
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "stub"))
import f4stub  # noqa: E402
import stubtool  # noqa: E402


class PipePort:
    """A serial-port look-alike over the simulator's stdin / stdout."""

    def __init__(self, proc):
        self.proc = proc
        self.timeout = 0.02
        self.fd = proc.stdout.fileno()

    def write(self, b):
        self.proc.stdin.write(b)
        self.proc.stdin.flush()

    def read(self, n):
        r, _, _ = select.select([self.fd], [], [], self.timeout)
        return os.read(self.fd, n) if r else b""

    def reset_input_buffer(self):
        while select.select([self.fd], [], [], 0)[0]:
            if not os.read(self.fd, 65536):
                break

    def flush(self):
        pass

    def close(self):
        pass


def run(sim, flash, job, want_spi=False, board="dm1701"):
    with tempfile.TemporaryDirectory(prefix="rfoundry-f4wire-") as d:
        fin, fout = os.path.join(d, "in.bin"), os.path.join(d, "out.bin")
        open(fin, "wb").write(flash)
        proc = subprocess.Popen([sim, fin, fout], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        radio = f4stub.F4Radio(PipePort(proc), log=lambda *a: None)
        # The simulator may still be starting on a loaded machine, and hello() allows 0.5 s once
        for attempt in range(20):
            try:
                radio.hello()
                break
            except stubtool.DumpError:
                if attempt == 19:
                    raise
        if board:
            radio.configure(board)
        result = job(radio, d)
        proc.stdin.close()
        log = proc.stderr.read().decode()
        proc.wait(10)
        out = open(fout, "rb").read()
        if want_spi == "sreg":
            out = (out, open(fout + ".sreg", "rb").read())
        elif want_spi:
            out = (out, open(fout + ".spi", "rb").read())
        return out, result, log


def main():
    sim = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "f4stubSim")
    fails = []

    def check(c, what):
        if not c:
            fails.append(what)
            print("FAIL: " + what)

    boot = bytes((i * 7 + 1) & 0xFF for i in range(0xC000))
    flash = bytearray(boot + bytes((i * 5 + 3) & 0xFF for i in range(0x100000 - 0xC000)))
    flash[0xC000:0xC004] = (0x2001FFFC).to_bytes(4, "little")
    app = bytearray((i * 3 + (i >> 9)) & 0xFF for i in range(250000))
    app[:4] = (0x2001FFFC).to_bytes(4, "little")
    codec = bytes((i * 11) & 0xFF for i in range(0x40000))

    out, res, log = run(sim, bytes(flash), lambda r, d: r.write_image("dm1701", [(0x0800C000, bytes(app))], backup_dir=d))
    erases = [int(x.split()[1]) for x in log.splitlines() if x.startswith("erase")]
    check(res == (7, 0), "dm1701 install through the real engine %s" % (res,))
    check(erases[0] == 3 and sorted(erases) == [3, 4, 5, 6, 7, 8, 9], "dm1701 erases %s" % erases)
    check(out[:0xC000] == boot, "dm1701 bootloader untouched")
    check(out[0xC0000:] == bytes(flash[0xC0000:]), "dm1701 codec sectors untouched")
    check(out[0xC000:0xC000 + len(app)] == bytes(app) and out[0xC000 + len(app):0xC0000].count(0xFF) == 0xC0000 - 0xC000 - len(app),
          "dm1701 image in place, the rest of sectors 3-9 erased")
    # a build merged with the codec: sectors 10 and 11 written too
    out, res, log = run(sim, bytes(flash), lambda r, d: r.write_image("dm1701", [(0x0800C000, bytes(app)),
                                                                                 (0x080C0000, codec)], backup_dir=d))
    erases = [int(x.split()[1]) for x in log.splitlines() if x.startswith("erase")]
    check(sorted(erases) == [3, 4, 5, 6, 7, 8, 9, 10, 11] and out[0xC0000:] == codec, "dm1701 merged image %s" % erases)

    md_app = bytearray((i * 13 + 1) & 0xFF for i in range(0x08080000 - 0x0800C000 - 4))
    md_app[:4] = (0x2001FFFC).to_bytes(4, "little")
    out, res, log = run(sim, bytes(flash), lambda r, d: r.write_image("md380", [(0x0800C000, bytes(md_app))], backup_dir=d),
                        board="md380")
    check(res[0] == 5, "md380 install: sectors 3-7 %s" % (res,))
    check(out[0x80000:] == bytes(flash[0x80000:]), "md380 codec sectors 8-11 untouched")
    check(out[0xC000:0xC000 + len(md_app)] == bytes(md_app), "md380 image in place")

    def reads(r, d):
        got = r.read_flash(0x08000000, 0x10000)
        peek = r.peek(0x08000010, 16)
        bad = []
        for op, payload in ((f4stub.OP_ERASE, bytes([1]) + f4stub.MAGIC.to_bytes(4, "big")),
                            (f4stub.OP_ERASE, bytes([7]) + f4stub.MAGIC.to_bytes(4, "big")),
                            (f4stub.OP_PROGRAM, (0x08000000).to_bytes(4, "big") + b"\x00" * 4),
                            (f4stub.OP_PEEK, (0x40023C00).to_bytes(4, "big") + (4).to_bytes(2, "big"))):
            try:
                r.request(op, payload, retries=0)
                bad.append(op)
            except stubtool.DumpError:
                pass
        return got, peek, bad

    out, (got, peek, bad), log = run(sim, bytes(flash), reads)
    check(got == bytes(flash[:0x10000]) and peek == bytes(flash[0x10:0x20]), "read-back and peek of the bootloader")
    check(bad == [], "refusals: bootloader erase, interlocked erase, bootloader program, peripheral peek (%s)" % bad)
    check(out == bytes(flash) and "erase" not in log, "nothing changed by the refused requests")

    # The SPI flash through the real engine: read it, write a changed copy (only the changed sectors), read it again
    spi = bytes((i * 13 + (i >> 12)) & 0xFF for i in range(0x100000))
    new = bytearray(spi)
    new[0x21000:0x21020] = b"\x00" * 32
    new[0xF0000:0xF1000] = b"\xff" * 4096

    def spi_job(r, d):
        first = r.read_flash(0, 0x40000, f4stub.MEM_SPI)
        res = r.spi_write(bytes(new), 0)
        return first, res, r.verify_file(bytes(new), 0, f4stub.MEM_SPI)

    out, (first, res, bad), log = run(sim, bytes(flash), spi_job, want_spi=True)
    out, spi_out = out
    check(first == spi[:0x40000], "SPI flash read through the real engine")
    check(res == (2, 254) and log.count("xerase") == 2, "SPI write: 2 sectors written %s" % (res,))
    check(spi_out == bytes(new) and bad == [], "SPI flash holds the new copy")
    check(out == bytes(flash), "internal flash untouched by SPI work")

    # The security registers through the real engine: read with the CRC, register 1 rewritten (only it), register 3
    # locked (LB3): the tool refuses it before sending, and the engine refuses it when asked anyway
    sregs = bytes((i * 29 + 7) & 0xFF for i in range(768))
    new = bytearray(sregs)
    new[5] ^= 0xFF

    def sreg_job(r, d):
        sr2, first = r.sreg_read_all()
        written = r.sreg_write(bytes(new), d)
        locked = bytearray(new)
        locked[600] ^= 0xFF
        try:
            r.sreg_write(bytes(locked), d)
            tool = "written"
        except f4stub.F4Error:
            tool = "refused"
        try:
            r.request(f4stub.OP_SWRITE, bytes([3]) + f4stub.MAGIC.to_bytes(4, "big") + bytes(256), retries=0)
            stub = "written"
        except stubtool.DumpError as e:
            stub = str(e)
        return sr2, first, written, tool, stub

    out, (sr2, first, written, tool, stub), log = run(sim, bytes(flash), sreg_job, want_spi="sreg")
    out, sreg_out = out
    check(sr2 == 0x22 and f4stub.F4Radio.sreg_locks(sr2) == [False, False, True], "status register 2: LB3 %02X" % sr2)
    check(first == sregs, "security registers read through the real engine")
    check(written == [1] and log.count("sreg ") == 1 and sreg_out == bytes(new), "only register 1 written %s" % written)
    check(tool == "refused" and "status 0x0F" in stub, "locked register 3: the tool and the engine refuse (%s)" % stub)

    # Other wiring (f4stub's test profile): its board record reaches the engine (SPI2, mode 3, chip select PB12), then
    # an install in sectors 3-7
    f4stub.BOARDS[f4stub.TEST_BOARD] = f4stub.test_board()
    out, res, log = run(sim, bytes(flash), lambda r, d: r.write_image(f4stub.TEST_BOARD, [(0x0800C000, bytes(md_app))],
                                                                      backup_dir=d), board=f4stub.TEST_BOARD)
    del f4stub.BOARDS[f4stub.TEST_BOARD]
    check("config 254 2 3 1C" in log, "test profile record decoded by the stub: %r" % log.splitlines()[:1])
    check(res[0] == 5 and out[0x80000:] == bytes(flash[0x80000:]) and out[:0xC000] == boot,
          "test profile install: sectors 3-7, bootloader and codec sectors untouched %s" % (res,))

    # Unconfigured: the engine refuses every write itself (the tool's own check bypassed)
    def raw_writes(r, d):
        got = []
        for op, payload in ((f4stub.OP_ERASE, bytes([5]) + f4stub.MAGIC.to_bytes(4, "big")),
                            (f4stub.OP_PROGRAM, (0x08060000).to_bytes(4, "big") + b"\x00" * 4),
                            (f4stub.OP_XERASE, (0x3000).to_bytes(4, "big") + f4stub.MAGIC.to_bytes(4, "big"))):
            try:
                r.request(op, payload, retries=0)
                got.append("ok")
            except stubtool.DumpError as e:
                got.append(str(e))
        return got, r.board, r.app_base

    out, (got, bd, base), log = run(sim, bytes(flash), raw_writes, board=None)
    check(len(got) == 3 and all("status 0x11" in g for g in got), "unconfigured: every write refused %s" % got)
    check(bd is None and base == 0 and out == bytes(flash), "unconfigured: no board, no app base, nothing changed")

    # The registry holds the profiles to the boards' sources (tools/boards.py check); here only the record's id
    for bid, prof in f4stub.BOARDS.items():
        check(f4stub.config_record(bid)[0] == prof["id"], "%s: record board id" % bid)
    check(len({b["id"] for b in f4stub.BOARDS.values()}) == len(f4stub.BOARDS), "board ids are unique")

    print("f4stub_wire_check %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                   "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
