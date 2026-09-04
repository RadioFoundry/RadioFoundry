#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Package built firmware images as release artifacts (docs/dev/release.md "Release artifacts").

    tools/release.py --board dm1701 --version v0.1.0 [--bin build/dm1701/odr_dm1701.bin] [--out dist]

Writes, into --out:
  odr-<board>-<version>.bin    the codec-less image, exactly as built
  odr-<board>-<version>.json   its manifest (board, version, git hash, image ranges, codec profile, codec sectors)
  SHA256SUMS                   one line per file (appended; one file for the whole release)

Refuses an image that reaches into a codec sector or that carries the codec signature, so a release can never ship
the codec or break "keep the radio's codec" (tools/odrflash).
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "odrflash"))
from odrflash import boards, manifest as mf, stockfw  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*a):
    return subprocess.run(["git", "-C", ROOT] + list(a), capture_output=True, text=True, check=True).stdout.strip()


def features(board_id):
    r = subprocess.run(["make", "-s", "-C", ROOT, "BOARD=" + board_id, "print-config"], capture_output=True, text=True)
    return ",".join(kv.split("=")[0] for kv in r.stdout.split() if kv.endswith("=y"))


def toolchain(board_id):
    r = subprocess.run(["make", "-s", "-C", ROOT, "BOARD=" + board_id, "print-cc"], capture_output=True, text=True)
    cc = r.stdout.strip()
    try:
        v = subprocess.run([cc, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except (OSError, IndexError):
        v = cc
    return v


def check_no_codec(board, image):
    """The release image must not contain any codec bytes: it ends before the codec sectors (manifest.generate checks
    that), and for the DM-1701 the codec's first word must not appear at the code window either."""
    if board.id == "dm1701":
        off = board.codec.windows[0][0] - board.app_base
        if off + 4 <= len(image) and struct.unpack_from("<I", image, off)[0] == stockfw.CODEC_SIG:
            raise SystemExit("release: %s carries the codec signature: never release a merged image" % board.id)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--board", required=True)
    ap.add_argument("--version", required=True, help="the release tag, e.g. v0.1.0")
    ap.add_argument("--bin", help="the built image (default build/<board>/odr_<board>.bin)")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--git", help="full commit hash (default: git rev-parse HEAD)")
    ap.add_argument("--features", help="feature list (default: make print-config)")
    ap.add_argument("--toolchain", help="compiler version line (default: the board's compiler --version)")
    args = ap.parse_args(argv)

    board = boards.get(args.board)
    path = args.bin or os.path.join(ROOT, "build", board.id, "odr_%s.bin" % board.id)
    with open(path, "rb") as f:
        image = f.read()
    check_no_codec(board, image)
    commit = args.git or git("rev-parse", "HEAD")
    feats = args.features if args.features is not None else features(board.id)
    tc = args.toolchain if args.toolchain is not None else toolchain(board.id)
    try:
        man = mf.generate(board, image, args.version, commit, feats, tc)
    except mf.ManifestError as e:
        raise SystemExit("release: %s" % e)
    mf.verify(man, image, board.id)

    os.makedirs(args.out, exist_ok=True)
    bin_name = mf.image_name(board.id, args.version)
    json_name = mf.image_name(board.id, args.version, "json")
    text = mf.dumps(man)
    with open(os.path.join(args.out, bin_name), "wb") as f:
        f.write(image)
    with open(os.path.join(args.out, json_name), "w", encoding="utf-8") as f:
        f.write(text)
    sums = os.path.join(args.out, "SHA256SUMS")
    old = []
    if os.path.exists(sums):
        with open(sums, "r", encoding="ascii") as f:
            old = [ln for ln in f if not ln.rstrip().endswith((" " + bin_name, " " + json_name))]
    with open(sums, "w", encoding="ascii") as f:
        f.writelines(old)
        f.write(mf.sha256sums_line(bin_name, image))
        f.write(mf.sha256sums_line(json_name, text.encode("utf-8")))
    print("release: %s (%d bytes, sectors %s; codec %s in sectors %s, not included)"
          % (bin_name, len(image), man["sectors"], man["codec"]["profile"], man["codec"]["sectors"]))
    print("release: %s, SHA256SUMS in %s" % (json_name, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
