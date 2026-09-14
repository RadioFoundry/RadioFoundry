# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The DM-1701 stock firmware file as a library: container, decryption, codec extraction and merge, key derivation.

Recipe from docs/spec/codec-dm1701.md ("Container and encryption", "Extraction recipe (V02.06 CSV)"). Used by
odrflash (merge from a stock file) and by tools/codec_extract.py (the command line merge tool).

The codec is DVSI's and Baofeng's, not ours: nothing here stores it; it is read from the user's file on the user's
machine and only ever written to a file or a radio the user chooses.
"""
import collections
import hashlib
import itertools
import struct

APP_BASE = 0x0800C000
FLASH_END = 0x08100000
KEY_LEN = 1024
HEADER_LEN = 0x100
MAGIC = b"OutSecurityBin"
CODEC_SIG = 0x4FF0E92D

# DM-1701 bootloader key identity (docs/spec/hardware-dm1701.md 15.3); the key bytes are never stored here
DM1701_KEY_SHA256 = "d983ce95fbc19fc78b913d9afc0381e3ba4ffedb480798865292d05a2fb3deff"
# MD-380 / MD-390 / RT3 (docs/spec/hardware-md380.md 14.2, as tools/stm32_dfu.py's md380 model)
MD380_KEY_SHA256 = "5254139c6b596d9413d106e6b7c0685ec245d69555cb28df98f061d6b879c4cd"
# The count word of the single-segment container form (the MD-380 D002.032 file: one segment, count 0xFFFFFFFF)
SINGLE_SEGMENT = 0xFFFFFFFF

# One profile per supported stock file, keyed by its SHA-256 (codec-dm1701.md, "Stock files" and "Codec in the stock
# image"). Window = (flash address, length, SHA-256 of the decrypted bytes). "codec_profile" is the id the firmware
# reports (debug link VERSION codec=) and the release manifests carry.
PROFILES = {
    "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d": {
        "name": "DM-1701(CSV)-V02.06.bin",
        "version": "V02.06 CSV",
        "codec_profile": "dm1701-v0206-csv",
        "size": 1272320,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xD9400)],
        "code": (0x08074714, 0x1021C, "1d8d04603b288e102f70252c1dd489600dfde656eccea6264e7bf4fac413f0cd"),
        "tables": (0x080D19F0, 0x12850, "b99f14e72bf72555946c1f04e8a1636652e6f62843baaf7a135787879e028894"),
    },
}

# Stock files that are not codec donors but are known DM-1701 files: the key can be derived from them (any of them;
# codec-dm1701.md "Stock files"), and they tell the user which version they hold
KNOWN_FILES = {
    "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d": "DM-1701 V02.06 CSV",
    "4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9": "DM-1701 V02.06 noCSV",
    "35a045d7a71164183b164d6ecfdd9e349c757b3539be67a16fecd68ca6f65948": "DM-1701 V02.03 CSV",
    "0a23d563fba04a1874c1e4eb4c0c23bf86e1d17da37cd3681f06ea71ad9c800a": "DM-1701 V02.03 without CSV",
    "643650e821db3adf3e0b203437fcdd776d6a7754bc01c0359bd30785ff2478f7": "DM-1701 V02.02 CSV",
    "cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713": "DM-1701 V02.01 CSV",
}


class StockError(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_container(data):
    """Return the segment list [(address, length), ...] of an OutSecurityBin file."""
    if len(data) < HEADER_LEN or not data.startswith(MAGIC):
        raise StockError("not a TYT/Baofeng OutSecurityBin firmware file (no \"OutSecurityBin\" header)")
    count = struct.unpack_from("<I", data, 0x7C)[0]
    if count == SINGLE_SEGMENT:
        count = 1	# the TYT MD-380 files (docs/spec/hardware-md380.md 14.1): one segment in the first table entry
    if not 1 <= count <= 15:
        raise StockError("implausible segment count %d" % count)
    segs = [struct.unpack_from("<II", data, 0x80 + 8 * i) for i in range(count)]
    total = sum(length for _, length in segs)
    if HEADER_LEN + total > len(data):	# the V02.06 files carry 0x100 more bytes after the last segment
        raise StockError("segment lengths (0x%X) run past the end of the file (0x%X)" % (total, len(data)))
    return segs


def app_file_offset(segs, addr):
    """File offset of flash address addr inside the application segment."""
    off = HEADER_LEN
    for base, length in segs:
        if base <= addr < base + length:
            return off + addr - base
        off += length
    raise StockError("address 0x%08X is in no segment" % addr)


def decrypt(data, off, length, key):
    """plain[i] = file[off + i] XOR key[(off + i - 0x100) mod 1024]"""
    if off < HEADER_LEN or off + length > len(data):
        raise StockError("window 0x%X+0x%X is outside the payload" % (off, length))
    return bytes(b ^ key[(off + i - HEADER_LEN) % KEY_LEN] for i, b in enumerate(data[off:off + length]))


def identify(stock):
    """A short description of a stock file ("DM-1701 V02.06 CSV"), or None when unknown."""
    return KNOWN_FILES.get(sha256(stock))


def extract(stock, key, profiles=None):
    """Check the stock file and return (profile, code bytes, table bytes)."""
    profiles = PROFILES if profiles is None else profiles
    digest = sha256(stock)
    prof = profiles.get(digest)
    if prof is None:
        known = KNOWN_FILES.get(digest)
        raise StockError("%s (sha256 %s...), not a supported codec source. Supported: %s"
                         % ("this file is " + known if known else "unknown stock file", digest[:16],
                            ", ".join("%s (%s...)" % (p["name"], h[:8]) for h, p in profiles.items())))
    if len(stock) != prof["size"]:
        raise StockError("size %d, expected %d" % (len(stock), prof["size"]))
    segs = parse_container(stock)
    if segs != prof["segments"]:
        raise StockError("segment table %s, expected %s" % (segs, prof["segments"]))
    out = []
    for which in ("code", "tables"):
        addr, length, want = prof[which]
        plain = decrypt(stock, app_file_offset(segs, addr), length, key)
        got = sha256(plain)
        if got != want:
            raise StockError("decrypted %s window sha256 %s, expected %s (wrong key?)" % (which, got, want))
        out.append(plain)
    code, tables = out
    if struct.unpack_from("<I", code, 0)[0] != CODEC_SIG:
        raise StockError("codec first word is not 0x%08X" % CODEC_SIG)
    return prof, code, tables


def codec_segments(prof, code, tables):
    """The codec as flash segments [(address, bytes), ...] for odrflash's planner."""
    return [(prof["code"][0], code), (prof["tables"][0], tables)]


def merge(image, prof, code, tables):
    """The built application image (plain, from 0x0800C000) with both windows written at their stock addresses, as
    one contiguous image (0xFF between the parts)."""
    if image.startswith(MAGIC):
        raise StockError("the image is an official firmware file; pass the OpenDigitalRadio build (odr_dm1701.bin)")
    caddr, clen, _ = prof["code"]
    taddr, tlen, _ = prof["tables"]
    if APP_BASE + len(image) > caddr:
        raise StockError("the application image ends at 0x%08X, inside the codec code window at 0x%08X: rebuild "
                         "with the window reserved (boards/dm1701/dm1701.ld)" % (APP_BASE + len(image), caddr))
    end = taddr + tlen
    if end > FLASH_END:
        raise StockError("merged image would pass the end of flash")
    out = bytearray(b"\xFF" * (end - APP_BASE))
    out[0:len(image)] = image
    out[caddr - APP_BASE:caddr - APP_BASE + clen] = code
    out[taddr - APP_BASE:taddr - APP_BASE + tlen] = tables
    return bytes(out)


def derive_key(stock, want_sha256=DM1701_KEY_SHA256, max_alternatives=2, candidates=24):
    """Recover the 1024-byte bootloader key from a stock file, checked against its published SHA-256.

    The payload is plain XOR key[i mod 1024] (codec-dm1701.md "Container and encryption"), and the most common plain
    byte in every key column of a firmware image is 0x00 (erased-to-zero data, padding, zero-filled tables), so the
    most common cipher byte of column j is key[j] for nearly every j. The handful of columns where that guess is
    wrong are found by trying the next most common bytes in the least confident columns until the SHA-256 matches.
    No key material is needed or stored: a wrong result can't pass the hash check. Raises StockError when it fails.
    """
    segs = parse_container(stock)
    total = sum(length for _, length in segs)
    payload = stock[HEADER_LEN:HEADER_LEN + total]
    if len(payload) < 64 * KEY_LEN:
        raise StockError("the file is too short to derive the key from")
    cols = [collections.Counter(payload[j::KEY_LEN]).most_common(4) for j in range(KEY_LEN)]
    base = [c[0][0] for c in cols]
    if sha256(bytes(base)) == want_sha256:
        return bytes(base)
    margin = [c[0][1] - (c[1][1] if len(c) > 1 else 0) for c in cols]
    doubtful = sorted(range(KEY_LEN), key=lambda j: margin[j])[:candidates]
    for r in range(1, max_alternatives + 1):
        for js in itertools.combinations(doubtful, r):
            for alts in itertools.product(*[range(1, len(cols[j])) for j in js]):
                k = list(base)
                for j, a in zip(js, alts):
                    k[j] = cols[j][a][0]
                k = bytes(k)
                if sha256(k) == want_sha256:
                    return k
    raise StockError("could not derive the bootloader key from this file (is it a DM-1701 stock firmware file?)")


def random_bytes(rng, n):
    return rng.getrandbits(8 * n).to_bytes(n, "little") if n else b""


def build_synthetic_stock(key, rng, profile_sha=None):
    """A synthetic stock file with the real V02.06 CSV layout, for tests: random key-independent content with the
    statistics of a firmware image (mostly zero bytes), a random codec at the stock windows. Returns
    (stock bytes, profiles dict for it, code, tables)."""
    real = PROFILES[profile_sha or "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"]
    segs = real["segments"]
    payload_len = sum(length for _, length in segs)
    # firmware-like plain text: about 60 % zero bytes, the rest random
    raw = random_bytes(rng, payload_len)
    plain = bytearray(raw.translate(bytes(b if b < 100 else 0 for b in range(256))))
    app_off = segs[0][1]	# the application payload follows the SPI flash segment
    code = bytearray(random_bytes(rng, real["code"][1]))
    struct.pack_into("<I", code, 0, CODEC_SIG)
    tables = random_bytes(rng, real["tables"][1])
    c0 = app_off + real["code"][0] - APP_BASE
    t0 = app_off + real["tables"][0] - APP_BASE
    plain[c0:c0 + len(code)] = code
    plain[t0:t0 + len(tables)] = tables
    header = bytearray(HEADER_LEN)
    header[0:len(MAGIC)] = MAGIC
    struct.pack_into("<I", header, 0x7C, len(segs))
    for i, (a, n) in enumerate(segs):
        struct.pack_into("<II", header, 0x80 + 8 * i, a, n)
    stream = (key * (payload_len // KEY_LEN + 1))[:payload_len]
    enc = bytes(a ^ b for a, b in zip(plain, stream))
    stock = bytes(header) + enc + random_bytes(rng, 0x100)	# trailer, as the real files
    prof = dict(real, code=(real["code"][0], real["code"][1], sha256(bytes(code))),
                tables=(real["tables"][0], real["tables"][1], sha256(tables)), size=len(stock))
    return stock, {sha256(stock): prof}, bytes(code), tables
