#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""k22stub: the host side of the Kinetis K22F flash stub. docs/spec/stub-k22f.md.

The stub (stub/k22f, built with `make -C stub/k22f`) is loaded by a running RadioFoundry built with CONFIG_STUB_ENTRY=y through
the debug link's STUB command, over the radio's USB serial port, and answers on the same port (or, if it had to
enumerate again, on a port with the same serial number). It runs the protocol engine the STM32F4 stub runs
(stub/common/stubeng.c), so the radio class, the install order and the checks are f4stub.py's, given this chip
(K22). stubtool.py hands the K22F radios to it:

    stubtool.py PORT --rfoundry-enter --write-app build/BOARD/rfoundry_BOARD.bin       install a build
    stubtool.py PORT --rfoundry-enter --write-app build/BOARD/rfoundry_BOARD_codec.bin a build merged with the codec
    stubtool.py PORT --rfoundry-enter --progmem flash.bin                         back up the whole program flash (512 KB)
    stubtool.py PORT --rfoundry-enter --verify-progmem flash.bin                  compare by CRC
    stubtool.py PORT --rfoundry-enter --q128 spi.bin                              back up the SPI flash
    stubtool.py PORT --rfoundry-enter --write-q128 spi.bin --q128-range 0x100000:  put RadioFoundry's part back (changed sectors)
    stubtool.py PORT --attach --board BOARD --boot reset                     restart into the firmware
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
K22STUB_SHA256 = "6b6f41415504bf15b3c6f964d0a504c069b96435a3a1ef5927e1ee72424839bf"
K22STUB_B64 = (
    "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"
    "39CmRE3s7z8BPP98mw=="
)
# --- K22STUB end

MAGIC = 0x4B325342                     # 'K2SB': ERASE, XERASE and RESET (stub/k22f/k22stub.h)
VERSION = 4                            # the shared engine's protocol (stub/f4stub.py CONFIG_VERSION)
REC_LEN, REC_LAYOUT = 24, 1            # the board record (stub/k22f/k22stub.h)
FLASH_BASE = 0x00000000
FLASH_KB = 512
SECTOR = 0x800                         # 2 KB erase sectors
PROG_UNIT = 8
APP_BASE = 0x00004000                  # the radio's loader is below; the application's vector table is here
RUN_ADDR = 0x1FFF0000                  # SRAM_L's base: the stub's link address (platform/k22f/stub_handoff.h)
SRAM_SIZE = 0x10000                    # the stub keeps to SRAM_L
ERASE_TIMEOUT = 1.0                    # 2 KB: 114 ms worst case in the data sheet, the stub gives up at 300 ms

# The radios: every board whose [flash.stub] profiles this stub (the registry, tools/boards.py Board.stub). A profile
# is what the firmware owns (the linker FLASH region), the codec area a merged image fills (memory.ld's codec slot; the
# image ends at "codec_image_end", after the descriptor, and the rest of the area counts erased), the end of the
# writable flash ("app_end": the board's protected last sector stays), and the SPI NOR on GPIO pins with its chip
# select's idle state and its write floor (below: the radio's own data). "id" is echoed by HELLO.
BOARDS = {b.id: {"id": b.stub["id"], "name": b.name, "names": tuple(b.debuglink_names), "owned": [b.app_region],
                 "codec": b.codec_slot, "codec_image_end": b.stub["codec_image_end"], "app_end": b.stub["app_end"],
                 "spi": b.stub["spi"], "spi_floor": b.stub["spi_floor"]}
          for b in stubtool.rfoundryboards.BOARDS.values() if b.stub and b.stub["tool"] == "k22stub"}
# The tool installs at one application base, which every profile must give
for _bid, _b in BOARDS.items():
    if _b["owned"][0][0] != APP_BASE:
        raise ImportError("k22stub: the %s's image starts at 0x%08X, the stub's tools know 0x%08X only"
                          % (_bid, _b["owned"][0][0], APP_BASE))


def pin_byte(name):
    """'PTE5' -> 0x85 (port * 32 + pin, platform/k22f/k22f_pin.h), None -> 0xFF"""
    if name is None:
        return 0xFF
    if len(name) < 4 or name[:2] != "PT" or not "A" <= name[2] <= "E" or not 0 <= int(name[3:]) <= 31:
        raise ValueError("pin %r" % name)
    return (ord(name[2]) - ord("A")) * 32 + int(name[3:])


TEST_BOARD = "k22test"


def test_board():
    """A test profile for the self-test, id 0xFE (tools/boards.py STUB_TEST_ID): a 512 KB part with the codec area at
    0x55D00-0x7B800, the last sector kept and an SPI NOR on GPIO pins with a 1 MB floor"""
    return {"id": 0xFE, "name": "K22 test board", "names": ("K22 test board",), "owned": [(APP_BASE, 0x55D00)],
            "codec": (0x55D00, 0x7B800), "codec_image_end": 0x7B780, "app_end": 0x7F800,
            "spi": {"sck": "PTE5", "mosi": "PTE4", "miso": "PTE6", "cs": "PTA19", "cs_released": True},
            "spi_floor": 0x100000}


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
    """stubtool.py's K22F branch: f4stub.run with this chip."""
    return f4stub.run(a, port, reopen, log=log, chip=K22)


def self_test():
    fails = []

    def check(c, what):
        if not c:
            fails.append(what)
            print("FAIL: " + what)

    quiet = lambda *a: None
    import tempfile
    tmpdir = tempfile.TemporaryDirectory(prefix="rfoundry-k22stub-")
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

    BOARDS[TEST_BOARD] = test_board()
    TB = TEST_BOARD
    rec = config_record(TB)
    check(rec.hex() == "fe0101010000400000" "07f80000100000" "85848613" "00000000",
          "test board record %s" % rec.hex())
    check(K22.parse_record(rec) == {"app_base": APP_BASE, "app_end": 0x7F800, "xfloor": 0x100000},
          "test board record decodes")
    check(board_for("K22 test board") == TB and board_for("XR-100") is None, "board names")

    # A plain build: sectors 8 (the commit sector, first) to 171; sector 171 also holds the codec's first bytes, which
    # are read back, kept and saved; the loader, the codec area and the last sector untouched
    f, fake, r = fresh()
    try:
        r.write_image(TB, [(APP_BASE, app(5000, 3))], backup_dir=tmp)
        check(False, "test board: no write before the board record")
    except f4stub.F4Error:
        check(fake.erased == [] and fake.configs == 0, "test board: refused by the tool before anything is sent")
    r.configure(TB)
    check(r.board == TB and r.record == rec and r.flash_kb == 512, "test board: configured, the echo checked")
    img = app(300000, 3)
    nw, nk = r.write_image(TB, [(APP_BASE, img)], backup_dir=tmp)
    fl = fake.flash
    check(fake.erased[0] == 8 and sorted(fake.erased) == list(range(8, 172)),
          "test board: commit sector 8 first, then 9-171 (%d erases, first %s)" % (len(fake.erased), fake.erased[:2]))
    check(bytes(fl[:APP_BASE]) == loader and bytes(fl[-SECTOR:]) == last, "test board: loader and last sector untouched")
    check(bytes(fl[0x55D00:-SECTOR]) == bytes(f[0x55D00:-SECTOR]), "test board: the codec area untouched")
    check(bytes(fl[APP_BASE:APP_BASE + len(img)]) == img, "test board: application in place")
    check(bytes(fl[APP_BASE + len(img):0x55D00]) == b"\xff" * (0x55D00 - APP_BASE - len(img)), "test board: old tail erased")
    saved = os.path.join(tmp, TB + "_sector171_0x00055800.bin")
    check(os.path.exists(saved) and open(saved, "rb").read() == bytes(f[0x55800:0x56000]),
          "test board: sector 171 (shared with the codec) saved first")
    fake.erased = []
    nw, nk = r.write_image(TB, [(APP_BASE, img)], backup_dir=tmp)
    check(nw == 0 and fake.erased == [], "test board: the same image again writes nothing (%d)" % nw)
    img2 = img[:100000] + app(1000, 9) + img[101000:]
    fake.erased = []
    nw, _ = r.write_image(TB, [(APP_BASE, img2)], backup_dir=tmp)
    check(sorted(fake.erased) == [8, 56, 57] and nw == 3, "test board: only the changed 2 KB sectors (plus 8) %s" % fake.erased)
    check(bytes(fake.flash[APP_BASE:APP_BASE + len(img2)]) == img2, "test board: changed image in place")

    # A build merged with the codec ends at 0x7B780 (the descriptor's end): split at 0x55D00, the codec area to 0x7B800
    merged = bytearray(b"\xff" * (0x7B780 - APP_BASE))
    merged[:len(img)] = img
    merged[0x55D00 - APP_BASE:0x55D00 - APP_BASE + 0x11F58] = app(0x11F58, 13)
    merged[0x7B700 - APP_BASE:] = app(0x80, 17)
    mp = os.path.join(tmp, "rfoundry_%s_codec.bin" % TB)
    open(mp, "wb").write(merged)
    segs = f4stub.load_segments(mp, TB, K22)
    check([(a, len(d)) for a, d in segs] == [(APP_BASE, len(img)), (0x55D00, 0x7B800 - 0x55D00)],
          "merged image split in two %s" % [(hex(a), len(d)) for a, d in segs])
    fake.erased = []
    r.write_image(TB, segs, backup_dir=tmp)
    check(max(fake.erased) == 246 and bytes(fake.flash[APP_BASE:0x7B780]) == bytes(merged),
          "merged image in place, sectors up to 246 (0x7B000)")
    check(bytes(fake.flash[0x7B780:0x7B800]) == b"\xff" * 0x80 and bytes(fake.flash[0x7B800:]) == bytes(f[0x7B800:]),
          "merged image: the rest of sector 246 erased, sectors 247 up untouched")
    short = os.path.join(tmp, "short_codec.bin")
    open(short, "wb").write(bytes(merged[:-0x800]))
    try:
        f4stub.load_segments(short, TB, K22)
        check(False, "an image ending inside the codec area refused")
    except f4stub.F4Error:
        pass
    for bad, what in (([(APP_BASE, b"\x00" * 64)], "no stack pointer"), ([(0x2000, app(64, 3))], "the loader"),
                      ([(APP_BASE, app(0x52000, 3))], "into the codec area")):
        fake.erased = []
        try:
            r.write_image(TB, bad, backup_dir=tmp)
            check(False, "refused: " + what)
        except f4stub.F4Error:
            check(fake.erased == [], "refused before erasing: " + what)

    # Interrupted after a few programs: the first unit at 0x4000 still erased (the loader finds no application)
    f, fake, r = fresh(seed=7, fail_after=10)
    r.configure(TB)
    try:
        r.write_image(TB, [(APP_BASE, img)], backup_dir=tmp)
        check(False, "interrupted write raises")
    except OSError:
        pass
    check(fake.w(APP_BASE) == 0xFFFFFFFF and bytes(fake.flash[:APP_BASE]) == loader,
          "interrupted: not committed, loader untouched")
    fake.fail_after = None
    r.write_image(TB, [(APP_BASE, img)], backup_dir=tmp)
    check(bytes(fake.flash[APP_BASE:APP_BASE + len(img)]) == img, "a second run completes the install")

    # The SPI flash: the first megabyte is the radio's; a copy that differs there is refused before anything is written
    spi = bytearray((i * 13 + (i >> 12)) & 0xFF for i in range(0x200000))
    f, fake, r = fresh(spi=spi)
    r.configure(TB)
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
    check((nw, nk) == (1, 255) and bytes(fake.spi[0x100000:]) == bytes(upper), "SPI: RadioFoundry's part, one sector (%d)" % nw)
    try:
        r.request(f4stub.OP_XERASE, (0x0F000).to_bytes(4, "big") + MAGIC.to_bytes(4, "big"), retries=0)
        check(False, "the stub refuses an erase below the floor")
    except stubtool.DumpError:
        pass

    # Entering from a running RadioFoundry: the debug link reports SRAM_L's base and the 8 KB staging buffer; the image checks
    # keep the STM32F4 stub and this one apart
    if K22STUB_B64:
        image = stub_image()
        top = f4stub.stub_stack_top(image)
        check(top == RUN_ADDR + SRAM_SIZE, "the embedded stub's entry code loads the stack top 0x%X" % (top or 0))
        rfoundry = stubtool.FakeRFoundry((RUN_ADDR, 8192))
        r = f4stub.F4Radio(rfoundry, log=quiet, chip=K22)
        r.attach = r.configure = lambda *a: None
        r.rfoundry_enter(image, RUN_ADDR, board=TB)
        check(rfoundry.entered and bytes(rfoundry.mem[:len(image)]) == image, "rfoundry_enter sends the embedded K22F stub")
        for what, img_, span in (("the STM32F4 stub", f4stub.stub_image(), (RUN_ADDR, 8192)),
                                 ("the stub on the STM32 span", image, (0x20000000, 8192))):
            rfoundry = stubtool.FakeRFoundry(span)
            r = f4stub.F4Radio(rfoundry, log=quiet, chip=K22)
            r.attach = r.configure = lambda *a: None
            try:
                r.rfoundry_enter(img_, RUN_ADDR, board=TB)
                check(False, "K22F rfoundry_enter refuses %s" % what)
            except stubtool.DumpError:
                check(not rfoundry.entered and rfoundry.total is None, "K22F rfoundry_enter refuses %s before BEGIN" % what)
        rfoundry = stubtool.FakeRFoundry((0x20000000, 8192))
        r = f4stub.F4Radio(rfoundry, log=quiet)
        r.attach = lambda: None
        try:
            r.rfoundry_enter(image, 0x20000000, board=next(iter(f4stub.BOARDS)))
            check(False, "the STM32F4 path refuses the K22F stub")
        except stubtool.DumpError:
            check(rfoundry.total is None, "the STM32F4 path refuses the K22F stub before BEGIN")
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
