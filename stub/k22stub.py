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
K22STUB_SHA256 = "6f590f05339ef80cbc4cb57e270a670a82b68d86bf85096587103acf8efa1e78"
K22STUB_B64 = (
    "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"
    "vf5v7XmDgw=="
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
