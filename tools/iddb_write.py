#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Writing a DMR ID database, format N (docs/spec/open-firmware-cps.md 7.5) or format C written fresh
(docs/spec/iddb-compact.md 4.1), as the 4 KB blocks of the codeplug image it takes, and putting blocks on the radio
over the fast codeplug protocol (tools/cps_fast.py, docs/spec/cps-fast.md) in the order of iddb-compact.md 4.3. The
counterpart of tools/iddb_read.py, written from the same specs; the web editor's encoder is
site/static/js/codeplug/iddb.js. tests/hil/test_iddb.py writes a synthetic database of the format a test needs and
puts the radio's own blocks back after.

    blocks = encode_n(records, text_len=32)      # {block address: 4096 bytes}; records [(id, text)]
    blocks = encode_c(records)
    backup = save(link, blocks, path)            # the radio's bytes of those blocks, also into a file
    put(link, blocks)                            # the header block erased first, written last
    restore(link, backup)                        # put back, then read back and compared byte for byte

  iddb_write.py restore FILE [--port PORT]      put a backup file save() wrote back on the radio
"""
import argparse
import collections
import math
import os
import struct
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from iddb_read import ALPHABET, AREA1_OFF, AREA1_SIZE, AREA2_OFF, FLASH_START, HEADER, PAGE  # noqa: E402

BLOCK = 4096
TEXT_LENGTHS = (16, 24, 32, 48)     # format N text lengths (whole 3-byte groups)
TEXT_MAX = 48                       # the radio keeps no more
C_HEADER = 32
PAGE_FILL = 3686                    # a fresh record page is closed at 90 % (iddb-compact.md 4.1 rule 3)
DICT_MAX = 16512
SLACK = 1.25                        # the index and the dictionary get their size and a quarter (4.1 rule 4)
BACKUP_MAGIC = b"IdBk"


def pack6(text):
    """text packed at 6 bits per character, most significant first, the last byte padded with zero bits"""
    bits = 0
    for c in text:
        k = ALPHABET.find(c)
        if k < 0:
            raise ValueError(f"{c!r} is not in the database alphabet")
        bits = bits << 6 | k
    n = (6 * len(text) + 7) // 8
    return (bits << (8 * n - 6 * len(text))).to_bytes(n, "big")


def clean(text):
    """Text the alphabet holds: accents dropped, anything else a space, spaces collapsed"""
    t = "".join(c for c in unicodedata.normalize("NFD", text) if not unicodedata.combining(c))
    return " ".join("".join(c if c in ALPHABET else " " for c in t).split())


def prepare(records, max_len):
    """Records as the radio takes them: IDs 1 to 2^24 - 1, the first of each ID kept, texts cleaned and cut, sorted"""
    seen, out = set(), []
    for dmr_id, text in records:
        if 1 <= dmr_id <= 0xFFFFFF and dmr_id not in seen:
            seen.add(dmr_id)
            out.append((dmr_id, clean(text)[:max_len].strip()))
    return sorted(out)


def synthetic_records(n=600, first_id=9000001, max_gap=300):
    """n made-up records: IDs with gaps of 1 to max_gap (one- and two-byte LEB128 gaps in format C), texts of
    a made-up callsign, a name from a small set and a town from a larger one, so format C gets one- and two-byte
    dictionary codes, literals and empty tokens"""
    out, dmr_id = [], first_id
    for i in range(n):
        call = f"X{i % 10}{chr(65 + i % 26)}{chr(65 + i * 7 % 26)}{chr(65 + i * 11 % 26)}"
        if i % 9 == 0:
            text = call                                 # callsign alone: two empty tokens
        elif i % 7 == 0:
            text = f"{call} Name{i % 23}"               # no town: one empty token
        else:
            text = f"{call} Name{i % 23} Town{i % 170} ST{i % 4}"
        out.append((dmr_id, text))
        dmr_id += 1 + (i * 37) % max_gap
    return out


class Blocks(dict):
    """{block address: bytearray(4096)}: bytes put at image addresses, a block 0xFF until something is put in it"""
    def put(self, addr, data):
        while data:
            blk = addr - addr % BLOCK
            k = min(len(data), blk + BLOCK - addr)
            b = self.setdefault(blk, bytearray(b"\xff" * BLOCK))
            b[addr - blk:addr - blk + k] = data[:k]
            addr, data = addr + k, data[k:]

    def frozen(self):
        return {a: bytes(b) for a, b in sorted(self.items())}


def header_addr(flash_start=FLASH_START):
    """The image address of the database header: the start of area 1"""
    return flash_start + AREA1_OFF


def encode_n(records, text_len=32, flash_start=FLASH_START):
    """Format N: the header, then the records sorted by ID, a 3-byte LE ID and the text packed at 6 bits (text_len
    characters, padded with spaces), area 1 holding as many whole records as fit and area 2 the rest. Returns
    {block address: 4096 bytes}, 0xFF where nothing goes"""
    if text_len not in TEXT_LENGTHS:
        raise ValueError(f"text length {text_len}: one of {TEXT_LENGTHS}")
    rec = 3 + text_len * 3 // 4
    recs = prepare(records, text_len)
    area1, area2 = flash_start + AREA1_OFF, flash_start + AREA2_OFF
    per_area1 = (AREA1_SIZE - HEADER) // rec
    out = Blocks()
    out.put(area1, b"IdN" + bytes([rec + 0x4A]) + b"001\x00" + struct.pack("<I", len(recs)))
    for i, (dmr_id, text) in enumerate(recs):
        a = area1 + HEADER + i * rec if i < per_area1 else area2 + (i - per_area1) * rec
        out.put(a, dmr_id.to_bytes(3, "little") + pack6(text.ljust(text_len)))
    return out.frozen()


def _slots(text):
    """A text's three tokens: split at its first two spaces (iddb-compact.md 3.4)"""
    parts = text.split(" ", 2)
    return parts + [""] * (3 - len(parts))


def _leb(v):
    out = bytearray()
    while v >= 0x80:
        out.append(v & 0x7F | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def _token(s, codes):
    if not s:
        return b"\xc0"
    k = codes.get(s)
    if k is None:
        return bytes([0xC0 + len(s)]) + pack6(s)
    return bytes([k]) if k < 128 else bytes([0x80 + ((k - 128) >> 8), (k - 128) & 0xFF])


def _record(dmr_id, slots, prev, codes):
    return _leb(dmr_id if prev is None else dmr_id - prev) + b"".join(_token(s, codes) for s in slots)


def _pages_for(n):
    return max(1, math.ceil(n * SLACK / PAGE))


def encode_c(records, flash_start=FLASH_START):
    """Format C written fresh (iddb-compact.md 4.1): page 0 the header, then the index, the dictionary's table and
    strings with a quarter spare, then the record pages, each closed at 90 %. Returns {block address: 4096 bytes}"""
    recs = [(i, t, _slots(t)) for i, t in prepare(records, TEXT_MAX)]
    if not recs:
        raise ValueError("no records")
    lit = lambda s: 1 + (6 * len(s) + 7) // 8           # noqa: E731  (a literal, or a string in the dictionary)
    uses = collections.Counter(s for _, _, sl in recs for s in sl if s)
    # an entry when its uses as a code plus the string once cost less than as literals (2-byte codes counted)
    words = sorted((s for s, n in uses.items() if n * lit(s) > n * 2 + lit(s) + 3), key=lambda s: (-uses[s], s))
    words = words[:DICT_MAX]
    codes = {s: k for k, s in enumerate(words)}
    pages, cur, used = [], [], 2
    for dmr_id, _, sl in recs:
        n = len(_record(dmr_id, sl, cur[-1][0] if cur else None, codes))
        if cur and used + n > PAGE_FILL:
            pages.append(cur)
            cur, used = [], 2 + len(_record(dmr_id, sl, None, codes))
        else:
            used += n
        cur.append((dmr_id, sl))
    pages.append(cur)
    strings = b"".join(bytes([len(s)]) + pack6(s) for s in words)
    table, o = bytearray(), 0
    for s in words:
        table += o.to_bytes(3, "little")
        o += lit(s)
    at_index = 1
    at_table = at_index + _pages_for(5 * len(pages))
    at_strings = at_table + _pages_for(len(table))
    first = at_strings + _pages_for(len(strings))
    size = (first + len(pages)) * PAGE
    area1, area2 = flash_start + AREA1_OFF, flash_start + AREA2_OFF
    out = Blocks()

    def put(off, data):
        """data at a database offset (iddb-compact.md 2); a section never crosses from area 1 to area 2 here"""
        out.put(area1 + off if off < AREA1_SIZE else area2 + off - AREA1_SIZE, data)

    index = bytearray()
    for k, page in enumerate(pages):
        index += page[0][0].to_bytes(3, "little") + (first + k).to_bytes(2, "little")
        b = bytearray(struct.pack("<H", len(page)))
        prev = None
        for dmr_id, sl in page:
            b += _record(dmr_id, sl, prev, codes)
            prev = dmr_id
        put((first + k) * PAGE, bytes(b) + b"\xff" * (PAGE - len(b)))
    for p in range(at_index, first):                    # the sections' spare room erased
        put(p * PAGE, b"\xff" * PAGE)
    put(at_index * PAGE, bytes(index))
    put(at_table * PAGE, bytes(table))
    put(at_strings * PAGE, strings)
    h = (b"IdC\x00" + bytes([1, max(len(t) for _, t, _ in recs)]) + struct.pack("<H", first)
         + struct.pack("<IIIHHII", len(recs), size, at_index * PAGE, len(pages), len(words), at_table * PAGE,
                       at_strings * PAGE))
    assert len(h) == C_HEADER
    put(0, h + b"\xff" * (PAGE - C_HEADER))
    return out.frozen()


# ── on the radio, through a cps_fast.FastLink ──

def read_blocks(link, addrs):
    return {a: link.read_block(a) for a in sorted(addrs)}


def save(link, blocks, path):
    """The radio's bytes of the blocks blocks names, read before anything is written, and kept in path (magic, count,
    then address u32 and 4096 bytes for each) so `iddb_write.py restore path` can put them back by hand"""
    backup = read_blocks(link, blocks)
    with open(path, "wb") as f:
        f.write(BACKUP_MAGIC + struct.pack("<I", len(backup)))
        for a, b in backup.items():
            f.write(struct.pack("<I", a) + b)
    return backup


def load(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != BACKUP_MAGIC:
        raise ValueError(f"{path}: not a database backup")
    n = struct.unpack_from("<I", d, 4)[0]
    if len(d) != 8 + n * (4 + BLOCK):
        raise ValueError(f"{path}: {len(d)} bytes for {n} blocks")
    return {struct.unpack_from("<I", d, 8 + k * (4 + BLOCK))[0]: d[12 + k * (4 + BLOCK):8 + (k + 1) * (4 + BLOCK)]
            for k in range(n)}


def put(link, blocks, header=None):
    """Write the blocks that differ from the radio's in one direct session (a database is never staged), in the order
    of iddb-compact.md 4.3: the header block first with its header erased (from here the radio has no database), the
    others, the header block last. A session cut short leaves no database, never a mix. Returns the blocks written"""
    import cps_fast
    header = header_addr(link.info()["flash_start"]) if header is None else header
    if header not in blocks:
        raise ValueError(f"the blocks don't hold the header block {header:#x}")
    sums = link.sums(sorted(blocks))
    changed = [a for a in sorted(blocks) if cps_fast.crc32(blocks[a]) != sums[a][0]]
    if not changed:
        return 0
    # the 32 bytes of the longer header (format C) erased: no "Id" tag, so no database in either format
    erased = b"\xff" * C_HEADER + bytes(blocks[header][C_HEADER:])
    try:
        link.begin(cps_fast.MODE_DIRECT)
    except cps_fast.FastError as e:
        if e.reason != 6:
            raise
        link.abort()                    # a session left open (a run stopped mid-write): end it, then begin
        link.begin(cps_fast.MODE_DIRECT)

    def send(addr, data, again=3):
        """One block. A BLOCK refused for the session's state means the radio ended it (no request for 10 s: the
        host stalled): a new session, and the block again. The header is still erased, so nothing reads a mix"""
        while True:
            try:
                return link.write_block(addr, data)
            except cps_fast.FastError as e:
                if e.reason != 6 or again == 0:
                    raise
                again -= 1
                link.begin(cps_fast.MODE_DIRECT)

    try:
        send(header, erased)
        for a in changed:
            if a != header:
                send(a, bytes(blocks[a]))
        send(header, bytes(blocks[header]))
    except cps_fast.FastError:
        link.abort()
        raise
    link.commit()
    return len(changed) + (header not in changed)


def differ(link, blocks):
    """Addresses of the blocks whose bytes on the radio differ from blocks (each read back whole)"""
    return [a for a in sorted(blocks) if link.read_block(a) != bytes(blocks[a])]


def restore(link, backup, header=None, tries=2):
    """put() the backup, read each block back and compare it byte for byte; a block that still differs is sent
    again. Raises cps_fast.FastError naming the blocks when it can't make them match"""
    import cps_fast
    bad = list(backup)
    for _ in range(tries):
        put(link, backup, header)
        bad = differ(link, backup)
        if not bad:
            return
    raise cps_fast.FastError(f"restore: blocks {[hex(a) for a in bad]} differ from the backup after {tries} writes")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--exec", dest="exec_cmd", help="command speaking the protocol on stdin / stdout (tests)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("restore", help="put a backup file save() wrote back on the radio and read it back")
    r.add_argument("backup")
    a = ap.parse_args(argv)
    import cps_fast
    backup = load(a.backup)
    link = cps_fast.open_link(a.port, a.exec_cmd)
    try:
        restore(link, backup)
        info = link.iddb()
    except cps_fast.FastError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        link.close()
    print(f"restored {len(backup)} block(s) from {a.backup}, read back and matching; the radio holds "
          f"{'format ' + info['format'] if info['format'] else 'no'} ID database, {info['records']} records")
    return 0


if __name__ == "__main__":
    sys.exit(main())
