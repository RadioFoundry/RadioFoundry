#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The host tool (stub/f4stub.py) against the stub's real protocol engine (f4stubSim, built from
stub/stm32f4/f4stub_proto.c): installs on a simulated DM-1701 and MD-380 flash, a read-back and the refusals, all
over the stub's wire protocol. Run by `make -C tests/host`."""
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


def run(sim, flash, job, want_spi=False):
    with tempfile.TemporaryDirectory(prefix="odr-f4wire-") as d:
        fin, fout = os.path.join(d, "in.bin"), os.path.join(d, "out.bin")
        open(fin, "wb").write(flash)
        proc = subprocess.Popen([sim, fin, fout], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        radio = f4stub.F4Radio(PipePort(proc), log=lambda *a: None)
        radio.hello()
        result = job(radio, d)
        proc.stdin.close()
        log = proc.stderr.read().decode()
        proc.wait(10)
        out = open(fout, "rb").read()
        if want_spi:
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
    ro = bytes((i * 11) & 0xFF for i in range(40000))

    out, res, log = run(sim, bytes(flash),
                        lambda r, d: r.write_image("dm1701", [(0x0800C000, bytes(app)), (0x080A0000, ro)], backup_dir=d))
    erases = [int(x.split()[1]) for x in log.splitlines() if x.startswith("erase")]
    check(res == (5, 0), "dm1701 install through the real engine %s" % (res,))
    check(erases[0] == 3 and sorted(erases) == [3, 4, 5, 6, 9], "dm1701 erases %s" % erases)
    check(out[:0xC000] == boot, "dm1701 bootloader untouched")
    check(out[0x60000:0xA0000] == bytes(flash[0x60000:0xA0000]) and out[0xC0000:] == bytes(flash[0xC0000:]),
          "dm1701 codec sectors untouched")
    check(out[0xC000:0xC000 + len(app)] == bytes(app) and out[0xA0000:0xA0000 + len(ro)] == ro, "dm1701 image in place")

    md_app = bytearray((i * 13 + 1) & 0xFF for i in range(0x08050C44 - 0x0800C000 - 4))
    md_app[:4] = (0x2001FFFC).to_bytes(4, "little")
    out, res, log = run(sim, bytes(flash), lambda r, d: r.write_image("md380", [(0x0800C000, bytes(md_app))], backup_dir=d))
    check(res[0] == 4, "md380 install %s" % (res,))
    check(out[0x50C44:] == bytes(flash[0x50C44:]), "md380 codec bytes kept in sector 6 and after")
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

    print("f4stub_wire_check %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                   "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
