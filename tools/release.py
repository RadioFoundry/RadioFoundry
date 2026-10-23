#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Package built firmware images as release artifacts (docs/dev/release.md "Release artifacts").

    tools/release.py --board ID --version v0.1.0 [--bin build/ID/odr_ID.bin] [--out dist]
    tools/release.py --board ID --variant amateur --version v0.1.0     a release variant (make VARIANT=amateur)
    tools/release.py --board ID --version v0.1.0     on an HR_C7000 radio build/ID/odr_ID.bin is the image without
                                                     the codec slot (never the _codec.bin)
    tools/release.py --board ID --build-option BAND=vhf --version v0.1.0   a build option's image (board.toml
                                                     [[build_options]]): build/ID-vhf/odr_ID-vhf.bin, odr-ID-vhf-<version>

Writes, into --out:
  odr-<board>-<version>.bin    the codec-less image, as built (a variant or a build option value:
                               odr-<board>-<variant>-<value>-<version>.bin)
  odr-<board>-<version>.json   its manifest (board, variant, version, git hash, image ranges, codec profile, sectors)
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
from odrflash import boards, manifest as mf, serialflash, stockfw  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*a):
    return subprocess.run(["git", "-C", ROOT] + list(a), capture_output=True, text=True, check=True).stdout.strip()


def features(board_id, variant=None, options=()):
    cmd = (["make", "-s", "-C", ROOT, "BOARD=" + board_id, "print-config"] + (["VARIANT=" + variant] if variant else [])
           + ["%s=%s" % o for o in sorted(dict(options).items())])
    r = subprocess.run(cmd, capture_output=True, text=True)
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
    that), and the codec's first word must not appear where a merged image puts it: an STM32 radio's code window (or
    the stock address, where merges from before the codec moved put it), an HR_C7000 radio's codec slot (the build's
    _codec.bin)."""
    off = board.codec.windows[0][0] - board.app_base
    if board.codec_slot is None:	# a codec merged from a stock file at the stock placement
        offs = {off} | {p["code"][0] - board.app_base for p in stockfw.PROFILES.values() if p["board"] == board.id}
        if any(o + 4 <= len(image) and struct.unpack_from("<I", image, o)[0] == stockfw.CODEC_SIG for o in offs):
            raise SystemExit("release: %s carries the codec signature: never release a merged image" % board.id)
    else:				# a codec in ODR's own slot
        if off + 4 <= len(image) and struct.unpack_from("<I", image, off)[0] == serialflash.SLOT_MAGIC:
            raise SystemExit("release: the %s image carries the codec slot (a _codec.bin): release odr_%s.bin, never "
                             "a merged image" % (board.name, board.id))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--board", required=True)
    ap.add_argument("--version", required=True, help="the release tag, e.g. v0.1.0")
    ap.add_argument("--variant", help="a release variant (config/variants/<name>.mk); default: the full build")
    ap.add_argument("--build-option", action="append", default=[], metavar="NAME=VALUE",
                    help="a board build option's value other than its default (boards/<board>/board.toml "
                         "[[build_options]]): its own image, build directory and file names")
    ap.add_argument("--bin", help="the built image (default build/<board>[-<variant>]/odr_<board>.bin, or "
                    "odr_<board>_install.bin for a board with a first stage)")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--git", help="full commit hash (default: git rev-parse HEAD)")
    ap.add_argument("--features", help="feature list (default: make print-config)")
    ap.add_argument("--toolchain", help="compiler version line (default: the board's compiler --version)")
    args = ap.parse_args(argv)

    try:
        board = boards.get(args.board)
    except KeyError as e:
        raise SystemExit("release: %s" % e.args[0])
    variant = None if args.variant in (None, "", "full") else args.variant
    if variant and not os.path.isfile(os.path.join(ROOT, "config", "variants", variant + ".mk")):
        raise SystemExit("release: unknown variant %s (config/variants/)" % variant)
    options = {}
    for opt in args.build_option:
        name, _, value = opt.partition("=")
        if board.build_option(name, value) is None:
            raise SystemExit("release: %s has no build option %s=%s (its options: %s)"
                             % (board.id, name, value, ", ".join("%s=%s" % (o["name"], "|".join(v["id"] for v in o["values"]))
                                                                for o in board.build_options) or "none"))
        options[name] = value
    # the Makefile's names (odrflash/names.py): a non-default value is in the build directory and the image
    # (build/md380-vhf/odr_md380-vhf.bin)
    path = args.bin or os.path.join(ROOT, board.build_image(options, variant))
    with open(path, "rb") as f:
        image = f.read()
    check_no_codec(board, image)
    # the image is written at app_base, where the first stage lives: the application alone would replace it
    if board.first_stage:
        size, head = board.first_stage
        if len(image) <= size or not image.startswith(head):
            raise SystemExit("release: the %s image must start with its first stage: release odr_%s_install.bin, not the "
                             "application alone" % (board.name, board.id))
    commit = args.git or git("rev-parse", "HEAD")
    feats = args.features if args.features is not None else features(board.id, variant, options)
    tc = args.toolchain if args.toolchain is not None else toolchain(board.id)
    # variant and build option images carry them in their names; the full default build keeps the plain names odrflash
    # fetches
    name_id = board.release_id(options, variant)
    bin_name = mf.image_name(name_id, args.version)
    json_name = mf.image_name(name_id, args.version, "json")
    try:
        man = mf.generate(board, image, args.version, commit, feats, tc, file_name=bin_name)
    except mf.ManifestError as e:
        raise SystemExit("release: %s" % e)
    man["variant"] = variant or "full"
    if options:
        man["build_options"] = dict(options)
    whole = image
    mf.verify(man, whole, board.id)

    os.makedirs(args.out, exist_ok=True)
    text = mf.dumps(man)
    with open(os.path.join(args.out, bin_name), "wb") as f:
        f.write(whole)
    with open(os.path.join(args.out, json_name), "w", encoding="utf-8") as f:
        f.write(text)
    sums = os.path.join(args.out, "SHA256SUMS")
    old = []
    if os.path.exists(sums):
        with open(sums, "r", encoding="ascii") as f:
            old = [ln for ln in f if not ln.rstrip().endswith((" " + bin_name, " " + json_name))]
    with open(sums, "w", encoding="ascii") as f:
        f.writelines(old)
        f.write(mf.sha256sums_line(bin_name, whole))
        f.write(mf.sha256sums_line(json_name, text.encode("utf-8")))
    print("release: %s (%d bytes, sectors %s; codec %s in sectors %s, not included)"
          % (bin_name, len(whole), man["sectors"], man["codec"]["profile"], man["codec"]["sectors"]))
    print("release: %s, SHA256SUMS in %s" % (json_name, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
