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
K22STUB_SHA256 = "c0c010a87c4ad39e2436327736bc75626d6e28d614e7b594610ed9e923808ffc"
K22STUB_B64 = (
    "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"
    "UeDrURP7nBw9Qf8Lg+KIWw=="
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
