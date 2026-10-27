#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
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
sys.path.insert(0, os.path.join(TOOLS, "rfoundry"))
from rfoundry import boards  # noqa: E402  (the board registry)

# Board -> codec tool, the registry's [codec] tool (each takes --stock, --image, -o and keeps its supported stock files
# in PROFILES)
CODEC_TOOLS = {b.id: b.codec_tool for b in boards.BOARDS.values()}
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
            path = os.path.join(dirpath, name)
            if name not in SKIP and not name.startswith(".") and os.path.isfile(path):	# not a dangling link
                yield path


def find_key(folder, board):
    """The board's key file in the folder (optional: the codec tool derives the key from the stock file without one;
    the boards with an encrypted stock container, whose registry entry has the key's hash)."""
    want = boards.BOARDS[board].key_sha256 if board in boards.BOARDS else None
    if want is None:
        return None
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
    # one tool may serve several boards: only this board's stock files count
    mod = __import__(tool)
    profiles = {h: p for h, p in mod.PROFILES.items() if p.get("board", args.board) == args.board}
    if hasattr(mod, "identify"):	# a tool that knows a file by more than its SHA-256 (a program flash dump)
        # and may take another board's files too ([codec] from: a second SoC that runs the main SoC's codec): the rank of the file's
        # board among them, the preferred one 0, else None
        def known(path):
            with open(path, "rb") as f:
                prof = mod.identify(f.read(), board=args.board)
            return None if prof is None else getattr(mod, "source_rank", lambda p, b: 0)(prof, args.board)
    else:
        def known(path):
            return 0 if sha256_file(path) in profiles else None
    stock, best = None, None
    for path in candidates(args.dir):
        rank = known(path) if os.path.getsize(path) > 4096 else None
        if rank is not None and (best is None or rank < best):
            stock, best = path, rank
            if rank == 0:
                break
    out = args.output or os.path.splitext(args.image)[0] + "_codec.bin"
    if stock is None:
        print("codec: no supported %s stock firmware in %s, built without the codec (codecs/README.md)"
              % (args.board, os.path.relpath(args.dir)))
        if os.path.exists(out) and os.path.getmtime(out) < os.path.getmtime(args.image):
            os.remove(out)	# a merged image from an older build would be flashed by mistake
        return 0
    cmd = [sys.executable, os.path.join(TOOLS, tool + ".py"), "--stock", stock, "--image", args.image, "-o", out]
    cmd += ["--board", args.board]
    key = find_key(args.dir, args.board)
    if key:
        cmd += ["--key", key]
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
