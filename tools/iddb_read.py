#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Reading a DMR ID database from the codeplug image, format N (docs/spec/open-firmware-cps.md 7.5) or format C
(docs/spec/iddb-compact.md), through a callback that reads image bytes, so a test reads only the blocks it needs
(tools/cps_fast.py over the debug link, or a file). Written from the spec, apart from the radio's reader
(core/codeplug/dmr_iddb.c) and the web editor's (site/static/js/codeplug/iddb.js), so a test can hold one against
another: tests/hil/test_iddb.py compares records read here with the text the radio shows (DL_CMD_IDDB).

    db = Database(lambda addr, n: image[addr:addr + n])
    db.format, db.count, db.sample(), db.records()
"""

ALPHABET = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz."
FLASH_START = 0x20000           # the image address of the flash part (open-firmware-cps.md 7)
AREA1_OFF, AREA2_OFF = 0x30000, 0xB8000
AREA1_SIZE = 0x40000
HEADER = 12
PAGE = 4096


def unpack6(b, chars):
    """chars characters packed at 6 bits, most significant first"""
    bits = int.from_bytes(b, "big")
    total = len(b) * 8
    return "".join(ALPHABET[(bits >> (total - 6 * (j + 1))) & 0x3F] for j in range(chars))


class Database:
    def __init__(self, get, flash_start=FLASH_START):
        """get(image address, n) -> n bytes"""
        self.get = get
        self.area1 = flash_start + AREA1_OFF
        self.area2 = flash_start + AREA2_OFF
        h = get(self.area1, 32)
        self.format, self.count = None, 0
        if h[:2] != b"Id":
            return
        if h[2:4] == b"C\x00":
            self._compact(h)
        elif h[2:3] == b"N" and h[3] >= 0x4A:
            self.rec = h[3] - 0x4A
            self.count = int.from_bytes(h[8:12], "little")
            self.text_len = (self.rec - 3) * 4 // 3
            self.per_area1 = (AREA1_SIZE - HEADER) // self.rec
            self.format = "N"

    # ── format N: records end to end ──
    def record(self, i):
        """Format N: record i as (id, text)"""
        a = self.area1 + HEADER + i * self.rec if i < self.per_area1 else self.area2 + (i - self.per_area1) * self.rec
        r = self.get(a, self.rec)
        return int.from_bytes(r[:3], "little"), unpack6(r[3:], self.text_len).rstrip(" ")

    # ── format C: an index of pages, a dictionary ──
    def _compact(self, h):
        le = lambda o, n: int.from_bytes(h[o:o + n], "little")   # noqa: E731
        if h[4] != 1:
            return
        self.count, self.size, self.index = le(8, 4), le(12, 4), le(16, 4)
        self.pages, self.dict, self.table, self.strings = le(20, 2), le(22, 2), le(24, 4), le(28, 4)
        self.format = "C"

    def _db(self, off, n):
        """Bytes at a database offset (iddb-compact.md 2); a range never crosses from area 1 to area 2 here"""
        return self.get(self.area1 + off if off < AREA1_SIZE else self.area2 + off - AREA1_SIZE, n)

    def entry(self, k):
        """Format C: dictionary entry k"""
        off = self.strings + int.from_bytes(self._db(self.table + 3 * k, 3), "little")
        n = self._db(off, 1)[0]
        return unpack6(self._db(off + 1, (6 * n + 7) // 8), n)

    def index_entry(self, i):
        """Format C: index entry i as (first ID, page)"""
        e = self._db(self.index + 5 * i, 5)
        return int.from_bytes(e[:3], "little"), int.from_bytes(e[3:], "little")

    def page(self, p):
        """Format C: the records of page p as [(id, text)]"""
        b = self._db(p * PAGE, PAGE)
        count, i, out, dmr_id = int.from_bytes(b[:2], "little"), 2, [], 0
        for n in range(count):
            v, shift = 0, 0
            while True:
                x = b[i]
                i += 1
                v |= (x & 0x7F) << shift
                shift += 7
                if not x & 0x80:
                    break
            dmr_id = v if n == 0 else dmr_id + v
            parts = []
            for _ in range(3):
                t = b[i]
                i += 1
                if t < 0x80:
                    parts.append(self.entry(t))
                elif t < 0xC0:
                    parts.append(self.entry(128 + ((t - 0x80) << 8) + b[i]))
                    i += 1
                elif t == 0xC0:
                    continue
                elif t <= 0xF0:
                    k = (6 * (t - 0xC0) + 7) // 8
                    parts.append(unpack6(b[i:i + k], t - 0xC0))
                    i += k
                else:
                    raise ValueError(f"page {p}: token {t:#x} at {i - 1}")
            out.append((dmr_id, " ".join(s for s in parts if s)))
        return out

    # ── both ──
    def sample(self):
        """Records from the start, the middle and the end, as runs of records that follow each other in the database
        (an ID between two of a run is in no record): format N three records each, format C three whole pages"""
        if self.format == "N":
            starts = sorted({max(0, min(c - 1, self.count - 3)) for c in (0, self.count // 2, self.count - 1)})
            return [[self.record(i) for i in range(s, min(s + 3, self.count))] for s in starts]
        if self.format == "C":
            return [self.page(p) for p in sorted({self.index_entry(i)[1] for i in (0, self.pages // 2, self.pages - 1)})]
        return []

    def records(self):
        """Every record (reads the whole database)"""
        if self.format == "N":
            return [self.record(i) for i in range(self.count)]
        if self.format == "C":
            return [r for i in range(self.pages) for r in self.page(self.index_entry(i)[1])]
        return []
