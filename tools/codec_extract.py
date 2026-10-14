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
  4. write a new image: the built application from 0x0800C000, then every window at its stock address, 0xFF between.
     Nothing is relocated: the codec runs in place at its stock addresses.

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
from odrflash.stockfw import (APP_BASE, CODEC_SIG, FLASH_END, HEADER_LEN, KEY_LEN, MAGIC, PROFILES,  # noqa: E402,F401
                              StockError as ExtractError, app_file_offset, build_synthetic_stock, decrypt, derive_key,
                              extract, find_profile, merge, parse_container, sha256, windows)

DM1701_V0206 = "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"
MD380_D2032 = "e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43"

EPILOG = """\
examples:
  %(prog)s --stock "DM-1701(CSV)-V02.06.bin"                       (the key is derived from the stock file)
  %(prog)s --stock "DM-1701(CSV)-V02.06.bin" --key ~/keys/dm1701.key
  %(prog)s --stock "MD-380-D2.32(AD).bin"                          (MD-380: image build/md380/odr_md380.bin)
  %(prog)s --stock stock.bin --key dm1701.key --image build/dm1701/odr_dm1701.bin -o build/dm1701/odr_dm1701_codec.bin
  %(prog)s --self-test

then flash:  tools/stm32_dfu.py --model dm1701 --key ~/keys/dm1701.key build/dm1701/odr_dm1701_codec.bin

Supported stock files: Baofeng's DM-1701 V02.06 CSV firmware (sha256 0314efd4...) and TYT's MD-380 "Firmware 2.32"
D002.032 (sha256 e1a1a11a...). The key file is the radio's 1024-byte firmware key (DM-1701 sha256 d983ce95...,
MD-380 5254139c...); without --key it is derived from the stock file and checked by that hash. Neither is in this
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
    check(wins == [(0x08074714, code), (0x080D19F0, tables)], "windows decrypted")

    image = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(rng.randrange(256) for _ in range(5000))
    merged = merge(image, p, wins)
    check(len(merged) == 0x080E4240 - APP_BASE, "merged image ends at the table window end 0x080E4240")
    check(merged[:len(image)] == image, "application unchanged")
    check(merged[len(image):0x08074714 - APP_BASE] == b"\xFF" * (0x08074714 - APP_BASE - len(image)),
          "0xFF up to the code window")
    check(merged[0x08074714 - APP_BASE:0x08084930 - APP_BASE] == code, "code at 0x08074714")
    check(merged[0x08084930 - APP_BASE:0x080D19F0 - APP_BASE] == b"\xFF" * (0x080D19F0 - 0x08084930),
          "0xFF between the windows")
    check(merged[0x080D19F0 - APP_BASE:] == tables, "tables at 0x080D19F0")
    check(not stm32_dfu.check_image(merged, "dm1701"), "stm32_dfu accepts the merged image without warnings")

    # the firmware's sector 9 data (the build's _rodata2.bin) goes in at 0x080A0000, between the windows
    rodata = bytes(rng.randrange(256) for _ in range(3000))
    merged2 = merge(image, p, wins, [(0x080A0000, rodata)])
    check(len(merged2) == len(merged) and merged2[0x080A0000 - APP_BASE:0x080A0000 - APP_BASE + 3000] == rodata,
          "sector 9 data at 0x080A0000")
    check(merged2[0x08074714 - APP_BASE:0x08084930 - APP_BASE] == code and merged2[0x080D19F0 - APP_BASE:] == tables,
          "windows unchanged by sector 9 data")
    check(not stm32_dfu.check_image(merged2, "dm1701"), "stm32_dfu accepts the image with sector 9 data")

    bad_key = bytes([key[0] ^ 1]) + key[1:]
    expect_error(lambda: extract(stock, bad_key, profiles), "wrong key: window hash mismatch")
    expect_error(lambda: extract(stock[:-1] + b"\0", key, profiles), "other file: unsupported sha256")
    expect_error(lambda: extract(stock, key), "synthetic file is not the real stock file")
    expect_error(lambda: merge(b"\0" * (0x08074714 - APP_BASE + 1), p, wins), "image overlapping the codec refused")
    expect_error(lambda: merge(stock, p, wins), "official file as --image refused")
    expect_error(lambda: merge(image, p, wins, [(0x08090000, b"x")]), "data inside codec sector 8 refused")
    expect_error(lambda: merge(image, p, wins, [(0x080BFFFF, b"xx")]), "data running past sector 9 refused")
    expect_error(lambda: parse_container(b"x" * 300), "non-container refused")

    # MD-380 D002.032: single-segment container, code window in sector 6, four small extra table windows
    mstock, mprofiles, mcode, mtables = build_synthetic_stock(key, rng, MD380_D2032)
    mreal = PROFILES[MD380_D2032]
    check(parse_container(mstock) == [(0x0800C000, 0xF2C00)], "MD-380 single-segment container parsed")
    check(app_file_offset(mreal["segments"], 0x08050C44) == 0x44D44, "MD-380 code window file offset 0x44D44 (spec)")
    check(app_file_offset(mreal["segments"], 0x0808FC64) == 0x83D64, "MD-380 table window file offset 0x83D64 (spec)")
    mp, mwins = extract(mstock, key, mprofiles)
    check([a for a, _ in mwins] == [0x08050C44, 0x0808FC64, 0x080D0FC8, 0x080F9698, 0x080F9E74, 0x080FD490],
          "MD-380: all six windows, code first")
    check(mwins[0][1] == mcode and mwins[1][1] == mtables, "MD-380 windows decrypted")
    mimage = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(rng.randrange(256) for _ in range(0x44C44 - 8))
    mmerged = merge(mimage, mp, mwins)
    check(len(mmerged) == 0x080FD4B0 - APP_BASE, "MD-380 merged image ends at the last window 0x080FD4B0")
    check(mmerged[:len(mimage)] == mimage and mmerged[0x08050C44 - APP_BASE:0x080614C8 - APP_BASE] == mcode,
          "MD-380: an application up to 0x08050C44 and the code right after it")
    check(all(mmerged[a - APP_BASE:a - APP_BASE + len(d)] == d for a, d in mwins), "MD-380 windows in place")
    check(mmerged[0x080614C8 - APP_BASE:0x0808FC64 - APP_BASE] == b"\xFF" * (0x0808FC64 - 0x080614C8),
          "MD-380: 0xFF between code and tables")
    check(not stm32_dfu.check_image(mmerged, "md380"), "stm32_dfu accepts the MD-380 merged image without warnings")
    expect_error(lambda: merge(mimage + b"\0", mp, mwins), "MD-380: application one byte into the code refused")
    expect_error(lambda: extract(mstock, bad_key, mprofiles), "MD-380 wrong key refused")
    check(find_profile(mstock, mprofiles)["board"] == "md380", "MD-380 profile found by SHA-256")

    # main(): a missing explicit --image2 is an error, the default sibling file stays optional
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
        check(rc == 0, "no sector 9 file next to the image: merged without it")
        rc, err = run("--image2", os.path.join(d, "missing_rodata2.bin"))
        check(rc == 1 and "missing_rodata2.bin" in err, "an explicit --image2 that doesn't exist is an error")
        with open(os.path.join(d, "x2.bin"), "wb") as f:
            f.write(rodata)
        rc, err = run("--image2", os.path.join(d, "x2.bin"))
        with open(op, "rb") as f:
            check(rc == 0 and f.read()[0x080A0000 - APP_BASE:][:3000] == rodata, "an explicit --image2 lands in sector 9")

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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog=EPILOG,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stock", help="the radio's stock firmware file (DM-1701 V02.06 CSV or MD-380 D002.032)")
    ap.add_argument("--key", help="the radio's firmware key file (1024 bytes); derived from the stock file when left out")
    ap.add_argument("--board", choices=("dm1701", "md380"),
                    help="refuse a stock file for another radio (the stock file picks the board otherwise)")
    ap.add_argument("--image", help="built OpenDigitalRadio image (default: build/<board>/odr_<board>.bin)")
    ap.add_argument("--image2", help="the build's sector 9 image (DM-1701: odr_dm1701_rodata2.bin), written at its address "
                    "in the merged image (default: <image>_rodata2.bin next to the image, when it exists)")
    ap.add_argument("-o", "--output", help="merged image to write (default: <image>_codec.bin next to the image)")
    ap.add_argument("--self-test", action="store_true", help="check the tool's logic on a synthetic file and key")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
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
        data = []
        image2 = args.image2 or os.path.splitext(os.path.expanduser(image_path))[0] + "_rodata2.bin"
        # an explicit --image2 must exist (open() fails into the OSError handler); only the default is optional
        if prof.get("app_data") and (args.image2 or os.path.exists(os.path.expanduser(image2))):
            with open(os.path.expanduser(image2), "rb") as f:
                data = [(prof["app_data"][0][0], f.read())]
        merged = merge(image, prof, wins, data)
        for w in stm32_dfu.check_image(merged, board):
            print("warning: %s" % w)
        with open(out, "wb") as f:
            f.write(merged)
    except (OSError, ExtractError, stm32_dfu.DfuError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    print("stock  %s (%s), %s codec: %s verified"
          % (prof["name"], sha256(stock)[:8], board,
             ", ".join("0x%08X+0x%X" % (a, n) for a, n, _ in windows(prof))))
    print("wrote  %s (%d bytes; contains the proprietary codec: keep it to yourself)" % (out, len(merged)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
