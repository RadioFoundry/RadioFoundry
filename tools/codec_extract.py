#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Merge a radio's own stock AMBE+2 codec into an OpenDigitalRadio image, on your machine (DM-1701, MD-380).

Recipes from docs/spec/codec-dm1701.md ("Extraction recipe (V02.06 CSV)") and docs/spec/codec-md380.md ("Extraction
recipe"):
  1. check the stock file ("OutSecurityBin" container: Baofeng DM-1701 V02.06 CSV or TYT MD-380 D002.032) by SHA-256
     and its segment table; the file picks the radio;
  2. decrypt every codec window: plain = file[off] XOR key[(off - 0x100) mod 1024], with the radio's 1024-byte key
     (a key file checked by SHA-256 as tools/stm32_dfu.py does, or derived from the stock file and checked the same
     way);
  3. check each window's SHA-256 and the code's first word 0x4FF0E92D;
  4. write a new image: the built application from 0x0800C000, then the codec where its profile places it, 0xFF between
     (docs/spec/codec-placement.md): the tables at their stock address, the code moved as one block into sector 11 (the
     DM-1701's right after its tables, the MD-380's at the start of the sector), with the addresses the build's codec
     map lists rewritten, and the descriptor at 0x080FFF80 that tells the firmware where the code is.

The codec is DVSI's and the radio maker's, not ours: it is never stored in this repository or in a release. This tool
reads your stock file and your key file and writes the merged image to a file you choose (by default next to the
built image, in build/, which git ignores). Flash the result with tools/stm32_dfu.py.
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stm32_dfu  # noqa: E402  (key table and key file check, hardware-dm1701.md 15.3)

# The container, decryption, profiles and merge live in the odrflash package (one implementation for both tools)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "odrflash"))
from odrflash.stockfw import (APP_BASE, CODEC_SIG, DESC_ADDR, FLASH_END, HEADER_LEN, KEY_LEN, MAGIC,  # noqa: E402,F401
                              PROFILES, SYN_BL, SYN_SELF_REF, StockError as ExtractError, app_file_offset, build_synthetic_stock, decrypt,
                              derive_key, descriptor, extract, find_profile, layout, merge, parse_container, sha256,
                              windows)
from odrflash.codecmap import bl_target  # noqa: E402
from odrflash import boards  # noqa: E402  (the board registry: the boards this tool serves)
import zlib  # noqa: E402

DM1701_V0206 = "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"
MD380_D2032 = "e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43"
MD9600_V0609 = "79a561eda2f5843334ea5974f5de511b6e172a1643f9163806d54a94404f0f09"
MD9600_V1645 = "de368437de05307d3e217f049bb5e30b1a8ec201e50b8e81aa5c506089c53532"

EPILOG = """\
examples:
  %(prog)s --stock "DM-1701(CSV)-V02.06.bin"                       (the key is derived from the stock file)
  %(prog)s --stock "DM-1701(CSV)-V02.06.bin" --key ~/keys/dm1701.key
  %(prog)s --stock "MD-380-D2.32(AD).bin"                          (MD-380: image build/md380/odr_md380.bin)
  %(prog)s --stock "MD9600-CSV-V06.09.bin"                         (MD-9600: image build/md9600/odr_md9600.bin)
  %(prog)s --stock stock.bin --key dm1701.key --image build/dm1701/odr_dm1701.bin -o build/dm1701/odr_dm1701_codec.bin
  %(prog)s --self-test

then flash:  tools/stm32_dfu.py --model dm1701 --key ~/keys/dm1701.key build/dm1701/odr_dm1701_codec.bin

Supported stock files: Baofeng's DM-1701 V02.06 CSV firmware (sha256 0314efd4...), TYT's MD-380 "Firmware 2.32"
D002.032 (sha256 e1a1a11a...) and TYT's MD-9600 06.09 CSV (79a561ed...) and 16.45 CSV (de368437...). The key file is
the radio's 1024-byte firmware key (DM-1701 sha256 d983ce95..., MD-380 5254139c..., MD-9600 c3d665b2...); without --key
it is derived from the stock file and checked by that hash. Neither is in this
repository. For flashing release images, tools/odrflash does all of this in one step.
"""


def repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(here)


def check_output_path(path):
    """Refuse to write the merged image anywhere git would pick it up."""
    root = repo_root()
    full = os.path.abspath(path)
    if os.path.commonpath([full, root]) != root:
        return
    try:
        r = subprocess.run(["git", "-C", root, "check-ignore", "-q", full], capture_output=True)
        ignored = r.returncode == 0
    except OSError:
        rel = os.path.relpath(full, root).split(os.sep)
        ignored = rel[0] in ("build", "codec") or full.endswith(".bin")
    if not ignored:
        raise ExtractError("%s is inside the repository and not ignored by git: the merged image contains the "
                           "proprietary codec, write it under build/ or outside the tree" % path)


def self_test():
    import random
    rng = random.Random(1701)
    fails = []

    def check(cond, msg):
        print(("ok    " if cond else "FAIL  ") + msg)
        if not cond:
            fails.append(msg)

    def expect_error(fn, msg):
        try:
            fn()
            check(False, msg)
        except ExtractError:
            check(True, msg)

    # The real layouts with a random key and random windows (synthetic files: no codec, no key, no stock bytes)
    key = bytes(rng.randrange(256) for _ in range(KEY_LEN))
    stock, profiles, code, tables = build_synthetic_stock(key, rng, DM1701_V0206)
    segs = PROFILES[DM1701_V0206]["segments"]

    check(parse_container(stock) == segs, "container segment table parsed")
    check(app_file_offset(segs, 0x08074714) == 0xC5C14, "code window file offset 0xC5C14 (spec)")
    check(app_file_offset(segs, 0x080D19F0) == 0x122EF0, "table window file offset 0x122EF0 (spec)")
    check((0xC5C14 - HEADER_LEN) % KEY_LEN == 0x314 and (0x122EF0 - HEADER_LEN) % KEY_LEN == 0x1F0,
          "cipher phases 0x314 and 0x1F0 (spec)")
    p, wins = extract(stock, key, profiles)
    CODE, TABN = 0x080E3A74, 0x12082
    delta = CODE - 0x08074714
    check([(a, len(b)) for a, b in wins] == [(CODE, len(code)), (0x080D19F0, TABN), (DESC_ADDR, 0x80)],
          "windows placed: the code right after the codec's data, the tables cut there, then the descriptor")
    mc, mt = wins[0][1], wins[1][1]
    check(struct.unpack_from("<I", mc, SYN_SELF_REF)[0] == 0x08074714 + 0x201 + delta, "self reference moved")
    check(mc[:SYN_SELF_REF] + mc[SYN_SELF_REF + 4:] == code[:SYN_SELF_REF] + code[SYN_SELF_REF + 4:],
          "nothing else in the code changed")
    check(bl_target(mt, SYN_BL, 0x080D19F0 + SYN_BL) == 0x08074714 + 0x200 + delta, "island call re-encoded")
    check(mt[:SYN_BL] + mt[SYN_BL + 4:] == tables[:SYN_BL] + tables[SYN_BL + 4:TABN], "nothing else in the tables changed")
    d = wins[2][1]
    f = struct.unpack_from("<7I", d, 0)
    check(f == (0x43534544, 1, 0x0206, CODE, len(code), zlib.crc32(mc), 1), "descriptor header and code window")
    check(struct.unpack_from("<3I", d, 28) == (0x080D19F0, TABN, zlib.crc32(mt)), "descriptor table window")
    check(struct.unpack_from("<I", d, 124)[0] == zlib.crc32(d[:124]) & 0xFFFFFFFF, "descriptor CRC")

    image = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(rng.randrange(256) for _ in range(5000))
    merged = merge(image, p, wins)
    check(len(merged) == FLASH_END - APP_BASE, "merged image ends with the descriptor at the end of flash")
    check(merged[:len(image)] == image, "application unchanged")
    check(merged[len(image):0x080D19F0 - APP_BASE] == b"\xFF" * (0x080D19F0 - APP_BASE - len(image)),
          "0xFF up to the tables")
    check(merged[0x080D19F0 - APP_BASE:0x080D19F0 - APP_BASE + TABN] == mt, "tables at 0x080D19F0")
    check(merged[0x080D19F0 + TABN - APP_BASE:CODE - APP_BASE] == b"\xFF\xFF", "0xFF between tables and code")
    check(merged[CODE - APP_BASE:CODE - APP_BASE + len(code)] == mc, "code at 0x080E3A74")
    check(merged[DESC_ADDR - APP_BASE:] == d, "descriptor at 0x080FFF80")
    check(not stm32_dfu.check_image(merged, "dm1701"), "stm32_dfu accepts the merged image without warnings")
    big = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(0x080C0000 - APP_BASE - 8)
    check(len(merge(big, p, wins)) == FLASH_END - APP_BASE, "an application filling sectors 3-9 merges")

    bad_key = bytes([key[0] ^ 1]) + key[1:]
    expect_error(lambda: extract(stock, bad_key, profiles), "wrong key: window hash mismatch")
    expect_error(lambda: extract(stock[:-1] + b"\0", key, profiles), "other file: unsupported sha256")
    expect_error(lambda: extract(stock, key), "synthetic file is not the real stock file")
    expect_error(lambda: merge(b"\0" * (0x080D19F0 - APP_BASE + 1), p, wins), "image overlapping the codec refused")
    expect_error(lambda: merge(stock, p, wins), "official file as --image refused")
    expect_error(lambda: merge(big + b"\0" * 0x119F4, p, wins), "an application reaching the tables refused")
    expect_error(lambda: parse_container(b"x" * 300), "non-container refused")

    # MD-380 D002.032: single-segment container, code window in sectors 6 and 7, four small extra table windows; the
    # merge moves the code to the start of sector 11
    mstock, mprofiles, mcode, mtables = build_synthetic_stock(key, rng, MD380_D2032)
    mreal = PROFILES[MD380_D2032]
    check(parse_container(mstock) == [(0x0800C000, 0xF2C00)], "MD-380 single-segment container parsed")
    check(app_file_offset(mreal["segments"], 0x08050C44) == 0x44D44, "MD-380 code window file offset 0x44D44 (spec)")
    check(app_file_offset(mreal["segments"], 0x0808FC64) == 0x83D64, "MD-380 table window file offset 0x83D64 (spec)")
    mp, mwins = extract(mstock, key, mprofiles)
    MCODE = 0x080E0000
    check([a for a, _ in mwins] == [MCODE, 0x0808FC64, 0x080D0FC8, 0x080F9698, 0x080F9E74, 0x080FD490, DESC_ADDR],
          "MD-380: the code moved to sector 11, the five table windows at their stock addresses, the descriptor")
    mc = mwins[0][1]
    check(mc[SYN_SELF_REF:SYN_SELF_REF + 4] == struct.pack("<I", MCODE + 0x201) and
          mc[:SYN_SELF_REF] + mc[SYN_SELF_REF + 4:] == mcode[:SYN_SELF_REF] + mcode[SYN_SELF_REF + 4:],
          "MD-380: the code's self reference rewritten, nothing else")
    check(bl_target(mwins[1][1], SYN_BL, 0x0808FC64 + SYN_BL) == MCODE + 0x200,
          "MD-380: the island's call follows the code")
    check(all(sha256(d) == w[2] for (_, d), w in zip(mwins[2:6], mprofiles[sha256(mstock)]["extra"])),
          "MD-380: the small windows unchanged")
    md = mwins[6][1]
    check(struct.unpack_from("<7I", md, 0) == (0x43534544, 1, 0x2032, MCODE, len(mcode), zlib.crc32(mc), 5),
          "MD-380 descriptor header, code window and five table windows")
    check(struct.unpack_from("<3I", md, 28) == (0x0808FC64, len(mtables), zlib.crc32(mwins[1][1])),
          "MD-380 descriptor main table window")
    check(struct.unpack_from("<3I", md, 40) == (0x080D0FC8, 0x24, zlib.crc32(mwins[2][1])),
          "MD-380 descriptor first small window")
    mimage = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(rng.randrange(256) for _ in range(0x74000 - 8))
    mmerged = merge(mimage, mp, mwins)
    check(len(mmerged) == FLASH_END - APP_BASE, "MD-380 merged image ends with the descriptor at the end of flash")
    check(mmerged[:len(mimage)] == mimage, "MD-380: an application filling sectors 3-7 unchanged")
    check(all(mmerged[a - APP_BASE:a - APP_BASE + len(d)] == d for a, d in mwins), "MD-380 windows in place")
    check(mmerged[len(mimage):0x0808FC64 - APP_BASE] == b"\xFF" * (0x0808FC64 - APP_BASE - len(mimage)),
          "MD-380: 0xFF up to the tables")
    check(not stm32_dfu.check_image(mmerged, "md380"), "stm32_dfu accepts the MD-380 merged image without warnings")
    expect_error(lambda: merge(mimage + b"\0" * (0x0808FC64 - 0x08080000 + 1), mp, mwins),
                 "MD-380: application reaching the tables refused")
    expect_error(lambda: extract(mstock, bad_key, mprofiles), "MD-380 wrong key refused")
    check(find_profile(mstock, mprofiles)["board"] == "md380", "MD-380 profile found by SHA-256")

    # MD-9600 06.09 and 16.45 CSV: a two-segment container, the code in sectors 7 and 8 moved to the start of sector 11,
    # the table window at the build's own address in sector 9
    for sha, tab_at, tag in ((MD9600_V0609, 0x080AB13C, 0x0609), (MD9600_V1645, 0x080AC2BC, 0x1645)):
        gstock, gprofiles, gcode, gtables = build_synthetic_stock(key, rng, sha)
        greal = PROFILES[sha]
        check(parse_container(gstock) == greal["segments"], "MD-9600 %04X container parsed" % tag)
        gp, gwins = extract(gstock, key, gprofiles)
        check([a for a, _ in gwins] == [0x080E0000, tab_at, DESC_ADDR],
              "MD-9600 %04X: the code in sector 11, the tables at the build's address, the descriptor" % tag)
        check(gwins[0][1] != gcode and len(gwins[0][1]) == len(gcode), "MD-9600 %04X: the code moved" % tag)
        gd = gwins[2][1]
        check(struct.unpack_from("<7I", gd, 0) == (0x43534544, 1, tag, 0x080E0000, len(gcode), zlib.crc32(gwins[0][1]), 1),
              "MD-9600 %04X descriptor header and code window" % tag)
        gimage = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(rng.randrange(256) for _ in range(0x74000 - 8))
        gmerged = merge(gimage, gp, gwins)
        check(len(gmerged) == FLASH_END - APP_BASE, "MD-9600 %04X merged image ends with the descriptor" % tag)
        check(not stm32_dfu.check_image(gmerged, "md9600"), "stm32_dfu accepts the MD-9600 %04X merged image" % tag)
        expect_error(lambda: merge(gimage + b"\0" * (tab_at - 0x08080000 + 1), gp, gwins),
                     "MD-9600 %04X: application reaching the tables refused" % tag)
        expect_error(lambda: extract(gstock, bad_key, gprofiles), "MD-9600 %04X wrong key refused" % tag)

    # main() end to end on the synthetic file
    import contextlib
    import io
    import tempfile
    from unittest import mock
    with tempfile.TemporaryDirectory() as d:
        sp, ip, op = (os.path.join(d, n) for n in ("stock.bin", "odr_dm1701.bin", "out.bin"))
        for path, blob in ((sp, stock), (ip, image)):
            with open(path, "wb") as f:
                f.write(blob)

        def run(*extra):
            argv = ["codec_extract.py", "--stock", sp, "--image", ip, "--output", op, "--key", "unused"] + list(extra)
            err = io.StringIO()
            with mock.patch.object(sys, "argv", argv), mock.patch.object(sys.modules[__name__], "find_profile",
                                                                          lambda s: p), \
                    mock.patch.object(stm32_dfu, "load_key", lambda path, board: key), \
                    mock.patch.object(sys.modules[__name__], "extract", lambda s, k: (p, wins)), \
                    mock.patch.object(sys.modules[__name__], "check_output_path", lambda path: None), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                return main(), err.getvalue()

        rc, err = run()
        with open(op, "rb") as f:
            check(rc == 0 and f.read() == merged, "main() writes the merged image")

    expect_error(lambda: check_output_path(os.path.join(repo_root(), "tools", "x_codec.img")),
                 "output inside the tree and not ignored refused")
    try:
        check_output_path(os.path.join(repo_root(), "build", "dm1701", "odr_dm1701_codec.bin"))
        check(True, "output under build/ allowed")
    except ExtractError:
        check(False, "output under build/ allowed")
    check(sha256(key) not in [h for h, _ in stm32_dfu.KEYS.values()], "synthetic key is not a real key")

    print("%d failures" % len(fails))
    return 1 if fails else 0


def test_desc(path, board="dm1701"):
    """Write the codec sectors as the merge leaves them for a synthetic codec, for tests/host/codecDescTest.c (the
    firmware's descriptor checks and this writer must agree): the DM-1701's sectors 10 and 11 (0x080C0000..0x08100000),
    the MD-380's sectors 8-11 (0x08080000..0x08100000)"""
    import random
    rng = random.Random(1701)
    key = bytes(rng.randrange(256) for _ in range(KEY_LEN))
    stock, profiles, _, _ = build_synthetic_stock(key, rng, {"md380": MD380_D2032, "md9600": MD9600_V0609}.get(board, DM1701_V0206))
    p, wins = extract(stock, key, profiles)
    lo = 0x08080000 if board in ("md380", "md9600") else 0x080C0000
    out = bytearray(b"\xFF" * (FLASH_END - lo))
    for a, d in wins:
        out[a - lo:a - lo + len(d)] = d
    with open(path, "wb") as f:
        f.write(out)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog=EPILOG,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stock", help="the radio's stock firmware file (DM-1701 V02.06 CSV, MD-380 D002.032 or MD-9600 "
                    "06.09 / 16.45 CSV)")
    ap.add_argument("--key", help="the radio's firmware key file (1024 bytes); derived from the stock file when left out")
    ap.add_argument("--board", choices=sorted(b for b, t in boards.BOARDS.items() if t.codec_tool == "codec_extract"),
                    help="refuse a stock file for another radio (the stock file picks the board otherwise)")
    ap.add_argument("--image", help="built OpenDigitalRadio image (default: build/<board>/odr_<board>.bin)")
    ap.add_argument("-o", "--output", help="merged image to write (default: <image>_codec.bin next to the image)")
    ap.add_argument("--self-test", action="store_true", help="check the tool's logic on a synthetic file and key")
    ap.add_argument("--test-desc", metavar="FILE", help="write the codec sectors for a synthetic codec, as merged "
                    "(tests/host/codecDescTest.c), for --board")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if args.test_desc:
        if not args.board:
            ap.error("--test-desc needs --board")
        return test_desc(args.test_desc, args.board)
    if not args.stock:
        ap.error("--stock is required (or --self-test)")
    try:
        with open(os.path.expanduser(args.stock), "rb") as f:
            stock = f.read()
        prof = find_profile(stock)
        board = prof["board"]
        if args.board and args.board != board:
            raise ExtractError("%s is a %s stock file, not a %s one" % (args.stock, board, args.board))
        key = stm32_dfu.load_key(args.key, board) if args.key else derive_key(stock, prof["key_sha256"])
        image_path = args.image or os.path.join(repo_root(), "build", board, "odr_%s.bin" % board)
        out = args.output or os.path.splitext(image_path)[0] + "_codec.bin"
        with open(os.path.expanduser(image_path), "rb") as f:
            image = f.read()
        check_output_path(out)
        prof, wins = extract(stock, key)
        merged = merge(image, prof, wins)
        for w in stm32_dfu.check_image(merged, board):
            print("warning: %s" % w)
        with open(out, "wb") as f:
            f.write(merged)
    except (OSError, ExtractError, stm32_dfu.DfuError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    print("stock  %s (%s), %s codec: %s verified"
          % (prof["name"], sha256(stock)[:8], board,
             ", ".join("0x%08X+0x%X" % (a, n) for a, n, _ in layout(prof))))
    print("wrote  %s (%d bytes; contains the proprietary codec: keep it to yourself)" % (out, len(merged)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
