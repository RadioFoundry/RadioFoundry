# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""TYT/Baofeng stock firmware files as a library: container, decryption, codec extraction and merge, key derivation.

Recipes from docs/spec/codec-dm1701.md ("Container and encryption", "Extraction recipe (V02.06 CSV)") and
docs/spec/codec-md380.md ("Extraction recipe"). Used by rfoundry (merge from a stock file) and by
tools/codec_extract.py (the command line merge tool).

The codec is DVSI's and the radio maker's, not ours: nothing here stores it; it is read from the user's file on the
user's machine and only ever written to a file or a radio the user chooses.
"""
import collections
import hashlib
import itertools
import struct
import zlib

from . import codecmap

APP_BASE = 0x0800C000
FLASH_END = 0x08100000
KEY_LEN = 1024
HEADER_LEN = 0x100
MAGIC = b"OutSecurityBin"
CODEC_SIG = 0x4FF0E92D
# The descriptor the merge writes at the end of sector 11 (platform/stm32f4/codec_desc.h; docs/spec/codec-placement.md)
DESC_ADDR = 0x080FFF80
DESC_LEN = 0x80
DESC_MAGIC = 0x43534544      # "DESC"
DESC_VERSION = 1
DESC_WINDOWS = 5

# DM-1701 bootloader key identity (docs/spec/hardware-dm1701.md 15.3); the key bytes are never stored here
DM1701_KEY_SHA256 = "d983ce95fbc19fc78b913d9afc0381e3ba4ffedb480798865292d05a2fb3deff"
# MD-380 / MD-390 / RT3 (docs/spec/hardware-md380.md 14.2, as tools/stm32_dfu.py's md380 model)
MD380_KEY_SHA256 = "5254139c6b596d9413d106e6b7c0685ec245d69555cb28df98f061d6b879c4cd"
# MD-9600 / RT90 (docs/spec/codec-md9600.md "Container and encryption"; tools/stm32_dfu.py's md9600 model)
MD9600_KEY_SHA256 = "c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae"
# The count word of the single-segment container form (the MD-380 D002.032 file: one segment, count 0xFFFFFFFF)
SINGLE_SEGMENT = 0xFFFFFFFF

# One profile per supported stock file, keyed by its SHA-256 (codec-dm1701.md and codec-md380.md, "Stock files" and
# "Codec in the stock image"). Window = (flash address, length, SHA-256 of the decrypted bytes): "code" (its first word
# is CODEC_SIG), "tables", and "extra" for profiles whose codec has further small table windows. "codec_profile" is
# the id the firmware reports (debug link VERSION codec=) and the release manifests carry; "board" the radio it is
# for; "key_sha256" the identity of that radio's bootloader key. "placement" (docs/spec/codec-placement.md): where the
# merge puts the code ("code"), how much of the table window it keeps ("tables_len": the codec's own data, from the
# codec map; the tables stay at their stock address) and the descriptor's tag; "placed": the CRC-32 and SHA-256 of the
# code and tables as placed (the code's self references and the islands' calls into it rewritten for the new address),
# which the descriptor carries and the firmware checks, and the CRC-32 of each "extra" window (written unchanged).
PROFILES = {
    "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d": {
        "name": "DM-1701(CSV)-V02.06.bin",
        "version": "V02.06 CSV",
        "board": "dm1701",
        "key_sha256": DM1701_KEY_SHA256,
        "codec_profile": "dm1701-v0206-csv-s11",
        "size": 1272320,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xD9400)],
        "code": (0x08074714, 0x1021C, "1d8d04603b288e102f70252c1dd489600dfde656eccea6264e7bf4fac413f0cd"),
        "tables": (0x080D19F0, 0x12850, "b99f14e72bf72555946c1f04e8a1636652e6f62843baaf7a135787879e028894"),
        # The code goes right after the codec's data (which ends at 0x080E3A72) in sector 11, word aligned
        # (boards/dm1701/memory.ld, boards/dm1701/vocoder_dm1701.c); sectors 3-9 are the application's
        "placement": {"code": 0x080E3A74, "tables_len": 0x12082, "tag": 0x0206},
        "placed": {"code": (0x485BFC13, "d88c93ba5c3eac043558d05b182fe824a25aa373f84c48a341b369c0cc942e8c"),
                   "tables": (0xC1EEFA8B, "9ed939d23dd7b20f633d084f4713c35277d0227821070c551990caf8df409215")},
    },
    # TYT MD-380 "Firmware 2.32" (D002.032), single-segment container. Stock keeps the code in sectors 6 and 7
    "e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43": {
        "name": "MD-380-D2.32(AD).bin",
        "version": "D002.032",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": "md380-d2032-s11",
        "size": 0xF2E00,
        "segments": [(0x0800C000, 0xF2C00)],
        "code": (0x08050C44, 0x10884, "00d9565dc459c47119690e47c35daf9ae1a89c13bb3513d966390e0835b0c0ad"),
        "tables": (0x0808FC64, 0xAFF8, "9c7b66b08a6d397565c70551df9097870757f8de719b8d31525eba72e1db4840"),
        "extra": [
            (0x080D0FC8, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F9698, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F9E74, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FD490, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        # The code goes to the start of sector 11, below the small windows there (boards/md380/memory.ld,
        # boards/md380/vocoder_md380.c); the tables stay in sector 8, so sectors 3-7 are the application's
        "placement": {"code": 0x080E0000, "tag": 0x2032},
        "placed": {"code": (0x067E6A77, "d5c68973b33d5709b6d1df719b80841b77f1835d3bef5b61c6c682147da6bebc"),
                   "tables": (0x2A5C9EE1, "8c704a9606ec19fbe4af275de4fbd1ded64e2ab644e0115ec34bff86cb3d21a9"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    # TYT MD-9600 06.09 CSV (V1-V3 boards) and 16.45 CSV (V4), docs/spec/codec-md9600.md. The codec is board independent,
    # so either file serves every board revision, and both report one codec id (the descriptor's tag tells them apart). Stock keeps the code in sectors 7 and 8, which are the application's
    # now; the tables stay in sector 9 and the code goes to the start of sector 11 (boards/md9600/memory.ld,
    # boards/md9600/vocoder_md9600.c), checked in an emulator against the stock-address run
    "79a561eda2f5843334ea5974f5de511b6e172a1643f9163806d54a94404f0f09": {
        "name": "MD9600-CSV-V06.09.bin",
        "version": "V06.09 CSV",
        "board": "md9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": "md9600-s11",
        "size": 1094144,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB1800)],
        "code": (0x08074B18, 0xF214, "c729480a47c7444c03eff1ec6b176a76d071f72b64d2f0fe3965e517c85a1215"),
        "tables": (0x080AB13C, 0x10FE6, "0b431c14ef7e32fe8a0915976c250f127a757ed4cc5af2bc7f867ae261c6f1ca"),
        "placement": {"code": 0x080E0000, "tag": 0x0609},
        "placed": {"code": (0xD7D67AE8, "76b94a2727e0180cad4d354ee9bbea0a9d4278696dde76647508d6592963c737"),
                   "tables": (0x9983906A, "08d9806571c5d1a8280be09d6bace1f1b73a7b21713a41d4d4fec3d151e82533")},
    },
    "de368437de05307d3e217f049bb5e30b1a8ec201e50b8e81aa5c506089c53532": {
        "name": "MD9600-CSV(V4)-V16.45.bin",
        "version": "V16.45 CSV",
        "board": "md9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": "md9600-s11",
        "size": 1099264,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB2C00)],
        "code": (0x08075C98, 0xF214, "4bcfec3e1113d17bb53bee1543039894db00c00c32d1b7dfd258f224043f52cd"),
        "tables": (0x080AC2BC, 0x11036, "a101749f26c6df7d784130c45570e25abc5c88e02bee6f2510beb0a6c3ad4dfa"),
        "placement": {"code": 0x080E0000, "tag": 0x1645},
        "placed": {"code": (0xD5E2E873, "625b369a1f298e4760b0b07029a6cb98014715397c3ef529a6c5d7450a2f55e1"),
                   "tables": (0xA8A52935, "1dd472d6574b852ea57f86ffb9b366aab9d44398ea289bd750927bac6b95e51d")},
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
    "e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43": "MD-380 D002.032",
    # the bootloader key and a restore to stock only: no codec profile (the codec comes from D002.032)
    "1c903d440e35917e8a16906ccae1cbbd539230a5a3352e3eb896ffc3e96eccc6": "MD-380 D002.030",
    # MD-9600 builds that are not codec donors here (docs/spec/codec-md9600.md "Stock files"): the key derives from any
    "9454eb221ae1f206fca5a15ec1b540bf06b3db4991f466b1626a3a3073ca91d5": "MD-9600 V06.09 CSV + GPS",
    "7b1b1b33e661766a5b0c0b3ddab9c31c96e3855b09f4e280b07f4990b563b68f": "MD-9600 V06.09 REC",
    "4610ed115e80efa04be554a94915ec2e053ae00f6b64dfa4815451966840dba7": "MD-9600 V06.09 GPS + REC",
    "7f4d760288f0c1875c7c5dc83dcdf3f355f5188becde2b505eedf609fdbe665f": "MD-9600 V16.45 CSV + GPS",
    "d2ef7a7e9c484dc1a8777488816f4da47181899ff8b7299a0ead7d141d55e10c": "MD-9600 V16.45 REC",
    "3d71b4358267fe5fdae8980f340b6a158e7b229c9d32c8ab894c4985964e8aff": "MD-9600 V16.45 GPS + REC",
    "d8a653307222e576ee416ab6d4704d14758fa71c1b0e826ffd840f26266cf11f": "MD-9600 V26.45 CSV (V5)",
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


def windows(prof):
    """Every codec window of a profile in the stock file, in a fixed order: [(addr, length, sha256)], code first, then
    tables, then extra. Where the merge puts them in the radio: layout()."""
    return [prof["code"], prof["tables"]] + list(prof.get("extra", ()))


def code_map(prof, code=None):
    """The codec map for a profile's code (a synthetic profile carries its own, "map"), or None"""
    if prof.get("map"):
        return prof["map"]
    found = codecmap.by_code_sha(codecmap.load_maps(), prof["code"][2])
    return found[0] if found else None


def placed_windows(prof):
    """Where the merge puts a profile's codec: [(addr, length, sha256)], code first, then the table windows (the extra
    ones at their stock address, unchanged). Without "placement", the stock windows."""
    if not prof.get("placement"):
        return windows(prof)
    pl, pd = prof["placement"], prof["placed"]
    tab = prof["tables"]
    return [(pl["code"], prof["code"][1], pd["code"][1]),
            (tab[0], pl.get("tables_len", tab[1]), pd["tables"][1])] + list(prof.get("extra", ()))


def descriptor(prof):
    """The 128-byte descriptor for a placed codec (codec_desc.h): tag, code window, table windows, each with its CRC"""
    wins = placed_windows(prof)
    crcs = [prof["placed"]["code"][0], prof["placed"]["tables"][0]] + list(prof["placed"].get("extra", ()))
    if len(wins) - 1 > DESC_WINDOWS:
        raise StockError("%d table windows: the descriptor holds %d" % (len(wins) - 1, DESC_WINDOWS))
    words = [DESC_MAGIC, DESC_VERSION, prof["placement"]["tag"], wins[0][0], wins[0][1], crcs[0], len(wins) - 1]
    for i in range(DESC_WINDOWS):
        words += [wins[i + 1][0], wins[i + 1][1], crcs[i + 1]] if i + 1 < len(wins) else [0, 0, 0]
    body = struct.pack("<%dI" % len(words), *words)
    body += b"\0" * (DESC_LEN - 4 - len(body))
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def layout(prof):
    """Where the merge puts the codec in the radio: [(addr, length, sha256)], code first, then the tables, then the
    descriptor when the profile moves the code. Without "placement", the stock windows."""
    if not prof.get("placement"):
        return windows(prof)
    desc = descriptor(prof)
    return placed_windows(prof) + [(DESC_ADDR, DESC_LEN, sha256(desc))]


def place(prof, segments):
    """The decrypted stock windows [(stock address, bytes)] as the merge writes them (layout()). With "placement": the
    code moved (codec map: its self references rewritten), the table window cut to the codec's data with the islands'
    calls into the code re-encoded, the extra windows unchanged, then the descriptor. Every result is checked against
    "placed"."""
    if not prof.get("placement"):
        return list(segments)
    pl = prof["placement"]
    m = code_map(prof)
    if m is None:
        raise StockError("no codec map for %s: its code can't be moved" % prof["name"])
    (_, code), (tab_addr, tables) = segments[0], segments[1]
    try:
        moved = codecmap.move_code(code, m, pl["code"])
        tab = codecmap.move_branches(tab_addr, tables[:pl.get("tables_len", len(tables))], m, pl["code"])
    except codecmap.MapError as e:
        raise StockError("can't move the codec: %s" % e)
    if codecmap.branches_in(m, tab_addr, len(tab)) != len(m["branches"]):
        raise StockError("a branch into the code lies outside the tables the merge keeps")
    extra = list(segments[2:])
    for (_, n, want), got, what in zip(placed_windows(prof), [moved, tab] + [d for _, d in extra],
                                       ["code", "tables"] + ["table 0x%08X" % a for a, _ in extra]):
        if sha256(got) != want:
            raise StockError("the moved %s doesn't match the profile (sha256 %s)" % (what, sha256(got)))
    return [(pl["code"], moved), (tab_addr, tab)] + extra + [(DESC_ADDR, descriptor(prof))]


def profile_for_board(board_id):
    """The SHA-256 of the first supported stock file for a board, or None."""
    return next((h for h, p in PROFILES.items() if p.get("board") == board_id), None)


def find_profile(stock, profiles=None):
    """The profile for a stock file (by SHA-256), or raise StockError naming the supported files."""
    profiles = PROFILES if profiles is None else profiles
    digest = sha256(stock)
    prof = profiles.get(digest)
    if prof is None:
        known = KNOWN_FILES.get(digest)
        raise StockError("%s (sha256 %s...), not a supported codec source. Supported: %s"
                         % ("this file is " + known if known else "unknown stock file", digest[:16],
                            ", ".join("%s (%s...)" % (p["name"], h[:8]) for h, p in profiles.items())))
    return prof


def extract(stock, key, profiles=None):
    """Check the stock file and return (profile, segments): every codec window decrypted and checked, placed as the
    merge writes them, [(flash address, bytes)] in layout() order (segments[0] is the code)."""
    prof = find_profile(stock, profiles)
    if len(stock) != prof["size"]:
        raise StockError("size %d, expected %d" % (len(stock), prof["size"]))
    segs = parse_container(stock)
    if segs != prof["segments"]:
        raise StockError("segment table %s, expected %s" % (segs, prof["segments"]))
    out = []
    for i, (addr, length, want) in enumerate(windows(prof)):
        plain = decrypt(stock, app_file_offset(segs, addr), length, key)
        got = sha256(plain)
        if got != want:
            which = ("code", "tables")[i] if i < 2 else "table 0x%08X" % addr
            raise StockError("decrypted %s window sha256 %s, expected %s (wrong key?)" % (which, got, want))
        out.append((addr, plain))
    if struct.unpack_from("<I", out[0][1], 0)[0] != CODEC_SIG:
        raise StockError("codec first word is not 0x%08X" % CODEC_SIG)
    return prof, place(prof, out)


def codec_segments(prof, segments):
    """The codec as flash segments [(address, bytes), ...] for rfoundry's planner."""
    return [(a, d) for a, d in segments]


def merge(image, prof, segments):
    """The built application image (plain, from 0x0800C000) with every codec segment (extract()) written at its
    address, as one contiguous image (0xFF between the parts)."""
    if image.startswith(MAGIC):
        raise StockError("the image is an official firmware file; pass the RadioFoundry build (rfoundry_%s.bin)"
                         % prof.get("board", "dm1701"))
    first = min(a for a, _ in segments)
    if APP_BASE + len(image) > first:
        raise StockError("the application image ends at 0x%08X, inside the codec window at 0x%08X: rebuild with "
                         "the window reserved (boards/%s/%s.ld)" % (APP_BASE + len(image), first,
                                                                    prof.get("board", "dm1701"),
                                                                    prof.get("board", "dm1701")))
    end = max(a + len(d) for a, d in segments)
    if end > FLASH_END:
        raise StockError("merged image would pass the end of flash")
    out = bytearray(b"\xFF" * (end - APP_BASE))
    out[0:len(image)] = image
    for a, d in segments:
        out[a - APP_BASE:a - APP_BASE + len(d)] = d
    return bytes(out)


def split_merged(image, profiles=None):
    """The reverse of merge(): (application image, profile, codec segments) when the image carries a codec where a
    profile's layout() puts it, None when it holds none (a plain image, which the planner checks as usual). A merged
    image has no manifest, so the codec windows are checked by their SHA-256 (which also names the radio) and every
    byte outside the application and the windows must be 0xFF, as merge() leaves it. Raises StockError for an image
    with the codec signature where a profile puts its code whose windows don't match that profile."""
    profiles = PROFILES if profiles is None else profiles
    candidates = []
    for prof in profiles.values():
        wins = layout(prof)
        first = min(a for a, _, _ in wins)
        if APP_BASE + len(image) <= first:
            continue
        code = wins[0][0] - APP_BASE
        if len(image) >= code + 4 and struct.unpack_from("<I", image, code)[0] == CODEC_SIG:
            candidates.append(prof)	# looks merged: a mismatch below is an error, not a plain image
        if APP_BASE + len(image) < max(a + n for a, n, _ in wins):
            continue
        segs = [(a, image[a - APP_BASE:a - APP_BASE + n]) for a, n, _ in wins]
        if any(sha256(d) != want for (_, d), (_, _, want) in zip(segs, wins)):
            continue
        rest = bytearray(image[first - APP_BASE:])
        for a, d in segs:
            rest[a - first:a - first + len(d)] = b"\xFF" * len(d)
        if rest.count(0xFF) != len(rest):
            raise StockError("the image holds the %s codec, but also other data between or after its windows: not "
                             "an image rfoundry merge (or the build's codec step) wrote" % prof["codec_profile"])
        return image[:first - APP_BASE].rstrip(b"\xFF"), prof, segs
    if candidates:
        raise StockError("the image has a codec at 0x%08X, but not the one from %s: a merged image must be made from "
                         "a supported stock file" % (layout(candidates[0])[0][0], candidates[0]["name"]))
    return None


def _alternatives(base, cands, order, want_sha256, max_alternatives):
    """Try the next candidates in the least certain columns (order) until the key matches its hash, or None."""
    for r in range(1, max_alternatives + 1):
        for js in itertools.combinations(order, r):
            for alts in itertools.product(*[cands[j][1:] for j in js]):
                k = list(base)
                for j, a in zip(js, alts):
                    k[j] = a
                k = bytes(k)
                if sha256(k) == want_sha256:
                    return k
    return None


def _refine(pairs, cands, key, seed_columns, strict, rounds=8):
    """Bigram refinement of a voted key, trained on the file itself.

    Firmware isn't random: the plain byte pairs that occur next to each other (Thumb-2 opcodes, little endian
    addresses, tables) have a strongly skewed distribution, with even and odd offsets distinct. pairs[j] counts the
    cipher byte pairs (column j-1, column j) over all rows, so with a key guess the plain pair counts follow without
    touching the payload again. Round 0 learns the pair model only from the seed columns (the clearest votes) and
    re-picks every other column among its vote candidates (strict: only columns next to a seed column, judged against
    their seed neighbors alone); later rounds learn from the whole image and re-pick all
    columns, until nothing changes. Returns (key, per-column score margin)."""
    import math
    key = list(key)
    seed = set(seed_columns)
    margin = [0.0] * KEY_LEN
    for rnd in range(rounds):
        counts = [[1] * 65536, [1] * 65536]
        for j in range(KEY_LEN):
            jl = (j - 1) % KEY_LEN
            if rnd == 0 and not (j in seed and jl in seed):
                continue
            m, kl, kj = counts[jl & 1], key[jl], key[j]
            for (a, c), n in pairs[j].items():
                m[(a ^ kl) << 8 | (c ^ kj)] += n
        logp = [[math.log(x) for x in m] for m in counts]
        changed = 0
        for j in range(KEY_LEN):
            if rnd == 0 and j in seed:
                continue
            jl, jr = (j - 1) % KEY_LEN, (j + 1) % KEY_LEN
            left, right = logp[jl & 1], logp[j & 1]
            kl, kr = key[jl], key[jr]
            # strict: round 0 trusts only seed neighbors (two wrong neighbors can hold each other in place)
            trust = rnd or not strict
            lp = [((a ^ kl) << 8, c, n) for (a, c), n in pairs[j].items()] if trust or jl in seed else []
            rp = [(c, b ^ kr, n) for (c, b), n in pairs[jr].items()] if trust or jr in seed else []
            if not lp and not rp:
                continue
            scored = sorted(((sum(n * left[a | (c ^ k)] for a, c, n in lp) +
                              sum(n * right[(c ^ k) << 8 | b] for c, b, n in rp), k) for k in cands[j]), reverse=True)
            margin[j] = scored[0][0] - scored[1][0] if len(scored) > 1 else 1e9
            if scored[0][1] != key[j]:
                key[j] = scored[0][1]
                changed += 1
        if rnd and not changed:
            break
    return bytes(key), margin


def derive_key(stock, want_sha256=DM1701_KEY_SHA256, max_alternatives=2, candidates=24):
    """Recover the 1024-byte bootloader key from a stock file, checked against its published SHA-256.

    The payload is plain XOR key[i mod 1024] (codec-dm1701.md "Container and encryption"). In a DM-1701 image the
    most common plain byte in every key column is 0x00, so the most common cipher byte of column j is key[j] for
    nearly every j, and the few columns where that guess is wrong are found by trying the next most common bytes in the
    least confident columns until the SHA-256 matches. The MD-380 image is far less zero-dominated (a third of the
    columns vote wrong), so when the vote fails the key is refined with a byte-pair model learned from the file itself
    (_refine) and then checked the same way. No key material is needed or stored: a wrong result can't pass the hash
    check. Raises StockError when it fails.
    """
    segs = parse_container(stock)
    total = sum(length for _, length in segs)
    payload = stock[HEADER_LEN:HEADER_LEN + total]
    if len(payload) < 64 * KEY_LEN:
        raise StockError("the file is too short to derive the key from")
    cols = [collections.Counter(payload[j::KEY_LEN]).most_common(8) for j in range(KEY_LEN)]
    base = bytes(c[0][0] for c in cols)
    if sha256(base) == want_sha256:
        return base
    cands = [[b for b, _ in c] for c in cols]
    margin = [c[0][1] - (c[1][1] if len(c) > 1 else 0) for c in cols]
    doubtful = sorted(range(KEY_LEN), key=lambda j: margin[j])[:candidates]
    k = _alternatives(base, [c[:4] for c in cands], doubtful, want_sha256, max_alternatives)
    if k:
        return k
    # cipher byte pairs (column j-1, column j) per column, counted once
    pairs = [collections.Counter() for _ in range(KEY_LEN)]
    for j in range(KEY_LEN):
        start = j if j else KEY_LEN
        pairs[j].update(zip(payload[start - 1::KEY_LEN], payload[start::KEY_LEN]))
    rel = sorted(range(KEY_LEN), key=lambda j: -(cols[j][0][1] - cols[j][1][1]) / cols[j][0][1] if len(cols[j]) > 1
                 else -1)
    for frac, strict in ((0.4, False), (0.4, True), (0.3, False), (0.3, True), (0.2, True)):
        k, score_margin = _refine(pairs, cands, base, rel[:int(KEY_LEN * frac)], strict)
        if sha256(k) == want_sha256:
            return k
        order = sorted(range(KEY_LEN), key=lambda j: score_margin[j])[:candidates // 2]
        alt = [[k[j]] + [b for b in cands[j] if b != k[j]][:2] for j in range(KEY_LEN)]
        k = _alternatives(k, alt, order, want_sha256, max_alternatives)
        if k:
            return k
    raise StockError("could not derive the bootloader key from this file (is it a stock firmware file for this radio?)")


def random_bytes(rng, n):
    return rng.getrandbits(8 * n).to_bytes(n, "little") if n else b""


SYN_SELF_REF = 0x40      # the synthetic codec's self reference (code offset) and island call (table window offset)
SYN_BL = 0x110


def build_synthetic_stock(key, rng, profile_sha=None):
    """A synthetic stock file with the layout of a real profile (default the DM-1701 V02.06 CSV), for tests:
    random key-independent content with the statistics of a firmware image (mostly zero bytes), random codec windows
    at the stock addresses. Returns (stock bytes, profiles dict for it, code, tables)."""
    real = PROFILES[profile_sha or "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"]
    segs = real["segments"]
    payload_len = sum(length for _, length in segs)
    # firmware-like plain text: about 60 % zero bytes, the rest random
    raw = random_bytes(rng, payload_len)
    plain = bytearray(raw.translate(bytes(b if b < 100 else 0 for b in range(256))))
    app_off = sum(length for base, length in segs if base != APP_BASE and base < APP_BASE)	# app segment offset
    wins = []
    run = real["code"][0]
    for i, (addr, length, _) in enumerate(windows(real)):
        data = bytearray(random_bytes(rng, length))
        if i == 0:
            struct.pack_into("<I", data, 0, CODEC_SIG)
            if real.get("placement"):
                struct.pack_into("<I", data, SYN_SELF_REF, run + 0x201)	# a Thumb pointer into the code
        elif i == 1 and real.get("placement"):
            codecmap.put_bl(data, SYN_BL, addr + SYN_BL, run + 0x200)	# an island's call into the code
        off = app_off + addr - APP_BASE
        plain[off:off + length] = data
        wins.append((addr, length, sha256(bytes(data)), bytes(data)))
    header = bytearray(HEADER_LEN)
    header[0:len(MAGIC)] = MAGIC
    struct.pack_into("<I", header, 0x7C, len(segs) if len(segs) > 1 else SINGLE_SEGMENT)
    for i, (a, n) in enumerate(segs):
        struct.pack_into("<II", header, 0x80 + 8 * i, a, n)
    stream = (key * (payload_len // KEY_LEN + 1))[:payload_len]
    enc = bytes(a ^ b for a, b in zip(plain, stream))
    stock = bytes(header) + enc + random_bytes(rng, 0x100)	# trailer, as the real files
    prof = dict(real, code=wins[0][:3], tables=wins[1][:3], size=len(stock))
    if "extra" in real:
        prof["extra"] = [w[:3] for w in wins[2:]]
    if real.get("placement"):	# its own codec map (a self reference, an island with a call into the code), placed
        code, tab = wins[0][3], wins[1][3]
        isl = wins[1][0] + SYN_BL - 0x10
        prof["map"] = codecmap.normalize({
            "format": codecmap.FORMAT, "profile": "synthetic-" + real["codec_profile"], "board": real["board"],
            "isa": "thumb2", "code": {"flash": run, "len": len(code), "run": run, "sha256": sha256(code)},
            "entries": {}, "self_refs": [{"off": SYN_SELF_REF, "value": run + 0x201}],
            "islands": [{"start": isl, "len": 0x40}],
            "branches": [{"at": wins[1][0] + SYN_BL, "target": run + 0x200}]})
        at, n = real["placement"]["code"], real["placement"].get("tables_len", len(tab))
        mc = codecmap.move_code(code, prof["map"], at)
        mt = codecmap.move_branches(wins[1][0], tab[:n], prof["map"], at)
        prof["placed"] = {"code": (zlib.crc32(mc) & 0xFFFFFFFF, sha256(mc)),
                          "tables": (zlib.crc32(mt) & 0xFFFFFFFF, sha256(mt))}
        if "extra" in real:
            prof["placed"]["extra"] = [zlib.crc32(w[3]) & 0xFFFFFFFF for w in wins[2:]]
    return stock, {sha256(stock): prof}, wins[0][3], wins[1][3]
