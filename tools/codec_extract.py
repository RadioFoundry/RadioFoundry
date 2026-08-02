#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Merge the DM-1701's own stock AMBE+2 codec into an OpenDigitalRadio image, on your machine.

Recipe from docs/spec/codec-dm1701.md ("Extraction recipe (V02.06 CSV)"):
  1. check the stock file (Baofeng DM-1701 V02.06 CSV, "OutSecurityBin" container) by SHA-256 and its segment table;
  2. decrypt the codec code window and the table window: plain = file[off] XOR key[(off - 0x100) mod 1024], with the
     DM-1701 1024-byte key (the same key file tools/stm32_dfu.py uses, checked by SHA-256, hardware-dm1701.md 15.3);
  3. check each window's SHA-256 and the code's first word 0x4FF0E92D;
  4. write a new image: the built application from 0x0800C000, 0xFF padding, the code at 0x08074714, 0xFF padding,
     the tables at 0x080D19F0. Nothing is relocated: the codec runs in place at its stock addresses.

The codec is DVSI's and Baofeng's, not ours: it is never stored in this repository or in a release. This tool reads
your stock file and your key file and writes the merged image to a file you choose (by default next to the built
image, in build/, which git ignores). Flash the result with tools/stm32_dfu.py.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stm32_dfu  # noqa: E402  (key table and key file check, hardware-dm1701.md 15.3)

APP_BASE = 0x0800C000
FLASH_END = 0x08100000
KEY_LEN = 1024
HEADER_LEN = 0x100
MAGIC = b"OutSecurityBin"
CODEC_SIG = 0x4FF0E92D

# One profile per supported stock file, keyed by its SHA-256 (codec-dm1701.md, "Stock files" and "Codec in the stock
# image"). Window = (flash address, length, SHA-256 of the decrypted bytes).
PROFILES = {
    "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d": {
        "name": "DM-1701(CSV)-V02.06.bin",
        "size": 1272320,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xD9400)],
        "code": (0x08074714, 0x1021C, "1d8d04603b288e102f70252c1dd489600dfde656eccea6264e7bf4fac413f0cd"),
        "tables": (0x080D19F0, 0x12850, "b99f14e72bf72555946c1f04e8a1636652e6f62843baaf7a135787879e028894"),
    },
}

EPILOG = """\
examples:
  %(prog)s --stock "DM-1701(CSV)-V02.06.bin" --key ~/keys/dm1701.key
  %(prog)s --stock stock.bin --key dm1701.key --image build/dm1701/odr_dm1701.bin -o build/dm1701/odr_dm1701_codec.bin
  %(prog)s --self-test

then flash:  tools/stm32_dfu.py --model dm1701 --key ~/keys/dm1701.key build/dm1701/odr_dm1701_codec.bin

The stock file is Baofeng's DM-1701 V02.06 CSV firmware (sha256 0314efd4...). The key file is the 1024-byte DM-1701
firmware key (sha256 d983ce95...); neither is in this repository.
"""


class ExtractError(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_container(data):
    """Return the segment list [(address, length), ...] of an OutSecurityBin file."""
    if len(data) < HEADER_LEN or not data.startswith(MAGIC):
        raise ExtractError("not a TYT/Baofeng OutSecurityBin firmware file (no \"OutSecurityBin\" header)")
    count = struct.unpack_from("<I", data, 0x7C)[0]
    if not 1 <= count <= 15:
        raise ExtractError("implausible segment count %d" % count)
    segs = [struct.unpack_from("<II", data, 0x80 + 8 * i) for i in range(count)]
    total = sum(length for _, length in segs)
    if HEADER_LEN + total > len(data):	# the V02.06 files carry 0x100 more bytes after the last segment
        raise ExtractError("segment lengths (0x%X) run past the end of the file (0x%X)" % (total, len(data)))
    return segs


def app_file_offset(segs, addr):
    """File offset of flash address addr inside the application segment."""
    off = HEADER_LEN
    for base, length in segs:
        if base <= addr < base + length:
            return off + addr - base
        off += length
    raise ExtractError("address 0x%08X is in no segment" % addr)


def decrypt(data, off, length, key):
    """plain[i] = file[off + i] XOR key[(off + i - 0x100) mod 1024]"""
    if off < HEADER_LEN or off + length > len(data):
        raise ExtractError("window 0x%X+0x%X is outside the payload" % (off, length))
    return bytes(b ^ key[(off + i - HEADER_LEN) % KEY_LEN] for i, b in enumerate(data[off:off + length]))


def extract(stock, key, profiles=None):
    """Check the stock file and return (profile, code bytes, table bytes)."""
    profiles = PROFILES if profiles is None else profiles
    digest = sha256(stock)
    prof = profiles.get(digest)
    if prof is None:
        raise ExtractError("unsupported stock file (sha256 %s). Supported: %s"
                           % (digest, ", ".join("%s (%s...)" % (p["name"], h[:8]) for h, p in profiles.items())))
    if len(stock) != prof["size"]:
        raise ExtractError("size %d, expected %d" % (len(stock), prof["size"]))
    segs = parse_container(stock)
    if segs != prof["segments"]:
        raise ExtractError("segment table %s, expected %s" % (segs, prof["segments"]))
    out = []
    for which in ("code", "tables"):
        addr, length, want = prof[which]
        plain = decrypt(stock, app_file_offset(segs, addr), length, key)
        got = sha256(plain)
        if got != want:
            raise ExtractError("decrypted %s window sha256 %s, expected %s (wrong key?)" % (which, got, want))
        out.append(plain)
    code, tables = out
    if struct.unpack_from("<I", code, 0)[0] != CODEC_SIG:
        raise ExtractError("codec first word is not 0x%08X" % CODEC_SIG)
    return prof, code, tables


def merge(image, prof, code, tables):
    """The built application image (plain, from 0x0800C000) with both windows written at their stock addresses."""
    if image.startswith(MAGIC):
        raise ExtractError("--image is an official firmware file; pass the OpenDigitalRadio build (odr_dm1701.bin)")
    caddr, clen, _ = prof["code"]
    taddr, tlen, _ = prof["tables"]
    if APP_BASE + len(image) > caddr:
        raise ExtractError("the application image ends at 0x%08X, inside the codec code window at 0x%08X: rebuild "
                           "with the window reserved (boards/dm1701/dm1701.ld)" % (APP_BASE + len(image), caddr))
    end = taddr + tlen
    out = bytearray(b"\xFF" * (end - APP_BASE))
    out[0:len(image)] = image
    out[caddr - APP_BASE:caddr - APP_BASE + clen] = code
    out[taddr - APP_BASE:taddr - APP_BASE + tlen] = tables
    if end > FLASH_END:
        raise ExtractError("merged image would pass the end of flash")
    return bytes(out)


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

    # A synthetic stock file with the real layout: random key, random windows, everything else random
    key = bytes(rng.randrange(256) for _ in range(KEY_LEN))
    real = PROFILES["0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"]
    segs = real["segments"]
    payload_len = sum(length for _, length in segs)
    plain = bytearray(rng.randrange(256) for _ in range(payload_len))
    app_off = segs[0][1]	# the application payload follows the SPI flash segment
    code = bytearray(rng.randrange(256) for _ in range(real["code"][1]))
    struct.pack_into("<I", code, 0, CODEC_SIG)
    tables = bytes(rng.randrange(256) for _ in range(real["tables"][1]))
    plain[app_off + real["code"][0] - APP_BASE:app_off + real["code"][0] - APP_BASE + len(code)] = code
    plain[app_off + real["tables"][0] - APP_BASE:app_off + real["tables"][0] - APP_BASE + len(tables)] = tables
    header = bytearray(HEADER_LEN)
    header[0:len(MAGIC)] = MAGIC
    struct.pack_into("<I", header, 0x7C, len(segs))
    for i, (a, n) in enumerate(segs):
        struct.pack_into("<II", header, 0x80 + 8 * i, a, n)
    enc = bytes(b ^ key[i % KEY_LEN] for i, b in enumerate(plain))
    stock = bytes(header) + enc + bytes(rng.randrange(256) for _ in range(0x100))	# trailer, as the real files
    prof = dict(real, code=(real["code"][0], real["code"][1], sha256(bytes(code))),
                tables=(real["tables"][0], real["tables"][1], sha256(tables)), size=len(stock))
    profiles = {sha256(stock): prof}

    check(parse_container(stock) == segs, "container segment table parsed")
    check(app_file_offset(segs, 0x08074714) == 0xC5C14, "code window file offset 0xC5C14 (spec)")
    check(app_file_offset(segs, 0x080D19F0) == 0x122EF0, "table window file offset 0x122EF0 (spec)")
    check((0xC5C14 - HEADER_LEN) % KEY_LEN == 0x314 and (0x122EF0 - HEADER_LEN) % KEY_LEN == 0x1F0,
          "cipher phases 0x314 and 0x1F0 (spec)")
    p, c, t = extract(stock, key, profiles)
    check(c == bytes(code) and t == tables, "windows decrypted")

    image = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(rng.randrange(256) for _ in range(5000))
    merged = merge(image, p, c, t)
    check(len(merged) == 0x080E4240 - APP_BASE, "merged image ends at the table window end 0x080E4240")
    check(merged[:len(image)] == image, "application unchanged")
    check(merged[len(image):0x08074714 - APP_BASE] == b"\xFF" * (0x08074714 - APP_BASE - len(image)),
          "0xFF up to the code window")
    check(merged[0x08074714 - APP_BASE:0x08084930 - APP_BASE] == c, "code at 0x08074714")
    check(merged[0x08084930 - APP_BASE:0x080D19F0 - APP_BASE] == b"\xFF" * (0x080D19F0 - 0x08084930),
          "0xFF between the windows")
    check(merged[0x080D19F0 - APP_BASE:] == t, "tables at 0x080D19F0")
    check(not stm32_dfu.check_image(merged, "dm1701"), "stm32_dfu accepts the merged image without warnings")

    bad_key = bytes([key[0] ^ 1]) + key[1:]
    expect_error(lambda: extract(stock, bad_key, profiles), "wrong key: window hash mismatch")
    expect_error(lambda: extract(stock[:-1] + b"\0", key, profiles), "other file: unsupported sha256")
    expect_error(lambda: extract(stock, key), "synthetic file is not the real stock file")
    expect_error(lambda: merge(b"\0" * (0x08074714 - APP_BASE + 1), p, c, t), "image overlapping the codec refused")
    expect_error(lambda: merge(stock, p, c, t), "official file as --image refused")
    expect_error(lambda: parse_container(b"x" * 300), "non-container refused")
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
    ap.add_argument("--stock", help="Baofeng DM-1701 stock firmware file (V02.06 CSV)")
    ap.add_argument("--key", help="DM-1701 firmware key file (1024 bytes)")
    ap.add_argument("--image", default=os.path.join(repo_root(), "build", "dm1701", "odr_dm1701.bin"),
                    help="built OpenDigitalRadio image (default: build/dm1701/odr_dm1701.bin)")
    ap.add_argument("-o", "--output", help="merged image to write (default: <image>_codec.bin next to the image)")
    ap.add_argument("--self-test", action="store_true", help="check the tool's logic on a synthetic file and key")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.stock or not args.key:
        ap.error("--stock and --key are required (or --self-test)")
    try:
        key = stm32_dfu.load_key(args.key, "dm1701")
    except stm32_dfu.DfuError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    out = args.output or os.path.splitext(args.image)[0] + "_codec.bin"
    try:
        with open(os.path.expanduser(args.stock), "rb") as f:
            stock = f.read()
        with open(os.path.expanduser(args.image), "rb") as f:
            image = f.read()
        check_output_path(out)
        prof, code, tables = extract(stock, key)
        merged = merge(image, prof, code, tables)
        for w in stm32_dfu.check_image(merged, "dm1701"):
            print("warning: %s" % w)
        with open(out, "wb") as f:
            f.write(merged)
    except (OSError, ExtractError, stm32_dfu.DfuError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    print("stock  %s (%s), codec code 0x%08X+0x%X and tables 0x%08X+0x%X verified"
          % (prof["name"], sha256(stock)[:8], prof["code"][0], prof["code"][1], prof["tables"][0], prof["tables"][1]))
    print("wrote  %s (%d bytes; contains the proprietary codec: keep it to yourself)" % (out, len(merged)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
