#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Merge the codec into a freshly built image when the radio's stock firmware is in codecs/ (run by make).

Looks through codecs/ for a stock firmware file the board's codec tool supports (checked by SHA-256, so file names
don't matter) and runs that tool to write <image>_codec.bin next to the image. With no supported file it says so and
does nothing: the plain image is still the build's output. See codecs/README.md.
"""
import argparse
import hashlib
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

# Board -> codec tool (each takes --stock, --image, -o and keeps its supported stock files in PROFILES)
CODEC_TOOLS = {"dm1701": "codec_extract", "dm32": "dm32_codec"}
SKIP = ("README.md", ".gitignore")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def candidates(folder):
    for dirpath, dirnames, filenames in os.walk(folder):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            if name not in SKIP and not name.startswith("."):
                yield os.path.join(dirpath, name)


def find_key(folder, board):
    """A DM-1701 key file in the folder (optional: the codec tool derives the key from the stock file without one)."""
    if board != "dm1701":
        return None
    import stm32_dfu
    want = stm32_dfu.KEYS.get("dm1701", (None,))[0]
    for path in candidates(folder):
        if os.path.getsize(path) == 1024 and sha256_file(path) == want:
            return path
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--board", required=True)
    ap.add_argument("--image", required=True, help="the built image")
    ap.add_argument("--dir", default=os.path.join(ROOT, "codecs"), help="where to look (default: codecs/)")
    ap.add_argument("-o", "--output", help="merged image (default: <image>_codec.bin)")
    args = ap.parse_args()

    tool = CODEC_TOOLS.get(args.board)
    if tool is None or not os.path.isdir(args.dir):
        return 0
    profiles = __import__(tool).PROFILES
    stock = next((p for p in candidates(args.dir) if os.path.getsize(p) > 4096 and sha256_file(p) in profiles), None)
    out = args.output or os.path.splitext(args.image)[0] + "_codec.bin"
    if stock is None:
        print("codec: no supported %s stock firmware in %s, built without the codec (codecs/README.md)"
              % (args.board, os.path.relpath(args.dir)))
        if os.path.exists(out) and os.path.getmtime(out) < os.path.getmtime(args.image):
            os.remove(out)	# a merged image from an older build would be flashed by mistake
        return 0
    cmd = [sys.executable, os.path.join(TOOLS, tool + ".py"), "--stock", stock, "--image", args.image, "-o", out]
    key = find_key(args.dir, args.board)
    if key:
        cmd += ["--key", key]
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
