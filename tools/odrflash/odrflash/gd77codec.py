# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""GD-77: place the radio's own AMBE+2 codec where ODR wants it (docs/spec/codec-gd77.md).

The codec sits in three parts of the radio's stock firmware: a low block (0x4400, the fixed-point helpers), a main block
(about 69 KB, with a 13 KB stretch of the stock firmware's own code inside it) and a table window (about 78 KB, with two
small code islands) near the top of the stock image. ODR needs the low block's place and a contiguous region for its own
code, so the merge:

  * moves the main block to CODE_BASE and the low block after it,
  * leaves the table window at its stock address (the code reaches its tables through absolute addresses),
  * rewrites the 3 to 6 words that hold a code address (a spec lists them) and the 171 branches that cross between the
    parts (the spec lists those too: each is re-encoded for the new distance),
  * writes a descriptor at DESC_ADDR (platform/k22f/codec_desc.h) that names the code region and the table window with
    their CRC-32.

The spec for each supported stock build is codec_packs/gd77/<profile>.json (format odr-codec-pack/1): addresses, lengths
and hashes, never codec bytes. The input is the stock firmware as a flat flash image (flash address = file offset +
FLASH_BASE). Nothing here is part of a release; the bytes come from the user's own file and go to the user's radio.
"""
import glob
import hashlib
import json
import os
import struct
import zlib

FLASH_BASE = 0x4000			# flat image offset 0 is this flash address
CODE_BASE = 0x00055D00			# where the main block goes (boards/gd77/gd77.ld ends the application here)
DESC_ADDR = 0x0007B780			# the descriptor (platform/k22f/codec_desc.h)
DESC_LEN = 0x80
DESC_MAGIC = 0x43534544			# "DESC"
DESC_VERSION = 1
TABLE_LO = 0x00067C58			# the lowest table window start of any supported build
TABLE_HI = 0x0007B5F8			# the highest end
MAPS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "codec_packs", "gd77")
FORMAT = "odr-codec-pack/1"
# The codec as the merge places it, per supported build: the descriptor's tag, the code region's length and CRC-32 and the
# table window's CRC-32 after the rewrites. boards/gd77/vocoder_gd77.c pins the same numbers (a test compares them)
PLACED = {
    "gd77-v3.2.1": {"tag": 0x3201, "code_len": 0x11E90, "code_crc": 0xBC78498B, "tab_crc": 0xE81AFF5D},
    "gd77-v3.2.2": {"tag": 0x3202, "code_len": 0x11E90, "code_crc": 0xDBBA763D, "tab_crc": 0x4051B25A},
    "gd77-v4.2.8": {"tag": 0x4208, "code_len": 0x11E90, "code_crc": 0x1CA7B3D4, "tab_crc": 0xAF1FEB2E},
    "gd77-v4.3.3": {"tag": 0x4303, "code_len": 0x11B58, "code_crc": 0xC5EB3113, "tab_crc": 0xB9AE6A44},
    "gd77-v4.3.6": {"tag": 0x4306, "code_len": 0x11B58, "code_crc": 0x8BBE7F89, "tab_crc": 0xDCEC0213},
    "gd77-v4.3.9": {"tag": 0x4309, "code_len": 0x11B58, "code_crc": 0x9F4B97AC, "tab_crc": 0xC59EC61F},
    "gd77-v6.0.1": {"tag": 0x6001, "code_len": 0x11B58, "code_crc": 0x28E1981D, "tab_crc": 0xBBC2BCA7},
}
CODEC_ID = "gd77-p1"			# what the firmware reports for a build placed as above (debug link VERSION codec=)
ENTRY_NAMES = ("encode", "decode", "enc_init", "fec_init", "dec_init", "enc_params", "enc_set", "dec_set")


class PackError(Exception):
    pass


def sha256(b):
    return hashlib.sha256(bytes(b)).hexdigest()


def crc32(b):
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def _n(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def load_specs(directory=MAPS_DIR):
    """{profile: spec} with every number as an int."""
    out = {}
    for p in sorted(glob.glob(os.path.join(directory, "*.json"))):
        with open(p) as f:
            j = json.load(f)
        if j.get("format") != FORMAT:
            raise PackError("%s: not a %s file" % (p, FORMAT))
        s = {"profile": j["profile"], "stock": j["stock"], "entries": {k: _n(v) for k, v in j["entries"].items()},
             "words": [(_n(a), _n(v)) for a, v in j["words"]], "branches": [(_n(a), _n(t)) for a, t in j["branches"]]}
        for part in ("main", "low", "tab"):
            s[part] = {"flash": _n(j[part]["flash"]), "len": j[part]["len"], "sha256": j[part]["sha256"]}
        if s["profile"] in out:
            raise PackError("two specs for profile %s" % s["profile"])
        out[s["profile"]] = s
    return out


def find_spec(image, specs=None):
    """The spec whose three windows hash to what the flat stock image holds, or None."""
    for s in (specs or load_specs()).values():
        if all(_window(image, s[p]) is not None and sha256(_window(image, s[p])) == s[p]["sha256"]
               for p in ("main", "low", "tab")):
            return s
    return None


def _window(image, w):
    lo = w["flash"] - FLASH_BASE
    return image[lo:lo + w["len"]] if lo >= 0 and lo + w["len"] <= len(image) else None


def _decode_branch(h1, h2):
    s = (h1 >> 10) & 1
    i10 = h1 & 0x3FF
    j1 = (h2 >> 13) & 1
    j2 = (h2 >> 11) & 1
    i11 = h2 & 0x7FF
    i1 = ~(j1 ^ s) & 1
    i2 = ~(j2 ^ s) & 1
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | (i10 << 12) | (i11 << 1)
    return imm - (1 << 25) if s else imm


def _encode_branch(imm, h2_old):
    s = 1 if imm < 0 else 0
    v = imm & 0x1FFFFFF
    i1 = (v >> 23) & 1
    i2 = (v >> 22) & 1
    h1 = 0xF000 | (s << 10) | ((v >> 12) & 0x3FF)
    h2 = (h2_old & 0xD000) | (((~(i1 ^ s)) & 1) << 13) | (((~(i2 ^ s)) & 1) << 11) | ((v >> 1) & 0x7FF)
    return h1, h2


class Packed:
    """The result of pack(): bytes for the code region at `code_addr`, the patched table window at `tab_addr`."""

    def __init__(self, spec, code_addr, code, tab_addr, tab, main_new, low_new):
        self.spec, self.code_addr, self.code, self.tab_addr, self.tab = spec, code_addr, code, tab_addr, tab
        self.main_new, self.low_new = main_new, low_new

    @property
    def entries(self):
        """Entry points as offsets from the code region start (the main block starts it)."""
        return {k: self.spec["entries"][k] for k in ENTRY_NAMES}

    def descriptor(self, tag):
        words = [DESC_MAGIC, DESC_VERSION, tag, self.code_addr, len(self.code), crc32(self.code), 1,
                 self.tab_addr, len(self.tab), crc32(self.tab)] + [0] * 12
        body = struct.pack("<%dI" % len(words), *words)
        body += b"\0" * (DESC_LEN - 4 - len(body))
        return body + struct.pack("<I", crc32(body))

    def segments(self, tag=None):
        """[(flash address, bytes)] the merge writes: the code region, the table window, the descriptor."""
        tag = PLACED[self.spec["profile"]]["tag"] if tag is None else tag
        return [(self.code_addr, self.code), (self.tab_addr, self.tab), (DESC_ADDR, self.descriptor(tag))]


def pack(image, spec, code_base=CODE_BASE):
    """Move the codec of a flat stock image (spec: one load_specs() entry) and patch what the move changes."""
    parts = {}
    for p in ("main", "low", "tab"):
        w = _window(image, spec[p])
        if w is None or sha256(w) != spec[p]["sha256"]:
            raise PackError("the %s window of this stock firmware is not what the %s spec describes" % (p, spec["profile"]))
        parts[p] = bytearray(w)
    main, low, tab = spec["main"], spec["low"], spec["tab"]
    if tab["flash"] < TABLE_LO or tab["flash"] + tab["len"] > TABLE_HI:
        raise PackError("the table window is outside the range the board reserves")
    main_new = code_base + ((main["flash"] - code_base) % 4)
    low_new = main_new + main["len"]
    low_new += (low["flash"] - low_new) % 4
    end = low_new + low["len"]
    if end > tab["flash"]:
        raise PackError("the moved code (0x%X..0x%X) reaches the table window at 0x%X" % (code_base, end, tab["flash"]))
    code = bytearray(b"\xFF" * (end - code_base))
    code[main_new - code_base:main_new - code_base + main["len"]] = parts["main"]
    code[low_new - code_base:low_new - code_base + low["len"]] = parts["low"]

    def new_addr(x):
        if main["flash"] <= x < main["flash"] + main["len"]:
            return main_new + x - main["flash"]
        if low["flash"] <= x < low["flash"] + low["len"]:
            return low_new + x - low["flash"]
        return x

    def where(x):
        """(buffer, offset) of the patched copy of old address x"""
        if main["flash"] <= x < main["flash"] + main["len"]:
            return code, main_new - code_base + x - main["flash"]
        if low["flash"] <= x < low["flash"] + low["len"]:
            return code, low_new - code_base + x - low["flash"]
        if tab["flash"] <= x < tab["flash"] + tab["len"]:
            return parts["tab"], x - tab["flash"]
        raise PackError("address 0x%X is in no codec window" % x)

    for at, value in spec["words"]:
        buf, off = where(at)
        if struct.unpack_from("<I", buf, off)[0] != value:
            raise PackError("the word at 0x%X is not 0x%X" % (at, value))
        struct.pack_into("<I", buf, off, new_addr(value & ~1) | (value & 1))
    for at, target in spec["branches"]:
        buf, off = where(at)
        h1, h2 = struct.unpack_from("<HH", buf, off)
        if (h1 & 0xF800) != 0xF000 or (h2 & 0xD000) not in (0xD000, 0x9000) or at + 4 + _decode_branch(h1, h2) != target:
            raise PackError("no branch to 0x%X at 0x%X" % (target, at))
        h1n, h2n = _encode_branch(new_addr(target) - (new_addr(at) + 4), h2)
        struct.pack_into("<HH", buf, off, h1n, h2n)
    return Packed(spec, code_base, bytes(code), tab["flash"], bytes(parts["tab"]), main_new, low_new)


def merge(app_image, packed, tag=None):
    """The flat image (from FLASH_BASE) with the application and the codec segments, 0xFF between."""
    if FLASH_BASE + len(app_image) > packed.code_addr:
        raise PackError("the application image ends at 0x%08X, inside the codec at 0x%08X: rebuild with the window "
                        "reserved (boards/gd77/gd77.ld)" % (FLASH_BASE + len(app_image), packed.code_addr))
    segs = packed.segments(tag)
    end = max(a + len(d) for a, d in segs)
    out = bytearray(b"\xFF" * (end - FLASH_BASE))
    out[:len(app_image)] = app_image
    for a, d in segs:
        out[a - FLASH_BASE:a - FLASH_BASE + len(d)] = d
    return bytes(out)
