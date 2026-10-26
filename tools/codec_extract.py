#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Merge a radio's own stock AMBE+2 codec into a RadioFoundry image, on your machine (the STM32 radios whose
board.toml says [codec] tool = "codec_extract").

Recipes from each radio's codec page (docs/spec/codec-<radio>.md, "Extraction recipe"):
  1. check the stock file ("OutSecurityBin" container, one of rfoundry/stockfw.py PROFILES) by SHA-256 and its segment
     table; the file picks the radio. Or take a read of the radio's internal flash made while it ran its stock firmware
     (stub/stubtool.py PORT --progmem FILE): the codec is plain there, found by the code window's SHA-256, no key;
  2. decrypt every codec window: plain = file[off] XOR key[(off - 0x100) mod 1024], with the radio's 1024-byte key
     (a key file checked by SHA-256 as tools/stm32_dfu.py does, or derived from the stock file and checked the same
     way);
  3. check each window's SHA-256 and the code's first word 0x4FF0E92D;
  4. write a new image: the built application from 0x0800C000, then the codec where its profile places it, 0xFF between
     (docs/spec/codec-placement.md): the tables at their stock address, the code moved as one block (right after
     the tables, or at the start of sector 10 or 11 as the build's profile says), with the addresses the build's
     codec map lists rewritten, and the descriptor at 0x080FFF80 that tells the firmware where the code is.

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

# The container, decryption, profiles and merge live in the rfoundry package (one implementation for both tools)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "rfoundry"))
from rfoundry.stockfw import (APP_BASE, CODEC_SIG, DESC_ADDR, FLASH_BASE, FLASH_END, HEADER_LEN, KEY_LEN,  # noqa: E402,F401
                              MAGIC, PROFILES, SYN_BL, SYN_SELF_REF, StockError as ExtractError, app_file_offset,
                              build_synthetic_stock, decrypt, derive_key, descriptor, extract, find_profile, from_flash,
                              is_container, layout, merge, no_codec, parse_container, profile_for_board, sha256,
                              windows)
from rfoundry.codecmap import bl_target  # noqa: E402
from rfoundry import boards  # noqa: E402  (the board registry: the boards this tool serves)
import zlib  # noqa: E402

DM1701_V0206 = "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"
MD380_D2032 = "e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43"
MDUV380_V1926 = "75e4d470b451cb5d191a647c0836c71801433b826cc4a7fe2c48d750036bd98e"
RT3S_V21960 = "447ed4e09482c45661cd6e4c83d1e9f777463993ab7dd48b0e0597bb14f519ee"
DM1701_V0201 = "cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713"
DM1701_V0206_NOCSV = "4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9"

EPILOG = """\
examples:
  %(prog)s --stock "DM-1701(CSV)-V02.06.bin"                       (the key is derived from the stock file)
  %(prog)s --stock "DM-1701(CSV)-V02.06.bin" --key ~/keys/dm1701.key
  %(prog)s --stock "MD-380-D2.32(AD).bin"                          (MD-380: image build/md380/rfoundry_md380.bin)
  %(prog)s --stock flash.bin --board mduv380                       (a stub read of a radio on its stock firmware)
  %(prog)s --stock stock.bin --key dm1701.key --image build/dm1701/rfoundry_dm1701.bin -o build/dm1701/rfoundry_dm1701_codec.bin
  %(prog)s --self-test

then flash:  tools/stm32_dfu.py --model dm1701 --key ~/keys/dm1701.key build/dm1701/rfoundry_dm1701_codec.bin

Supported stock files: rfoundry/stockfw.py PROFILES (--help lists them below). The key file is the radio's
1024-byte firmware key (its SHA-256 is in the board's board.toml [flash.dfu]); without --key it is derived from the
stock file and checked by that hash. Neither is in this repository. For flashing release images, tools/rfoundry does
all of this in one step.

supported stock files:
""" + "".join("  %-34s %-14s %s (sha256 %s...)\n" % (p["name"], p["board"], p["version"], h[:8])
              for h, p in PROFILES.items())


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

    # MD-UV380 V19.26 CSV: two-segment container, the DM-1701's placement at its own addresses; and the same codec from
    # a read of the internal flash of a radio on its stock firmware (the application plain at 0x0800C000)
    ustock, uprofiles, ucode, _ = build_synthetic_stock(key, rng, MDUV380_V1926)
    up, uwins = extract(ustock, key, uprofiles)
    check([(a, len(d)) for a, d in uwins] == [(0x080EB300, len(ucode)), (0x080D8C94, 0x1266A), (DESC_ADDR, 0x80)],
          "MD-UV380: code at 0x080EB300 after the tables cut at 0x080EB2FE, then the descriptor")
    check(struct.unpack_from("<I", uwins[2][1], 8)[0] == 0x00561926, "MD-UV380 V19.26 descriptor tag")
    usegs = parse_container(ustock)
    flash = bytearray(b"\xFF" * (FLASH_END - FLASH_BASE))
    flash[APP_BASE - FLASH_BASE:APP_BASE - FLASH_BASE + usegs[1][1]] = decrypt(
        ustock, app_file_offset(usegs, APP_BASE), usegs[1][1], key)
    check(from_flash(bytes(flash), profiles=uprofiles) == (up, uwins), "MD-UV380: the codec from a flash readout")
    flash[0x0807B830 - FLASH_BASE + 8] ^= 1
    check(from_flash(bytes(flash), profiles=uprofiles) is None, "a readout with a changed code byte gives nothing")
    umerged = merge(struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(0x080C0000 - APP_BASE - 8), up, uwins)
    check(not stm32_dfu.check_image(umerged, "mduv380"), "stm32_dfu accepts the MD-UV380 merged image")
    # RT3S V219.60: the code doesn't fit after the tables, so it goes to the start of sector 10, below them
    rstock, rprofiles, rcode, _ = build_synthetic_stock(key, rng, RT3S_V21960)
    rp, rwins = extract(rstock, key, rprofiles)
    check([a for a, _ in rwins] == [0x080C0000, 0x080E322C, DESC_ADDR] and 0x080C0000 + len(rcode) <= 0x080E322C,
          "RT3S 219.60: code at the start of sector 10, below the tables")
    rmerged = merge(struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(0x080C0000 - APP_BASE - 8), rp, rwins)
    check(not stm32_dfu.check_image(rmerged, "mduv380"), "stm32_dfu accepts the RT3S 219.60 merged image")
    # DM-1701 V02.01 CSV (V02.02 and V02.03 alike): the V02.06 placement 0x40 lower, its own tag
    ostock, oprofiles, ocode, _ = build_synthetic_stock(key, rng, DM1701_V0201)
    op, owins = extract(ostock, key, oprofiles)
    check([(a, len(d)) for a, d in owins] == [(0x080E3A34, len(ocode)), (0x080D19B0, 0x12082), (DESC_ADDR, 0x80)],
          "DM-1701 V02.01: code at 0x080E3A34 after the tables cut at 0x080E3A32, then the descriptor")
    check(struct.unpack_from("<I", owins[2][1], 8)[0] == 0x0203, "DM-1701 V02.01 descriptor tag")
    omerged = merge(struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(0x080C0000 - APP_BASE - 8), op, owins)
    check(not stm32_dfu.check_image(omerged, "dm1701"), "stm32_dfu accepts the DM-1701 V02.01 merged image")
    # DM-1701 V02.06 noCSV (V02.03 noCSV alike, 0x40 lower): a larger application segment, the codec higher up, the
    # code 0x1C bytes shorter; still after the tables in sector 11, below the descriptor
    nstock, nprofiles, ncode, _ = build_synthetic_stock(key, rng, DM1701_V0206_NOCSV)
    np_, nwins = extract(nstock, key, nprofiles)
    check([(a, len(d)) for a, d in nwins] == [(0x080E6828, 0x10200), (0x080D4794, 0x12092), (DESC_ADDR, 0x80)]
          and len(ncode) == 0x10200 and 0x080E6828 + 0x10200 <= DESC_ADDR,
          "DM-1701 V02.06 noCSV: code at 0x080E6828 after the tables cut at 0x080E6826, then the descriptor")
    check(struct.unpack_from("<I", nwins[2][1], 8)[0] == 0x10206, "DM-1701 V02.06 noCSV descriptor tag")
    nmerged = merge(struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(0x080C0000 - APP_BASE - 8), np_, nwins)
    check(not stm32_dfu.check_image(nmerged, "dm1701"), "stm32_dfu accepts the DM-1701 V02.06 noCSV merged image")

    # main() end to end on the synthetic file
    import contextlib
    import io
    import tempfile
    from unittest import mock
    with tempfile.TemporaryDirectory() as d:
        sp, ip, op = (os.path.join(d, n) for n in ("stock.bin", "rfoundry_dm1701.bin", "out.bin"))
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
        check_output_path(os.path.join(repo_root(), "build", "dm1701", "rfoundry_dm1701_codec.bin"))
        check(True, "output under build/ allowed")
    except ExtractError:
        check(False, "output under build/ allowed")
    check(sha256(key) not in [h for h, _ in stm32_dfu.KEYS.values()], "synthetic key is not a real key")

    print("%d failures" % len(fails))
    return 1 if fails else 0


def test_desc(path, board="dm1701"):
    """Write the codec sectors as the merge leaves them for a synthetic codec of the board's first stock file, for
    tests/host/codecDescTest.c (the firmware's descriptor checks and this writer must agree): from the end of the
    board's application region (the DM-1701's sectors 10 and 11, 0x080C0000..0x08100000; the MD-380's sectors 8-11,
    0x08080000..0x08100000) to the end of flash"""
    import random
    rng = random.Random(1701)
    key = bytes(rng.randrange(256) for _ in range(KEY_LEN))
    stock, profiles, _, _ = build_synthetic_stock(key, rng, profile_for_board(board))
    p, wins = extract(stock, key, profiles)
    lo = boards.get(board).app_region[1]
    out = bytearray(b"\xFF" * (FLASH_END - lo))
    for a, d in wins:
        out[a - lo:a - lo + len(d)] = d
    with open(path, "wb") as f:
        f.write(out)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog=EPILOG,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stock", help="the radio's stock firmware file (one listed below), or a read of its internal "
                    "flash while it ran its stock firmware (stubtool.py PORT --progmem FILE)")
    ap.add_argument("--key", help="the radio's firmware key file (1024 bytes); derived from the stock file when left out")
    ap.add_argument("--board", choices=sorted(b for b, t in boards.BOARDS.items() if t.codec_tool == "codec_extract"),
                    help="refuse a stock file for another radio (the stock file picks the board otherwise)")
    ap.add_argument("--image", help="built RadioFoundry image (default: build/<board>/rfoundry_<board>.bin)")
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
        if no_codec(stock):
            raise ExtractError(no_codec(stock))
        readout = None if is_container(stock) else from_flash(stock)
        if not is_container(stock) and readout is None:
            raise ExtractError("%s is neither a supported stock file nor a flash readout (from 0x%08X) holding a "
                               "supported codec" % (args.stock, FLASH_BASE))
        prof = readout[0] if readout else find_profile(stock)
        board = prof["board"]
        if args.board and args.board != board:
            raise ExtractError("%s is a %s stock file, not a %s one" % (args.stock, board, args.board))
        key = None
        if readout is None:
            key = stm32_dfu.load_key(args.key, board) if args.key else derive_key(stock, prof["key_sha256"])
        image_path = args.image or os.path.join(repo_root(), "build", board, "rfoundry_%s.bin" % board)
        out = args.output or os.path.splitext(image_path)[0] + "_codec.bin"
        with open(os.path.expanduser(image_path), "rb") as f:
            image = f.read()
        check_output_path(out)
        prof, wins = readout or extract(stock, key)
        merged = merge(image, prof, wins)
        for w in stm32_dfu.check_image(merged, board):
            print("warning: %s" % w)
        with open(out, "wb") as f:
            f.write(merged)
    except (OSError, ExtractError, stm32_dfu.DfuError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    print("stock  %s (%s), %s codec: %s verified"
          % (prof["name"] if readout is None else "flash readout with " + prof["version"], sha256(stock)[:8], board,
             ", ".join("0x%08X+0x%X" % (a, n) for a, n, _ in layout(prof))))
    print("wrote  %s (%d bytes; contains the proprietary codec: keep it to yourself)" % (out, len(merged)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
