#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""k22stub: the host side of the Kinetis K22F flash stub (GD-77). docs/spec/stub-k22f.md.

The stub (stub/k22f, built with `make -C stub/k22f`) is loaded by a running ODR built with CONFIG_STUB_ENTRY=y through
the debug link's STUB command, over the radio's USB serial port, and answers on the same port (or, if it had to
enumerate again, on a port with the same serial number). It runs the protocol engine the STM32F4 stub runs
(stub/common/stubeng.c), so the radio class, the install order and the checks are f4stub.py's, given this chip
(K22). stubtool.py hands the GD-77 to it:

    stubtool.py PORT --odr-enter --write-app build/gd77/odr_gd77.bin         install a build
    stubtool.py PORT --odr-enter --write-app build/gd77/odr_gd77_codec.bin   a build merged with the codec
    stubtool.py PORT --odr-enter --progmem flash.bin                         back up the whole program flash (512 KB)
    stubtool.py PORT --odr-enter --verify-progmem flash.bin                  compare by CRC
    stubtool.py PORT --odr-enter --q128 spi.bin                              back up the SPI flash
    stubtool.py PORT --odr-enter --write-q128 spi.bin --q128-range 0x100000:  put ODR's part back (changed sectors)
    stubtool.py PORT --attach --board gd77 --boot reset                      restart into the firmware
    k22stub.py --self-test                                                   against a simulated radio, no hardware
    k22stub.py --embed build/stub-k22f/k22stub.bin                           put a new stub build into this file
    k22stub.py --check-embedded build/stub-k22f/k22stub.bin                  exit 1 when the build differs from it

What differs from the STM32F4 stub: 2 KB erase sectors, 8-byte program units, the application from 0x4000 (the
radio's loader below is never written) up to the last sector (0x7F800, radio data, never written), and the SPI flash's
first megabyte, the radio's own data, never written (the board record's write floor; the tool refuses first). Nothing
here has run on a radio yet.
"""
import base64
import hashlib
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import f4stub  # noqa: E402  (the shared engine's radio class, install order and fake stub)
import stubtool  # noqa: E402

# --- K22STUB begin (k22stub.py --embed rewrites everything up to the end marker)
K22STUB_SHA256 = "661e15aefa3a25c2078e42d4c8ccde7738fbf673b5b3a6d9b9624a4202e7804f"
K22STUB_B64 = (
    "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"
)
# --- K22STUB end

MAGIC = 0x4B325342                     # 'K2SB': ERASE, XERASE and RESET (stub/k22f/k22stub.h)
VERSION = 3                            # the shared engine's protocol
REC_LEN, REC_LAYOUT = 24, 1            # the board record (stub/k22f/k22stub.h)
FLASH_BASE = 0x00000000
FLASH_KB = 512
SECTOR = 0x800                         # 2 KB erase sectors
PROG_UNIT = 8
APP_BASE = 0x00004000                  # the radio's loader is below; the application's vector table is here
RUN_ADDR = 0x1FFF0000                  # SRAM_L's base: the stub's link address (platform/k22f/stub_handoff.h)
SRAM_SIZE = 0x10000                    # the stub keeps to SRAM_L
ERASE_TIMEOUT = 1.0                    # 2 KB: 114 ms worst case in the data sheet, the stub gives up at 300 ms

# The GD-77 (docs/spec/hardware-gd77.md 6): what the firmware owns, the codec area a merged image fills (its last 128
# bytes at 0x7B700 are the descriptor and the image ends there, 0x7B780; the rest of the sector counts erased), the end
# of the writable flash (the last sector holds radio data), and the SPI NOR on GPIO pins: mode 0, the chip select
# PTA19 released to its pull-up when idle (it is also XTAL0), and its first megabyte the radio's own data
# (hardware-gd77.md 6.2). "id" is echoed by HELLO.
BOARDS = {
    "gd77": {"id": 1, "name": "Radioddity GD-77", "names": ("GD-77", "gd77"),
             "owned": [(0x00004000, 0x00055D00)], "codec": (0x00055D00, 0x0007B800), "codec_image_end": 0x0007B780,
             "app_end": 0x0007F800,
             "spi": {"sck": "PTE5", "mosi": "PTE4", "miso": "PTE6", "cs": "PTA19", "cs_released": True},
             "spi_floor": 0x100000},
}


def pin_byte(name):
    """'PTE5' -> 0x85 (port * 32 + pin, platform/k22f/k22f_pin.h), None -> 0xFF"""
    if name is None:
        return 0xFF
    if len(name) < 4 or name[:2] != "PT" or not "A" <= name[2] <= "E" or not 0 <= int(name[3:]) <= 31:
        raise ValueError("pin %r" % name)
    return (ord(name[2]) - ord("A")) * 32 + int(name[3:])


def config_record(board_id):
    """The 24-byte board record the stub's CONFIG takes (stub/k22f/k22stub.h)"""
    b = BOARDS[board_id]
    spi = b["spi"]
    rec = bytes([b["id"], REC_LAYOUT, 1 if spi else 0, 1 if spi and spi["cs_released"] else 0])
    rec += APP_BASE.to_bytes(4, "big") + b["app_end"].to_bytes(4, "big") + b["spi_floor"].to_bytes(4, "big")
    rec += bytes([pin_byte(spi and spi["sck"]), pin_byte(spi and spi["mosi"]), pin_byte(spi and spi["miso"]),
                  pin_byte(spi and spi["cs"])]) + bytes(4)
    assert len(rec) == REC_LEN
    return rec


def board_for(name):
    for bid, b in BOARDS.items():
        if name == bid or name in b["names"]:
            return bid
    return None


def stub_image():
    """The embedded stub build, checked against its SHA-256"""
    blob = zlib.decompress(base64.b64decode("".join(K22STUB_B64)))
    if hashlib.sha256(blob).hexdigest() != K22STUB_SHA256:
        raise f4stub.F4Error("the embedded K22F stub fails its own checksum")
    return blob


class K22Chip(f4stub.Chip):
    """The K22F stub for f4stub's radio class (f4stub.Chip)"""
    title = "K22F"
    name = b"K2STUB"
    magic = MAGIC
    flash_base = FLASH_BASE
    flash_kb = FLASH_KB
    prog_unit = PROG_UNIT
    app_base = APP_BASE
    run_addr = RUN_ADDR
    sram_size = SRAM_SIZE
    erase_timeout = ERASE_TIMEOUT
    min_version = VERSION

    @property
    def boards(self):
        return BOARDS

    def sectors(self, flash_kb):
        return [(i, FLASH_BASE + i * SECTOR, SECTOR) for i in range(flash_kb * 1024 // SECTOR)]

    def config_record(self, board_id):
        return config_record(board_id)

    def parse_record(self, rec):
        if len(rec) != REC_LEN or rec[0] == 0 or rec[1] != REC_LAYOUT or any(rec[20:24]):
            return None
        return {"app_base": int.from_bytes(rec[4:8], "big"), "app_end": int.from_bytes(rec[8:12], "big"),
                "xfloor": int.from_bytes(rec[12:16], "big")}

    def stub_image(self):
        return stub_image()

    def sp_ok(self, sp):
        return 0x1FFF0000 < sp <= 0x20010000 and sp % 4 == 0

    def board_for(self, name):
        return board_for(name)


K22 = K22Chip()


def run(a, port, reopen, log=print):
    """stubtool.py's GD-77 branch: f4stub.run with this chip."""
    return f4stub.run(a, port, reopen, log=log, chip=K22)


def self_test():
    fails = []

    def check(c, what):
        if not c:
            fails.append(what)
            print("FAIL: " + what)

    quiet = lambda *a: None
    import tempfile
    tmpdir = tempfile.TemporaryDirectory(prefix="odr-k22stub-")
    tmp = tmpdir.name
    loader = bytes((i * 7 + 1) & 0xFF for i in range(APP_BASE))
    last = bytes((i * 3 + 5) & 0xFF for i in range(SECTOR))

    def radio_flash(seed):
        f = bytearray((i * seed + 3) & 0xFF for i in range(FLASH_KB * 1024))
        f[:APP_BASE] = loader
        f[-SECTOR:] = last
        return f

    def app(n, seed):
        d = bytearray((i * seed + (i >> 9)) & 0xFF for i in range(n))
        d[:4] = (0x2000FEF0).to_bytes(4, "little")
        return bytes(d)

    def fresh(seed=5, **kw):
        f = radio_flash(seed)
        fake = f4stub.FakeF4(f, chip=K22, **kw)
        r = f4stub.F4Radio(fake, log=quiet, chip=K22)
        r.hello()
        return f, fake, r

    rec = config_record("gd77")
    check(rec.hex() == "010101010000400000" "07f80000100000" "85848613" "00000000",
          "gd77 board record %s" % rec.hex())
    check(K22.parse_record(rec) == {"app_base": APP_BASE, "app_end": 0x7F800, "xfloor": 0x100000},
          "gd77 board record decodes")
    check(board_for("GD-77") == "gd77" and board_for("DM-1701") is None, "board names")

    # A plain build: sectors 8 (the commit sector, first) to 171; sector 171 also holds the codec's first bytes, which
    # are read back, kept and saved; the loader, the codec area and the last sector untouched
    f, fake, r = fresh()
    try:
        r.write_image("gd77", [(APP_BASE, app(5000, 3))], backup_dir=tmp)
        check(False, "gd77: no write before the board record")
    except f4stub.F4Error:
        check(fake.erased == [] and fake.configs == 0, "gd77: refused by the tool before anything is sent")
    r.configure("gd77")
    check(r.board == "gd77" and r.record == rec and r.flash_kb == 512, "gd77: configured, the echo checked")
    img = app(300000, 3)
    nw, nk = r.write_image("gd77", [(APP_BASE, img)], backup_dir=tmp)
    fl = fake.flash
    check(fake.erased[0] == 8 and sorted(fake.erased) == list(range(8, 172)),
          "gd77: commit sector 8 first, then 9-171 (%d erases, first %s)" % (len(fake.erased), fake.erased[:2]))
    check(bytes(fl[:APP_BASE]) == loader and bytes(fl[-SECTOR:]) == last, "gd77: loader and last sector untouched")
    check(bytes(fl[0x55D00:-SECTOR]) == bytes(f[0x55D00:-SECTOR]), "gd77: the codec area untouched")
    check(bytes(fl[APP_BASE:APP_BASE + len(img)]) == img, "gd77: application in place")
    check(bytes(fl[APP_BASE + len(img):0x55D00]) == b"\xff" * (0x55D00 - APP_BASE - len(img)), "gd77: old tail erased")
    saved = os.path.join(tmp, "gd77_sector171_0x00055800.bin")
    check(os.path.exists(saved) and open(saved, "rb").read() == bytes(f[0x55800:0x56000]),
          "gd77: sector 171 (shared with the codec) saved first")
    fake.erased = []
    nw, nk = r.write_image("gd77", [(APP_BASE, img)], backup_dir=tmp)
    check(nw == 0 and fake.erased == [], "gd77: the same image again writes nothing (%d)" % nw)
    img2 = img[:100000] + app(1000, 9) + img[101000:]
    fake.erased = []
    nw, _ = r.write_image("gd77", [(APP_BASE, img2)], backup_dir=tmp)
    check(sorted(fake.erased) == [8, 56, 57] and nw == 3, "gd77: only the changed 2 KB sectors (plus 8) %s" % fake.erased)
    check(bytes(fake.flash[APP_BASE:APP_BASE + len(img2)]) == img2, "gd77: changed image in place")

    # A build merged with the codec ends at 0x7B780 (the descriptor's end): split at 0x55D00, the codec area to 0x7B800
    merged = bytearray(b"\xff" * (0x7B780 - APP_BASE))
    merged[:len(img)] = img
    merged[0x55D00 - APP_BASE:0x55D00 - APP_BASE + 0x11F58] = app(0x11F58, 13)
    merged[0x7B700 - APP_BASE:] = app(0x80, 17)
    mp = os.path.join(tmp, "odr_gd77_codec.bin")
    open(mp, "wb").write(merged)
    segs = f4stub.load_segments(mp, "gd77", K22)
    check([(a, len(d)) for a, d in segs] == [(APP_BASE, len(img)), (0x55D00, 0x7B800 - 0x55D00)],
          "merged image split in two %s" % [(hex(a), len(d)) for a, d in segs])
    fake.erased = []
    r.write_image("gd77", segs, backup_dir=tmp)
    check(max(fake.erased) == 246 and bytes(fake.flash[APP_BASE:0x7B780]) == bytes(merged),
          "merged image in place, sectors up to 246 (0x7B000)")
    check(bytes(fake.flash[0x7B780:0x7B800]) == b"\xff" * 0x80 and bytes(fake.flash[0x7B800:]) == bytes(f[0x7B800:]),
          "merged image: the rest of sector 246 erased, sectors 247 up untouched")
    short = os.path.join(tmp, "short_codec.bin")
    open(short, "wb").write(bytes(merged[:-0x800]))
    try:
        f4stub.load_segments(short, "gd77", K22)
        check(False, "an image ending inside the codec area refused")
    except f4stub.F4Error:
        pass
    for bad, what in (([(APP_BASE, b"\x00" * 64)], "no stack pointer"), ([(0x2000, app(64, 3))], "the loader"),
                      ([(APP_BASE, app(0x52000, 3))], "into the codec area")):
        fake.erased = []
        try:
            r.write_image("gd77", bad, backup_dir=tmp)
            check(False, "refused: " + what)
        except f4stub.F4Error:
            check(fake.erased == [], "refused before erasing: " + what)

    # Interrupted after a few programs: the first unit at 0x4000 still erased (the loader finds no application)
    f, fake, r = fresh(seed=7, fail_after=10)
    r.configure("gd77")
    try:
        r.write_image("gd77", [(APP_BASE, img)], backup_dir=tmp)
        check(False, "interrupted write raises")
    except OSError:
        pass
    check(fake.w(APP_BASE) == 0xFFFFFFFF and bytes(fake.flash[:APP_BASE]) == loader,
          "interrupted: not committed, loader untouched")
    fake.fail_after = None
    r.write_image("gd77", [(APP_BASE, img)], backup_dir=tmp)
    check(bytes(fake.flash[APP_BASE:APP_BASE + len(img)]) == img, "a second run completes the install")

    # The SPI flash: the first megabyte is the radio's; a copy that differs there is refused before anything is written
    spi = bytearray((i * 13 + (i >> 12)) & 0xFF for i in range(0x200000))
    f, fake, r = fresh(spi=spi)
    r.configure("gd77")
    path = os.path.join(tmp, "spi.bin")
    r.dump(path, 0, mem=f4stub.MEM_SPI)
    check(open(path, "rb").read() == bytes(spi), "SPI flash dump")
    new = bytearray(spi)
    new[0x3000:0x3010] = b"\x00" * 16
    fake.spi_erases = 0
    try:
        r.spi_write(bytes(new), 0)
        check(False, "SPI write below 1 MB refused")
    except f4stub.F4Error:
        check(fake.spi_erases == 0 and bytes(fake.spi) == bytes(spi), "SPI: nothing written below or above 1 MB")
    upper = bytearray(spi[0x100000:])
    upper[0x11000:0x11020] = b"\x00" * 32
    nw, nk = r.spi_write(bytes(upper), 0x100000)
    check((nw, nk) == (1, 255) and bytes(fake.spi[0x100000:]) == bytes(upper), "SPI: ODR's part, one sector (%d)" % nw)
    try:
        r.request(f4stub.OP_XERASE, (0x0F000).to_bytes(4, "big") + MAGIC.to_bytes(4, "big"), retries=0)
        check(False, "the stub refuses an erase below the floor")
    except stubtool.DumpError:
        pass

    # Entering from a running ODR: the debug link reports SRAM_L's base and the 8 KB staging buffer; the image checks
    # keep the STM32F4 stub and this one apart
    if K22STUB_B64:
        image = stub_image()
        top = f4stub.stub_stack_top(image)
        check(top == RUN_ADDR + SRAM_SIZE, "the embedded stub's entry code loads the stack top 0x%X" % (top or 0))
        odr = stubtool.FakeODR((RUN_ADDR, 8192))
        r = f4stub.F4Radio(odr, log=quiet, chip=K22)
        r.attach = r.configure = lambda *a: None
        r.odr_enter(image, RUN_ADDR, board="gd77")
        check(odr.entered and bytes(odr.mem[:len(image)]) == image, "odr_enter sends the embedded K22F stub")
        for what, img_, span in (("the STM32F4 stub", f4stub.stub_image(), (RUN_ADDR, 8192)),
                                 ("the stub on the STM32 span", image, (0x20000000, 8192))):
            odr = stubtool.FakeODR(span)
            r = f4stub.F4Radio(odr, log=quiet, chip=K22)
            r.attach = r.configure = lambda *a: None
            try:
                r.odr_enter(img_, RUN_ADDR, board="gd77")
                check(False, "K22F odr_enter refuses %s" % what)
            except stubtool.DumpError:
                check(not odr.entered and odr.total is None, "K22F odr_enter refuses %s before BEGIN" % what)
        odr = stubtool.FakeODR((0x20000000, 8192))
        r = f4stub.F4Radio(odr, log=quiet)
        r.attach = lambda: None
        try:
            r.odr_enter(image, 0x20000000, board="dm1701")
            check(False, "the STM32F4 path refuses the K22F stub")
        except stubtool.DumpError:
            check(odr.total is None, "the STM32F4 path refuses the K22F stub before BEGIN")
    else:
        check(False, "no K22F stub embedded (k22stub.py --embed)")

    tmpdir.cleanup()
    print("k22stub self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                   "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    if len(sys.argv) == 3 and sys.argv[1] == "--embed":
        f4stub.embed_into(sys.argv[2], os.path.abspath(__file__), "K22STUB")
        sys.exit(0)
    if len(sys.argv) == 3 and sys.argv[1] == "--check-embedded":
        same = open(sys.argv[2], "rb").read() == stub_image()
        print("the embedded K22F stub %s %s" % ("matches" if same else "differs from", sys.argv[2]))
        sys.exit(0 if same else 1)
    print(__doc__)
    sys.exit(2)
