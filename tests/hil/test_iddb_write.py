# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""tools/iddb_write.py's encoders against tools/iddb_read.py, no radio: the synthetic database test_iddb.py writes,
in format N and format C, read back record for record from the blocks alone, and the backup file round trip. These
run anywhere, and tools/ci.sh runs them. The radio's own lookup of both is in tests/host/iddbLookupClientTest.py."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import iddb_read  # noqa: E402
import iddb_write  # noqa: E402

BLOCK = iddb_write.BLOCK


def database(blocks, flash_start=iddb_read.FLASH_START):
    def get(addr, n):
        out = b""
        while n:
            blk = addr - addr % BLOCK
            k = min(n, blk + BLOCK - addr)
            out += blocks.get(blk, b"\xff" * BLOCK)[addr - blk:addr - blk + k]
            addr, n = addr + k, n - k
        return out
    return iddb_read.Database(get, flash_start=flash_start)


def test_synthetic_records():
    recs = iddb_write.synthetic_records()
    ids = [i for i, _ in recs]
    assert ids == sorted(set(ids)) and len(recs) == 600 and ids[-1] <= 0xFFFFFF
    assert all(iddb_write.clean(t) == t and len(t) <= 32 for _, t in recs)
    assert any(b - a >= 128 for a, b in zip(ids, ids[1:])), "no two-byte LEB128 gap"


def test_format_n_round_trip():
    recs = iddb_write.synthetic_records()
    for text_len in (16, 32):
        blocks = iddb_write.encode_n(recs, text_len=text_len)
        assert all(len(b) == BLOCK and a % BLOCK == 0 for a, b in blocks.items())
        h = blocks[iddb_write.header_addr()][:12]
        assert h[:3] == b"IdN" and h[3] == 3 + text_len * 3 // 4 + 0x4A and h[4:8] == b"001\x00"
        db = database(blocks)
        assert db.format == "N" and db.count == len(recs)
        assert db.records() == [(i, t[:text_len].rstrip(" ")) for i, t in recs]


def test_format_n_into_area2():
    """Records past area 1's whole records go to the start of area 2"""
    recs = [(100000 + 2 * i, f"K{i}") for i in range(17500)]
    blocks = iddb_write.encode_n(recs, text_len=16)
    assert any(a >= iddb_read.FLASH_START + iddb_read.AREA2_OFF for a in blocks)
    assert database(blocks).records() == recs


def test_format_c_round_trip():
    recs = iddb_write.synthetic_records()
    blocks = iddb_write.encode_c(recs)
    db = database(blocks)
    assert db.format == "C" and db.count == len(recs)
    assert db.records() == recs
    assert db.pages >= 2, "the synthetic database fills more than one record page"
    assert db.dict > 128, "no two-byte dictionary codes"
    assert all(r in recs for run in db.sample() for r in run)
    page0 = blocks[iddb_write.header_addr()]
    assert page0[:5] == b"IdC\x00\x01" and page0[32:] == b"\xff" * (BLOCK - 32)
    assert db.size == max(blocks) + BLOCK - iddb_write.header_addr()


def test_other_flash_start():
    """The layout follows the link's flash_start"""
    recs = iddb_write.synthetic_records(50)
    for enc in (iddb_write.encode_n, iddb_write.encode_c):
        blocks = enc(recs, flash_start=0x40000)
        assert min(blocks) == 0x40000 + iddb_read.AREA1_OFF
        assert database(blocks, flash_start=0x40000).records() == recs


def test_clean_and_prepare():
    assert iddb_write.clean("José  O'Neil, Jr.") == "Jose O Neil Jr."
    got = iddb_write.prepare([(5, "b"), (3, "a"), (5, "dup"), (0, "zero"), (1 << 24, "big")], 48)
    assert got == [(3, "a"), (5, "b")]


def test_backup_file(tmp_path):
    blocks = iddb_write.encode_c(iddb_write.synthetic_records())

    class Link:
        def read_block(self, a):
            return blocks[a]
    path = tmp_path / "iddb.bak"
    assert iddb_write.save(Link(), blocks, path) == blocks
    assert iddb_write.load(path) == blocks
