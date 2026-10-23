#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Merge a K22F radio's own AMBE+2 codec into an OpenDigitalRadio image, on your machine (docs/spec/codec-gd77.md).

The codec belongs to DVSI and the radio maker: this repository and its releases never contain it. The tool reads the
radio's stock firmware as a flat flash image (the program flash from address 0x4000: byte 0 of the file is the vector
table) and writes the application image with the codec placed behind it, to a file you choose (by default next to the
built image, in build/, which git ignores).

    k22f_codec.py --board BOARD --stock stock-flash.bin          (image build/<board>/odr_<board>.bin)
    k22f_codec.py --stock stock.bin --image build/<board>/odr_<board>.bin -o build/<board>/odr_<board>_codec.bin
    k22f_codec.py --self-test

Supported stock builds: one codec pack each (tools/odrflash/odrflash/codec_packs/<board>/); the file is recognized by
the hashes of the codec's three parts, not by its name. The result is a flat image from 0x4000, to be packaged for the
radio's update path (README, docs/spec/hardware-gd77.md 10).
"""
import argparse
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "odrflash"))
from odrflash import k22fcodec as G  # noqa: E402

PROFILES = {}		# tools/codec_auto.py reads this; the stock files are recognized by identify() instead


class ExtractError(Exception):
    pass


def repo_root():
    return os.path.dirname(HERE)


def identify(data, board=None):
    """The profile name of a flat stock image (board's, when given), or None."""
    s = G.find_spec(data, board=board)
    return s["profile"] if s else None


def check_output_path(path):
    """Refuse to write the merged image anywhere git would pick it up."""
    root = repo_root()
    full = os.path.abspath(path)
    if os.path.commonpath([full, root]) != root:
        return
    try:
        ignored = subprocess.run(["git", "-C", root, "check-ignore", "-q", full], capture_output=True).returncode == 0
    except OSError:
        rel = os.path.relpath(full, root).split(os.sep)
        ignored = rel[0] in ("build", "codec") or full.endswith(".bin")
    if not ignored:
        raise ExtractError("%s is inside the repository and not ignored by git: the merged image contains the "
                           "proprietary codec, write it under build/ or outside the tree" % path)


def merge_file(stock, image, specs=None, board=None):
    spec = G.find_spec(stock, specs, board=board)
    if spec is None:
        raise ExtractError("this is not a supported stock firmware (as a flat flash image from 0x4000). Supported: %s"
                           % ", ".join(sorted(G.load_specs(board=board))))
    try:
        packed = G.pack(stock, spec)
        return spec, G.merge(image, packed), packed
    except G.PackError as e:
        raise ExtractError(str(e))


def self_test():
    sys.path.insert(0, os.path.join(HERE, "odrflash", "tests"))
    import test_k22fcodec as T
    img, spec = T.synthetic(random.Random(77))
    fails = []

    def check(cond, msg):
        print(("ok    " if cond else "FAIL  ") + msg)
        if not cond:
            fails.append(msg)
    check(G.find_spec(img, {"synthetic": spec}) is spec, "the synthetic build is recognized by its hashes")
    pk = G.pack(img, spec)
    app = bytes(random.Random(1).randrange(256) for _ in range(5000))
    merged = G.merge(app, pk, 0x4309)
    check(merged[:len(app)] == app, "the application is kept")
    check(merged[G.CODE_BASE - G.FLASH_BASE:G.CODE_BASE - G.FLASH_BASE + len(pk.code)] == pk.code, "the code region is in place")
    check(len(merged) == G.DESC_ADDR + G.DESC_LEN - G.FLASH_BASE, "the image ends with the descriptor")
    try:
        check_output_path(os.path.join(repo_root(), "tools", "x_codec.img"))
        check(False, "output inside the tree and not ignored refused")
    except ExtractError:
        check(True, "output inside the tree and not ignored refused")
    print("%d failures" % len(fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog=__doc__.split("\n", 2)[2],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stock", help="the radio's stock firmware as a flat flash image from 0x4000")
    ap.add_argument("--board", help="the radio: refuse a stock file for another one, and find the built image")
    ap.add_argument("--image", help="built OpenDigitalRadio image (default: build/<board>/odr_<board>.bin)")
    ap.add_argument("-o", "--output", help="merged image to write (default: <image>_codec.bin next to the image)")
    ap.add_argument("--self-test", action="store_true", help="check the tool's logic on a synthetic file")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.stock:
        ap.error("--stock is required")
    if not args.image and not args.board:
        ap.error("--image or --board is required")
    image_path = args.image or os.path.join(repo_root(), "build", args.board, "odr_%s.bin" % args.board)
    out_path = args.output or os.path.splitext(image_path)[0] + "_codec.bin"
    try:
        with open(args.stock, "rb") as f:
            stock = f.read()
        with open(image_path, "rb") as f:
            image = f.read()
        check_output_path(out_path)
        spec, merged, packed = merge_file(stock, image, board=args.board)
    except (ExtractError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    with open(out_path, "wb") as f:
        f.write(merged)
    print("stock  %s: code 0x%08X+0x%X, tables 0x%08X+0x%X, descriptor 0x%08X" % (
        spec["profile"], packed.code_addr, len(packed.code), packed.tab_addr, len(packed.tab), G.DESC_ADDR))
    print("wrote  %s (%d bytes; contains the proprietary codec: keep it to yourself)" % (out_path, len(merged)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
