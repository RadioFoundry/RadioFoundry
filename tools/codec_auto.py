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
from rfoundry import stockfw  # noqa: E402  (stock file profiles)

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


def board_profiles(profiles, board):
    """The profiles that count for a board: its own and those that name no board"""
    return {h: p for h, p in profiles.items() if p.get("board", board) == board}


def profile_shas(profiles):
    """The stock files' SHA-256 values of PROFILES keys. A file with its own placement on a board is keyed
    "<sha>@<board>" (stockfw.profile_key), so the key itself never equals a file's hash"""
    return {stockfw.key_sha(h) for h in profiles}


def self_test():
    sha = "ab" * 32
    fake = {sha: {"board": "one"}, stockfw.profile_key(sha, "two"): {"board": "two"}}
    fails = []
    for board in ("one", "two"):
        if profile_shas(board_profiles(fake, board)) != {sha}:
            fails.append("a file keyed for %s is not found by its SHA-256" % board)
    for b, t in CODEC_TOOLS.items():	# every board with a codec tool finds each of its stock files
        if t is None:
            continue
        profs = board_profiles(getattr(__import__(t), "PROFILES", {}), b)
        if profs and not all(stockfw.key_sha(h) in profile_shas(profs) for h in profs):
            fails.append("%s: a stock file is not found" % b)
    for f in fails:
        print("FAIL:", f)
    print("codec_auto self-test %s" % ("PASSED" if not fails else "FAILED"))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--board")
    ap.add_argument("--image", help="the built image")
    ap.add_argument("--self-test", action="store_true", help="check the profile matching, no files needed")
    ap.add_argument("--dir", default=os.path.join(ROOT, "codecs"), help="where to look (default: codecs/)")
    ap.add_argument("-o", "--output", help="merged image (default: <image>_codec.bin)")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.board or not args.image:
        ap.error("--board and --image are required")

    tool = CODEC_TOOLS.get(args.board)
    if tool is None or not os.path.isdir(args.dir):
        return 0
    # one tool may serve several boards: only this board's stock files count
    mod = __import__(tool)
    profiles = board_profiles(getattr(mod, "PROFILES", {}), args.board)
    if hasattr(mod, "identify"):	# a tool that knows a file by more than its SHA-256 (a program flash dump)
        def known(path):
            with open(path, "rb") as f:
                return mod.identify(f.read(), board=args.board) is not None
    else:
        shas = profile_shas(profiles)

        def known(path):
            return sha256_file(path) in shas
    stock = next((p for p in candidates(args.dir) if os.path.getsize(p) > 4096 and known(p)), None)
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
