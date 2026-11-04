#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The debug link's IDDB command (docs/spec/debuglink.md 3.23) and tools/iddb_read.py against cpsSim: databases in
format N (open-firmware-cps.md 7.5, records running into area 2) and format C (iddb-compact.md: 1- and 2-byte
dictionary codes, literals, empty tokens, a record page in area 2), built here byte by byte from the specs, written
into the simulated flash, read back through tools/cps_fast.py and looked up by the radio's code. Run by
`make -C tests/host`."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import cps_fast  # noqa: E402
import iddb_read  # noqa: E402

AREA1 = iddb_read.FLASH_START + iddb_read.AREA1_OFF
AREA2 = iddb_read.FLASH_START + iddb_read.AREA2_OFF
failures = 0


def check(cond, what):
    global failures
    if not cond:
        print(f"FAIL {what}")
        failures += 1


def pack6(text):
    bits = 0
    for c in text:
        bits = (bits << 6) | iddb_read.ALPHABET.index(c)
    n = (6 * len(text) + 7) // 8
    return (bits << (8 * n - 6 * len(text))).to_bytes(n, "big")


def put(flash, off, data):
    """data at database offset off (area 1, then area 2)"""
    a = AREA1 + off if off < iddb_read.AREA1_SIZE else AREA2 + off - iddb_read.AREA1_SIZE
    flash[a:a + len(data)] = data


def format_n(flash, records, text_len=16):
    rec = 3 + text_len * 3 // 4
    per_area1 = (iddb_read.AREA1_SIZE - iddb_read.HEADER) // rec
    flash[AREA1:AREA1 + 12] = b"IdN" + bytes([rec + 0x4A]) + b"001\x00" + len(records).to_bytes(4, "little")
    for i, (dmr_id, text) in enumerate(records):
        r = dmr_id.to_bytes(3, "little") + pack6(text.ljust(text_len))
        a = AREA1 + 12 + i * rec if i < per_area1 else AREA2 + (i - per_area1) * rec
        flash[a:a + rec] = r
    return per_area1


def leb(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def format_c(flash, pages, words):
    """pages: [(page number, [(id, [token, token, token])])], a token a dictionary index, a str literal or None"""
    PAGE = iddb_read.PAGE
    index, table, strings = 1, 2, 3                     # pages
    strs = b"".join(bytes([len(w)]) + pack6(w) for w in words)
    offs, o = [], 0
    for w in words:
        offs.append(o)
        o += 1 + len(pack6(w))
    put(flash, table * PAGE, b"".join(x.to_bytes(3, "little") for x in offs))
    put(flash, strings * PAGE, strs)
    ix = b""
    for p, recs in sorted(pages, key=lambda x: x[1][0][0]):
        ix += recs[0][0].to_bytes(3, "little") + p.to_bytes(2, "little")
        b = bytearray(len(recs).to_bytes(2, "little"))
        prev = None
        for dmr_id, toks in recs:
            b += leb(dmr_id if prev is None else dmr_id - prev)
            prev = dmr_id
            for t in toks:
                if t is None:
                    b.append(0xC0)
                elif isinstance(t, str):
                    b += bytes([0xC0 + len(t)]) + pack6(t)
                elif t < 128:
                    b.append(t)
                else:
                    b += bytes([0x80 + ((t - 128) >> 8), (t - 128) & 0xFF])
        put(flash, p * PAGE, bytes(b) + b"\xff" * (PAGE - len(b)))
    put(flash, index * PAGE, ix)
    count = sum(len(r) for _, r in pages)
    size = (max(p for p, _ in pages) + 1) * PAGE
    h = (b"IdC\x00" + bytes([1, 48]) + (4).to_bytes(2, "little") + count.to_bytes(4, "little")
         + size.to_bytes(4, "little") + (index * PAGE).to_bytes(4, "little") + len(pages).to_bytes(2, "little")
         + len(words).to_bytes(2, "little") + (table * PAGE).to_bytes(4, "little")
         + (strings * PAGE).to_bytes(4, "little"))
    put(flash, 0, h + b"\xff" * (PAGE - len(h)))


def text_of(toks, words):
    return " ".join(words[t] if isinstance(t, int) else t for t in toks if t is not None)


def against_radio(sim, dump, want, misses, label, look=None):
    """Read the database back with iddb_read through the fast protocol, and look the records (or those in look) up on
    the radio"""
    link = cps_fast.open_link(exec_cmd=f"{sim} --image {dump}")
    try:
        cache = {}

        def get(addr, n):
            out = b""
            while n:
                blk = addr & ~(cps_fast.BLOCK - 1)
                if blk not in cache:
                    cache[blk] = link.read_block(blk)
                k = min(n, blk + cps_fast.BLOCK - addr)
                out += cache[blk][addr - blk:addr - blk + k]
                addr, n = addr + k, n - k
            return out

        db = iddb_read.Database(get)
        info = link.iddb()
        check(info["format"] == db.format and info["records"] == db.count == len(want),
              f"{label}: radio {info}, read {db.format} {db.count}, want {len(want)}")
        got = db.records()
        check(got == want, f"{label}: read back {len(got)} records, first difference "
              f"{next(((g, w) for g, w in zip(got, want) if g != w), None)}")
        runs = db.sample()
        check(runs and all(r and all(x in want for x in r) for r in runs), f"{label}: sample")
        look = want if look is None else look
        bad = [(i, t, r) for i, t in look if (r := link.iddb(i)) != {**info, "found": True, "text": t}]
        check(not bad, f"{label}: {len(bad)} lookups differ, first {bad[:2]}")
        for i in misses:
            check(not link.iddb(i)["found"], f"{label}: {i} not in the database, found")
        print(f"# {label}: {len(want)} records read, {len(look)} looked up, {len(misses)} misses")
    finally:
        link.close()


def main():
    sim = os.path.join(HERE, "cpsSim")
    with tempfile.TemporaryDirectory() as d:
        dump = os.path.join(d, "flash.bin")
        subprocess.run([sim, "--populate", "--dump", dump], stdin=subprocess.DEVNULL, check=True)
        base = bytearray(open(dump, "rb").read())

        # No database
        link = cps_fast.open_link(exec_cmd=f"{sim} --image {dump}")
        check(link.iddb(3100000) == {"format": "", "records": 0, "found": False, "text": None}, "no database")
        link.close()

        # Format N, the last records in area 2
        flash = bytearray(base)
        want = [(3100000 + 3 * i, f"K{i} Name{i % 7}") for i in range(17500)]
        per_area1 = format_n(flash, want)
        check(per_area1 < len(want), "format N reaches area 2")
        open(dump, "wb").write(flash)
        look = want[::101] + want[per_area1 - 2:per_area1 + 2] + want[-2:]      # across the area 1 / area 2 seam
        against_radio(sim, dump, want, [3100001, 3100000 + 3 * 17500, 1], "format N", look)

        # Format C: 200 words (1- and 2-byte codes), literals, empty tokens; the last page in area 2 (page 70)
        flash = bytearray(base)
        words = [f"Word{k}" for k in range(200)]
        pages, n = [], 0
        for p in (4, 5, 70):
            recs = []
            for _ in range(60):
                toks = [f"W{n}A", n % 200, None if n % 5 == 0 else f"City {n % 3}" if n % 2 else (n * 7) % 200]
                recs.append((2000000 + 11 * n, toks))
                n += 1
            pages.append((p, recs))
        format_c(flash, pages, words)
        open(dump, "wb").write(flash)
        want = [(i, text_of(t, words)) for _, recs in pages for i, t in recs]
        against_radio(sim, dump, want, [2000001, 2000000 + 11 * n, 1999999], "format C")

    if failures:
        print(f"iddbLookupClientTest: {failures} failure(s)")
        return 1
    print("iddbLookupClientTest: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
