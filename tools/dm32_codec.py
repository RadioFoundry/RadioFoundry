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
    "dm32": {"slot": (0x030E8000, 0x03100000), "run": (0x00016CDC, 0x0002F914), "tables": (0x18000E48, 0x18003E28)},
    "dm1801pro": {"slot": (0x030E8000, 0x03100000), "run": (0x00016CDC, 0x0002B2DC),
                  "tables": (0x18000EF0, 0x18003E68)},
}
PROGMEM_BASE = 0x03000000   # a program flash dump starts here

# Entry points, offsets from the codec start (codec-dm32.md "Entry points"); fec_init differs per version
ENTRIES = ("decode", "encode", "dec_init", "dec_set", "enc_init", "enc_set", "fec_init")

# The 049 entry points: every DM-32UV stock build from 032 to 049 carries the same codec code, byte for byte, except
# for its table pointers (the NRF and fanti builds' tables sit 8 bytes higher) and stock code that lies inside the
# window but isn't codec (codec-dm32.md "Stock versions")
ENTRIES_049 = {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
               "enc_set": 0x2E4, "fec_init": 0x1BF8}

PROFILES = {
    "03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a": {
        "name": "DM32.01.01.049.bin", "board": "dm32", "tag": 49, "size": 869688,
        "code": (0x030C687C, 0x14600, 0x00016CDC),       # flash address, length, run address
        "tables": (0x03072628, 0x2F78, 0x18000EA8),
        "code_sha256": "355ca9061979eceb2991826da9d39c95b4a9d40466245923d541a6556ceeafd1",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
                    "enc_set": 0x2E4, "fec_init": 0x1BF8},
    },
    "f8484c13c994e920649d7b7bc2b98a98a76cfd0836865e5995a00627fc0c2887": {
        "name": "DM32.00.01.046HRVocoder.bin", "board": "dm32", "tag": 46, "size": 890516,
        "code": (0x030CB13C, 0x140DC, 0x0001B838),
        "tables": (0x030723A8, 0x2F78, 0x18000E48),
        "code_sha256": "02743cf4b3d252b8764fe083fb808b00ab42f0f0ab76935e62ca59fd5c5e64fb",
        "tables_sha256": "cd5a4d270f555cdbd586bca8de89e4e845789ed7aa2a2bb7414b6c3a270ef803",
        "entries": {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
                    "enc_set": 0x2E4, "fec_init": 0x1DCC},
    },
    "5ee2cb549792d5596fa50bb826613cde212e38f58b10b0084fdfd7b6032ec44d": {
        "name": "DM32.01.01.032.bin", "board": "dm32", "tag": 32, "size": 872708,
        "code": (0x030C74B4, 0x14600, 0x00016CDC),
        "tables": (0x03071908, 0x2F78, 0x18000EA8),
        "code_sha256": "cc17ee0bf3ee45d16912e44351a3434fc1ebcbc6e22ef3e42463e394624bb164",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "249bcd25a021bb5e384633232c3a2da5fb5cf0881d08fc4ab573faaf3a1fb28a": {
        "name": "DM32.01.01.037.bin", "board": "dm32", "tag": 37, "size": 867280,
        "code": (0x030C5F08, 0x14600, 0x00016CDC),
        "tables": (0x03071D68, 0x2F78, 0x18000EA8),
        "code_sha256": "6b1c46cb4bfefbbe9fe5d2d47d43624a4854fa4d52d5db29b75a5c968d68a065",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "101dd348b11b21ba67274d40e1b2cc8248863e442c7da0471bc9f7db09f61f6b": {
        "name": "DM32.01.01.039.bin", "board": "dm32", "tag": 39, "size": 868572,
        "code": (0x030C6414, 0x14600, 0x00016CDC),
        "tables": (0x03072228, 0x2F78, 0x18000EA8),
        "code_sha256": "6b1c46cb4bfefbbe9fe5d2d47d43624a4854fa4d52d5db29b75a5c968d68a065",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "016962d82b926e489e729abee46b13f97d223f8e97e07fe3b8bcba1dd1e37058": {
        "name": "DM32.01.01.040.bin", "board": "dm32", "tag": 40, "size": 868588,
        "code": (0x030C6424, 0x14600, 0x00016CDC),
        "tables": (0x03072238, 0x2F78, 0x18000EA8),
        "code_sha256": "6b1c46cb4bfefbbe9fe5d2d47d43624a4854fa4d52d5db29b75a5c968d68a065",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "ab21bc8dedb225a3e48c72e07af029fd0c453f00018f3b83a062fd0b9a3e3049": {
        "name": "DM32.01.01.046.bin", "board": "dm32", "tag": 46, "size": 868984,
        "code": (0x030C65B0, 0x14600, 0x00016CDC),
        "tables": (0x03072378, 0x2F78, 0x18000EA8),
        "code_sha256": "9720ec0c923ac32bc909d06a0d4c414c6a1e80c3eaa6e9c5b5df0d0bf5ff5665",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "7c045763c5b98c85836e932d9b4612f2c97fe67f5a4893ff564ecff27cfe1005": {
        "name": "DM32.01.01.047.bin", "board": "dm32", "tag": 47, "size": 869748,
        "code": (0x030C68B8, 0x14600, 0x00016CDC),
        "tables": (0x03072658, 0x2F78, 0x18000EA8),
        "code_sha256": "355ca9061979eceb2991826da9d39c95b4a9d40466245923d541a6556ceeafd1",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "fda860febfcf1a234eed7fa73272112891074aac83746e4f8dfe224a2a700f8f": {
        "name": "DM32.01.L01.048.bin", "board": "dm32", "tag": 48, "size": 860416,
        "code": (0x030C4444, 0x14600, 0x00016CDC),
        "tables": (0x03070468, 0x2F78, 0x18000EA8),
        "code_sha256": "cf275cd01ef8ba09c2873009ac5923dd5d2bc2384f6ceaa8b658b9d46d4478e2",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "1930cbe7c8ae173badff868f5e6f1f754f8601956965696d1de4ce48433cbc93": {
        "name": "DM32.01.02.046.bin", "board": "dm32", "tag": 46, "size": 868952,
        "code": (0x030C6590, 0x14600, 0x00016CDC),
        "tables": (0x03072358, 0x2F78, 0x18000EA8),
        "code_sha256": "9720ec0c923ac32bc909d06a0d4c414c6a1e80c3eaa6e9c5b5df0d0bf5ff5665",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "aff6b4dc09dcce45847173defba26e94157cc628af86826e21e5af227bdc52dd": {
        "name": "DM32.01.01.049 NRF build", "board": "dm32", "tag": 49, "size": 873964,
        "code": (0x030C7914, 0x14600, 0x00016CDC),
        "tables": (0x03072E80, 0x2F78, 0x18000EB0),
        "code_sha256": "cd0820a18df0b53bf011907a02d5b649bfbccf030c3d0432f4e363077d67a2c0",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "a8a5fc80e7116e8e8c9926f9c5e6d56a72689d70e09db3f0fec57b3a01ec4a31": {
        "name": "DM32.01.01.049 fanti build", "board": "dm32", "tag": 49, "size": 874028,
        "code": (0x030C7954, 0x14600, 0x00016CDC),
        "tables": (0x03072EC0, 0x2F78, 0x18000EB0),
        "code_sha256": "cd0820a18df0b53bf011907a02d5b649bfbccf030c3d0432f4e363077d67a2c0",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    # A whole program flash dump (1 MB from 0x03000000, e.g. stub/stubtool.py --progmem) holds the radio's own data
    # besides the firmware, so it's known by the SHA-256 of the code window (the key) and of the table window
    # (docs/spec/codec-dm1801pro.md 1)
    "8f7641b69a5cdfc946507ec48ddb67febe91d964520d5522bebd707f363f1a42": {
        "name": "DM-1801 Pro program flash (049 codec)", "board": "dm1801pro", "kind": "progmem", "tag": 49,
        "size": 0x100000,
        "code": (0x030A22F0, 0x14600, 0x00016CDC),
        "tables": (0x03068770, 0x2F78, 0x18000EF0),
        "code_sha256": "8f7641b69a5cdfc946507ec48ddb67febe91d964520d5522bebd707f363f1a42",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
                    "enc_set": 0x2E4, "fec_init": 0x1BF8},
    },
}

# CRC-32 of the first 64 bytes of every table window above: a cheap first test when locate() looks for the tables in a
# program flash dump, before the SHA-256. Not a codec byte, only a fingerprint
TABLES_HEAD_CRC = 0x11475389
PROGMEM_SIZE = 0x100000

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


def carve(stock, prof):
    """The code and table windows of a stock file, by the profile's kind."""
    windows = []
    for key in ("code", "tables"):
        addr, length, _ = prof[key]
        off = addr - PROGMEM_BASE if prof.get("kind") == "progmem" else file_offset(addr)
        if off < 0 or off + length > len(stock):
            raise ExtractError("%s window outside the file" % key)
        windows.append(stock[off:off + length])
    return windows


def locate(dump, profiles=None, board="dm32"):
    """A known codec anywhere in a program flash dump (1 MB from 0x03000000). Each stock version links the codec to its
    own flash addresses (DM-32 047 and 049 hold the same code, 0x3C bytes apart), so it is found by content: the code
    starts with the signature words and the tables by TABLES_HEAD_CRC, both 4-byte aligned, and each window must match
    a profile's SHA-256. Returns the profile with the addresses found (kind "progmem"), else None."""
    profiles = PROFILES if profiles is None else profiles
    profiles = [p for p in profiles.values() if p["board"] == board and "code_sha256" in p]
    if len(dump) != PROGMEM_SIZE or not profiles:
        return None
    sig = struct.pack("<2I", *CODEC_SIG)
    heads = None
    at = dump.find(sig)
    while at >= 0:
        # stock versions with the same codec bytes (037, 039, 040) are told apart by where they put it
        for p in sorted(profiles, key=lambda p: p["code"][0] != PROGMEM_BASE + at):
            clen, tlen = p["code"][1], p["tables"][1]
            if at % 4 or sha256(dump[at:at + clen]) != p["code_sha256"]:
                continue
            want = p.get("tables_head_crc", TABLES_HEAD_CRC)
            if heads is None:
                heads = {}
                for i in range(0, len(dump) - 64, 4):
                    heads.setdefault(zlib.crc32(dump[i:i + 64]), []).append(i)
            for t in heads.get(want, []):
                if sha256(dump[t:t + tlen]) == p["tables_sha256"]:
                    return dict(p, kind="progmem", size=PROGMEM_SIZE, code=(PROGMEM_BASE + at, clen, p["code"][2]),
                                tables=(PROGMEM_BASE + t, tlen, p["tables"][2]))
        at = dump.find(sig, at + 1)
    return None


def identify(stock, profiles=None, board="dm32"):
    """The profile a stock file matches (whole-file SHA-256, or a program flash dump by its windows), else None."""
    profiles = PROFILES if profiles is None else profiles
    mine = {h: p for h, p in profiles.items() if p["board"] == board}
    prof = mine.get(sha256(stock))
    if prof is not None and prof.get("kind") != "progmem":
        return prof
    return locate(stock, mine, board)


def parse_slot(slot):
    """The header of a codec slot read back from a radio, checked: magic, version, windows inside the slot and the CRC
    of code then tables. Returns a dict of its fields, else None (an empty or damaged slot)."""
    if len(slot) < HEADER_LEN:
        return None
    f = struct.unpack_from("<8I7I2I", slot, 0)
    magic, version, code_off, code_len, run, tab_off, tab_len, tab_ram = f[:8]
    if magic != SLOT_MAGIC or version != SLOT_VERSION:
        return None
    if code_off < HEADER_LEN or code_off + code_len > len(slot) or tab_off + tab_len > len(slot) or not code_len:
        return None
    code, tables = slot[code_off:code_off + code_len], slot[tab_off:tab_off + tab_len]
    if zlib.crc32(tables, zlib.crc32(code)) & 0xFFFFFFFF != f[16] or struct.unpack_from("<2I", code, 0) != CODEC_SIG:
        return None
    return {"run": run, "tables_ram": tab_ram, "tag": f[15], "crc": f[16], "length": tab_off + tab_len,
            "code_sha256": sha256(code), "tables_sha256": sha256(tables)}


def extract(stock, profiles=None, board="dm32"):
    """Check the stock file and return (profile, code bytes, table bytes)."""
    profiles = PROFILES if profiles is None else profiles
    profiles = {h: p for h, p in profiles.items() if p["board"] == board}
    digest = sha256(stock)
    prof = identify(stock, profiles, board)
    if prof is None:
        raise ExtractError("unsupported stock file (sha256 %s); supported: %s"
                           % (digest, ", ".join("%s (%s...)" % (p["name"], h[:8]) for h, p in profiles.items())))
    if prof.get("kind") != "progmem" and not stock.startswith(MAGIC):
        raise ExtractError("not a BFUV32-V2 firmware file")
    code, tables = carve(stock, prof)
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

    # A program flash dump (DM-1801 Pro): known by its windows, whatever else the dump holds
    pm = PROFILES["8f7641b69a5cdfc946507ec48ddb67febe91d964520d5522bebd707f363f1a42"]
    dump = bytearray(os.urandom(pm["size"]))
    pcode = bytearray(os.urandom(pm["code"][1]))
    struct.pack_into("<2I", pcode, 0, *CODEC_SIG)
    ptab = os.urandom(pm["tables"][1])
    co, to = pm["code"][0] - PROGMEM_BASE, pm["tables"][0] - PROGMEM_BASE
    dump[co:co + len(pcode)] = pcode
    dump[to:to + len(ptab)] = ptab
    pprof = dict(pm, code_sha256=sha256(bytes(pcode)), tables_sha256=sha256(ptab), tables_head_crc=zlib.crc32(ptab[:64]))
    pprofiles = {sha256(bytes(pcode)): pprof}
    q, qc, qt = extract(bytes(dump), pprofiles, board="dm1801pro")
    check(qc == bytes(pcode) and qt == ptab, "program flash dump: windows carved at their addresses")
    dump2 = bytearray(dump)
    dump2[0x100] ^= 0xFF    # the radio's own data elsewhere in the dump doesn't matter
    check(identify(bytes(dump2), pprofiles, board="dm1801pro")["code"][0] == pm["code"][0], "program flash dump: other bytes ignored")
    moved = bytearray(os.urandom(pm["size"]))    # another stock version: the same codec at other addresses
    moved[0xC1234 & ~3:(0xC1234 & ~3) + len(pcode)] = pcode
    moved[0x70010:0x70010 + len(ptab)] = ptab
    found = locate(bytes(moved), pprofiles, board="dm1801pro")
    check(found and found["code"][0] == PROGMEM_BASE + (0xC1234 & ~3) and found["tables"][0] == PROGMEM_BASE + 0x70010,
          "program flash dump: the codec found wherever the stock version put it")
    check(locate(bytes(moved[:-1]), pprofiles, board="dm1801pro") is None, "locate wants a whole program flash")
    slot = merge(b"\x22" * 64, found, *carve(bytes(moved), found))[BOARDS["dm1801pro"]["slot"][0] - APP_BASE:]
    ps = parse_slot(slot)
    check(ps and ps["code_sha256"] == pprof["code_sha256"] and ps["tag"] == 49, "parse_slot reads a slot back")
    broken = bytearray(slot)
    broken[HEADER_LEN + 500] ^= 1
    check(parse_slot(bytes(broken)) is None and parse_slot(b"\xff" * 4096) is None, "parse_slot refuses a damaged or empty slot")
    qs = merge(b"\x22" * 64, q, qc, qt)[BOARDS["dm1801pro"]["slot"][0] - APP_BASE:]
    g = struct.unpack_from("<8I7I2I", qs, 0)
    check(g[4] == 0x16CDC and g[7] == 0x18000EF0 and g[14] == 0x1BF8, "program flash dump: slot header")
    dump2[co + 100] ^= 1
    expect_error(lambda: extract(bytes(dump2), pprofiles, board="dm1801pro"), "program flash dump: changed code")
    expect_error(lambda: extract(bytes(dump), pprofiles, board="dm32"), "program flash dump: other radio")
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
    ap.add_argument("--stock", help="your radio's stock firmware file (DM-32UV: 049 or 046; DM-1801 Pro: a program "
                    "flash dump, stub/stubtool.py --progmem)")
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
