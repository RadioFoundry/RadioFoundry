# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The DMR ID database on the radio: the text the radio shows for an ID (DL_CMD_IDDB, docs/spec/debuglink.md 3.23)
against the database's own records, read through the fast codeplug protocol (tools/cps_fast.py) and decoded by
tools/iddb_read.py from the specs (open-firmware-cps.md 7.5 format N, iddb-compact.md format C). A few of its blocks
are read (records from the start, middle and end; three record pages of format C).

The database the radio holds is the test data when it is in the format the test needs. When it isn't (or there is
none), the test skips, unless the run passes --allow-codeplug-write: then the blocks a synthetic database of that
format takes (600 made-up records, tools/iddb_write.py, a few blocks at the start of area 1) are backed up to
--codeplug-backup-dir (default the test's tmp directory) as iddb-<format>.bak, the synthetic database is written
over the fast protocol in the order of iddb-compact.md 4.3 (the header block erased first, written last), checked
as above, and the backup written back in a finally block and read back byte for byte. The rest of the radio's
database is never written. Should a run stop before the restore (the process killed, the cable pulled), the radio
shows no database or the synthetic one: `python3 tools/iddb_write.py restore <backup file>` puts the original back."""
import os

import pytest

from rfoundry_hil import skip
import cps_fast
import iddb_read
import iddb_write


def check_database(radio, want_format, pytestconfig, backup_dir):
    if "DMR" not in radio.features():
        skip("not_built", "built without CONFIG_DMR: no ID database")
    if "CPS_FAST" not in radio.features():
        skip("not_built", "built without CONFIG_CPS_FAST: the test reads the database through it")
    info = radio.link.iddb()
    held = f"{'a format ' + info['format'] if info['format'] else 'no'} ID database"
    write = info["format"] != want_format
    if write and not pytestconfig.getoption("--allow-codeplug-write"):
        skip("state", f"the radio holds {held}, this test needs format {want_format}: write one with the web editor, "
                      "or run with --allow-codeplug-write for a synthetic one (the radio's put back after)")
    with radio.released() as port:
        link = cps_fast.open_link(port=port, timeout=3.0)
        try:
            if not write:
                return lookups(link, want_format, info)
            fi = link.info()
            records = iddb_write.synthetic_records()
            encode = iddb_write.encode_n if want_format == "N" else iddb_write.encode_c
            synthetic = encode(records, flash_start=fi["flash_start"])
            if not fi["writable"]:
                skip("state", f"the radio holds {held} and its flash can't be written now (cps_fast INFO)")
            if not all(fi["flash_start"] <= a and a + cps_fast.BLOCK <= fi["end"] for a in synthetic):
                skip("state", f"the radio holds {held}; a synthetic format {want_format} database would reach past "
                              f"its image end {fi['end']:#x}")
            path = os.path.join(backup_dir, f"iddb-{want_format}.bak")
            backup = iddb_write.save(link, synthetic, path)
            print(f"\n# the radio holds {held}: blocks {[hex(a) for a in backup]} backed up to {path}; "
                  f"by hand: python3 tools/iddb_write.py restore {path}")
            try:
                iddb_write.put(link, synthetic)
                got = link.iddb()
                assert got["format"] == want_format and got["records"] == len(records), \
                    f"after writing {len(records)} synthetic records in format {want_format} the radio says {got}"
                lookups(link, want_format, got, want=records)
            finally:
                iddb_write.restore(link, backup)
                after = link.iddb()
                assert (after["format"], after["records"]) == (info["format"], info["records"]), \
                    f"restored (read back byte for byte), but the radio says {after}; before {info}"
                print(f"# restored {len(backup)} blocks, read back byte for byte: {held} again")
        finally:
            link.close()


def lookups(link, want_format, info, want=None):
    """Read the database's start, middle and end and look each record up on the radio, and IDs between them up as
    misses. want: the records written, to hold the whole database read back against"""
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
    if want is not None:
        assert db.records() == want, "the synthetic database read back differs from what was written"
    print(f"\n# format {want_format}: {info['records']} records, {len(records)} looked up, {len(misses)} misses, "
          f"{len(blocks)} blocks read")
    assert not bad, f"{len(bad)} of {len(records)} lookups differ: {bad[:3]}"
    assert not found, f"IDs in no record found: {found}"


@pytest.mark.feature("dmr_iddb")
def test_iddb_lookup_format_n(radio, pytestconfig, backup_dir):
    check_database(radio, "N", pytestconfig, backup_dir)


@pytest.mark.feature("iddb_compact")
def test_iddb_lookup_format_c(radio, pytestconfig, backup_dir):
    if "IDDB_COMPACT" not in radio.features():
        skip("not_built", "built without CONFIG_IDDB_COMPACT")
    check_database(radio, "C", pytestconfig, backup_dir)
