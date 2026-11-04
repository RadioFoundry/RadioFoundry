# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The DMR ID database on the radio: the text the radio shows for an ID (DL_CMD_IDDB, docs/spec/debuglink.md 3.23)
against the database's own records, read through the fast codeplug protocol (tools/cps_fast.py) and decoded by
tools/iddb_read.py from the specs (open-firmware-cps.md 7.5 format N, iddb-compact.md format C). Radio only, read only:
the database the radio already holds is the test data, a few of its blocks read (records from the start, middle and
end; three record pages of format C). Write one with the web editor or a CPS first; the test for the other format
skips."""
import pytest

from rfoundry_hil import skip
import cps_fast
import iddb_read


def check_database(radio, want_format):
    if "CPS_FAST" not in radio.features():
        skip("not_built", "built without CONFIG_CPS_FAST: the test reads the database through it")
    info = radio.link.iddb()
    if info["format"] != want_format:
        skip("state", f"the radio holds {'a format ' + info['format'] if info['format'] else 'no'} ID database, "
                      f"this test needs format {want_format}: write one with the web editor")
    with radio.released() as port:
        link = cps_fast.open_link(port=port, timeout=3.0)
        try:
            blocks = {}

            def get(addr, n):
                out = b""
                while n:
                    blk = addr & ~(cps_fast.BLOCK - 1)
                    if blk not in blocks:
                        blocks[blk] = link.read_block(blk)
                    k = min(n, blk + cps_fast.BLOCK - addr)
                    out += blocks[blk][addr - blk:addr - blk + k]
                    addr, n = addr + k, n - k
                return out

            db = iddb_read.Database(get, flash_start=link.info()["flash_start"])
            assert db.format == want_format and db.count == info["records"], \
                f"read format {db.format} with {db.count} records; the radio says {info}"
            runs = db.sample()
            records = [r for run in runs for r in run]
            assert records, "no records read"
            bad = []
            for dmr_id, text in records:
                got = link.iddb(dmr_id)
                if not got["found"] or got["text"] != text[:48]:
                    bad.append((dmr_id, text, got["text"]))
            # IDs between two records that follow each other in the database: in no record
            misses = [a + 1 for run in runs for (a, _), (b, _) in zip(run, run[1:]) if b - a > 1][:20]
            found = [i for i in misses if link.iddb(i)["found"]]
        finally:
            link.close()
    print(f"\n# format {want_format}: {info['records']} records, {len(records)} looked up, {len(misses)} misses, "
          f"{len(blocks)} blocks read")
    assert not bad, f"{len(bad)} of {len(records)} lookups differ: {bad[:3]}"
    assert not found, f"IDs in no record found: {found}"


@pytest.mark.feature("dmr_iddb")
def test_iddb_lookup_format_n(radio):
    check_database(radio, "N")


@pytest.mark.feature("iddb_compact")
def test_iddb_lookup_format_c(radio):
    if "IDDB_COMPACT" not in radio.features():
        skip("not_built", "built without CONFIG_IDDB_COMPACT")
    check_database(radio, "C")
