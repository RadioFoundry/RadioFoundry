#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Merge an HR_C7000 radio's own stock AMBE+2 codec into an OpenDigitalRadio image, on your machine.

The radios differ only in where things are (BOARDS: the slot and the windows its linker script reserves; PROFILES: where
a stock file keeps the codec). For the DM-32UV, the recipe from docs/spec/codec-dm32.md ("Where the codec is",
"Recommended hosting"):
  1. check the stock file (Baofeng DM32.01.01.049.bin or DM32.00.01.046HRVocoder.bin, "BFUV32-V2" header) by SHA-256;
  2. carve the codec code window and the codec table window (file offset = address - 0x0300C000 + 0x100);
  3. check the code's first two words (0x152514D8, 0x74C776CD);
  4. write a new raw image: the built application from 0x0300C000, 0xFF padding, then the codec slot at 0x030E8000:
     a 128-byte header (platform/hrc7000/vocoder.c, codec_slot_t), the code, the tables. At boot the firmware copies
     them to their stock RAM addresses; nothing is relocated.

The codec is DVSI's and Baofeng's, not ours: it is never stored in this repository or in a release. This tool reads
your stock file and writes the merged image to a file you choose (by default next to the built image, in build/, which
git ignores). Wrap the result for the Baofeng updater with tools/dm32_pack.py.
"""
import argparse
import hashlib
import os
import struct
import sys
import zlib

APP_BASE = 0x0300C000
STOCK_HEADER = 0x100
MAGIC = b"BFUV32-V2"
SLOT_MAGIC = 0x43524444  # "DDRC"
SLOT_VERSION = 1
HEADER_LEN = 128
CODEC_SIG = (0x152514D8, 0x74C776CD)

# Per radio: the codec slot in program flash, the RAM window its linker script leaves to the codec and the modem SRAM
# table window (the __codec_* symbols of boards/<radio>/<radio>.ld; keep the two in step)
BOARDS = {
    "dm32": {"slot": (0x030E8000, 0x03100000), "run": (0x00016CDC, 0x0002F914), "tables": (0x18000E48, 0x18003E20)},
}

# Entry points, offsets from the codec start (codec-dm32.md "Entry points"); fec_init differs per version
ENTRIES = ("decode", "encode", "dec_init", "dec_set", "enc_init", "enc_set", "fec_init")

PROFILES = {
    "03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a": {
        "name": "DM32.01.01.049.bin", "board": "dm32", "tag": 49, "size": 869688,
        "code": (0x030C687C, 0x14600, 0x00016CDC),       # flash address, length, run address
        "tables": (0x03072628, 0x2F78, 0x18000EA8),
        "entries": {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
                    "enc_set": 0x2E4, "fec_init": 0x1BF8},
    },
    "f8484c13c994e920649d7b7bc2b98a98a76cfd0836865e5995a00627fc0c2887": {
        "name": "DM32.00.01.046HRVocoder.bin", "board": "dm32", "tag": 46, "size": 890516,
        "code": (0x030CB13C, 0x140DC, 0x0001B838),
        "tables": (0x030723A8, 0x2F78, 0x18000E48),
        "entries": {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
                    "enc_set": 0x2E4, "fec_init": 0x1DCC},
    },
}

EPILOG = """\
examples:
  %(prog)s --stock DM32.01.01.049.bin
  %(prog)s --stock DM32.01.01.049.bin --image build/dm32/odr_dm32.bin -o build/dm32/odr_dm32_codec.bin
  %(prog)s --self-test

then:  tools/dm32_pack.py build/dm32/odr_dm32_codec.bin build/dm32/odr_dm32_codec_BFUV32.bin
       and load that file with the Baofeng updater (docs/testing/dm32-bringup.md)
"""


class ExtractError(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_offset(addr):
    return addr - APP_BASE + STOCK_HEADER


def extract(stock, profiles=None, board="dm32"):
    """Check the stock file and return (profile, code bytes, table bytes)."""
    profiles = PROFILES if profiles is None else profiles
    profiles = {h: p for h, p in profiles.items() if p["board"] == board}
    digest = sha256(stock)
    prof = profiles.get(digest)
    if prof is None:
        raise ExtractError("unsupported stock file (sha256 %s); supported: %s"
                           % (digest, ", ".join("%s (%s...)" % (p["name"], h[:8]) for h, p in profiles.items())))
    if not stock.startswith(MAGIC):
        raise ExtractError("not a BFUV32-V2 firmware file")
    windows = []
    for key in ("code", "tables"):
        addr, length, _ = prof[key]
        off = file_offset(addr)
        if off < STOCK_HEADER or off + length > len(stock):
            raise ExtractError("%s window outside the file" % key)
        windows.append(stock[off:off + length])
    code, tables = windows
    if struct.unpack_from("<2I", code, 0) != CODEC_SIG:
        raise ExtractError("codec signature words not found at the code window")
    return prof, code, tables


def slot_bytes(prof, code, tables):
    """The codec slot contents: header, code, tables (4-byte aligned)."""
    lay = BOARDS[prof["board"]]
    code_off = HEADER_LEN
    tab_off = code_off + ((len(code) + 3) & ~3)
    run = prof["code"][2]
    tab_ram = prof["tables"][2]
    if not (lay["run"][0] <= run and run + len(code) <= lay["run"][1]):
        raise ExtractError("code run address outside the reserved RAM window")
    if not (lay["tables"][0] <= tab_ram and tab_ram + len(tables) <= lay["tables"][1]):
        raise ExtractError("table address outside the reserved modem SRAM window")
    crc = zlib.crc32(tables, zlib.crc32(code)) & 0xFFFFFFFF
    entries = [prof["entries"][k] for k in ENTRIES]
    header = struct.pack("<8I7I2I", SLOT_MAGIC, SLOT_VERSION, code_off, len(code), run, tab_off, len(tables), tab_ram,
                         *entries, prof["tag"], crc)
    header += b"\0" * (HEADER_LEN - len(header))
    body = header + code + b"\xff" * (tab_off - code_off - len(code)) + tables
    if len(body) > lay["slot"][1] - lay["slot"][0]:
        raise ExtractError("codec does not fit the slot")
    return body


def merge(image, prof, code, tables):
    slot_off = BOARDS[prof["board"]]["slot"][0] - APP_BASE
    if len(image) > slot_off:
        raise ExtractError("the application image (%d bytes) runs into the codec slot at +0x%X" % (len(image), slot_off))
    return image + b"\xff" * (slot_off - len(image)) + slot_bytes(prof, code, tables)


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def check_output_path(path):
    """Refuse to write a merged image anywhere git could pick it up, outside build/."""
    root = repo_root()
    full = os.path.abspath(path)
    if full.startswith(root + os.sep) and not full.startswith(os.path.join(root, "build") + os.sep):
        raise ExtractError("write the merged image under build/ (ignored by git), not %s" % path)


def self_test():
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL", msg)
            ok = False

    real = PROFILES["03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a"]
    # A synthetic stock file laid out like the real one: header, zeros, code and tables at their file offsets
    size = real["size"]
    stock = bytearray(size)
    stock[:len(MAGIC)] = MAGIC
    stock[0xFD:0x100] = b"END"
    caddr, clen, _ = real["code"]
    taddr, tlen, _ = real["tables"]
    code = bytearray(os.urandom(clen))
    struct.pack_into("<2I", code, 0, *CODEC_SIG)
    tables = os.urandom(tlen)
    stock[file_offset(caddr):file_offset(caddr) + clen] = code
    stock[file_offset(taddr):file_offset(taddr) + tlen] = tables
    stock = bytes(stock)
    profiles = {sha256(stock): dict(real)}
    p, c, t = extract(stock, profiles)
    check(c == bytes(code) and t == tables, "windows carved at the right offsets")
    image = b"\x11" * 1000
    out = merge(image, p, c, t)
    slot_base, slot_end = BOARDS["dm32"]["slot"]
    slot = out[slot_base - APP_BASE:]
    check(out[:1000] == image and out[1000] == 0xFF, "image then padding")
    f = struct.unpack_from("<8I7I2I", slot, 0)
    check(f[0] == SLOT_MAGIC and f[1] == SLOT_VERSION, "header magic and version")
    check(f[2] == HEADER_LEN and f[3] == clen and f[4] == 0x16CDC, "code window")
    check(f[6] == tlen and f[7] == 0x18000EA8, "table window")
    check(f[8:15] == (0, 0x260, 0xB4, 0x84, 0x314, 0x2E4, 0x1BF8), "entry points")
    check(f[15] == 49, "stock tag")
    check(slot[f[2]:f[2] + clen] == bytes(code) and slot[f[5]:f[5] + tlen] == tables, "code and tables in the slot")
    check(f[16] == zlib.crc32(tables, zlib.crc32(bytes(code))) & 0xFFFFFFFF, "CRC of code then tables")
    check(len(slot) <= slot_end - slot_base, "fits the slot")

    def expect_error(fn, msg):
        try:
            fn()
        except ExtractError:
            return
        check(False, msg)

    expect_error(lambda: extract(stock[:-1] + b"\1", profiles), "other file: unsupported sha256")
    expect_error(lambda: extract(stock, profiles, board="other"), "another radio's stock file")
    bad = bytearray(stock)
    bad[file_offset(caddr)] ^= 1
    bad = bytes(bad)
    expect_error(lambda: extract(bad, {sha256(bad): dict(real)}), "signature check")
    expect_error(lambda: merge(b"\0" * (slot_base - APP_BASE + 1), p, c, t), "image too big")
    expect_error(lambda: check_output_path(os.path.join(repo_root(), "codec.bin")), "output outside build/")
    print("dm32_codec self-test: %s" % ("all passed" if ok else "FAILED"))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog=EPILOG,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default="dm32", choices=sorted(BOARDS), help="the radio (default: dm32)")
    ap.add_argument("--stock", help="your radio's stock firmware file (DM-32UV: 049 or 046)")
    ap.add_argument("--image", help="the built raw application image (default: build/<board>/odr_<board>.bin)")
    ap.add_argument("-o", "--output", help="merged image (default: <image>_codec.bin)")
    ap.add_argument("--self-test", action="store_true", help="run the built-in checks on synthetic data")
    args = ap.parse_args()
    if args.self_test:
        sys.exit(0 if self_test() else 1)
    if not args.stock:
        ap.error("--stock is required")
    args.image = args.image or os.path.join(repo_root(), "build", args.board, "odr_%s.bin" % args.board)
    out = args.output or os.path.splitext(args.image)[0] + "_codec.bin"
    try:
        check_output_path(out)
        prof, code, tables = extract(open(args.stock, "rb").read(), board=args.board)
        merged = merge(open(args.image, "rb").read(), prof, code, tables)
    except (ExtractError, OSError) as e:
        sys.exit("dm32_codec: %s" % e)
    with open(out, "wb") as f:
        f.write(merged)
    print("dm32_codec: %s codec (%d + %d bytes) merged into %s" % (prof["name"], len(code), len(tables), out))


if __name__ == "__main__":
    main()
